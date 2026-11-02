// VillageDLSS: a ReShade add-on for Resident Evil Village (RE Engine, DirectX 12) that replaces
// the game's TAA with DLSS (upscaler.cpp). This file: logging, shader ids, resource tracking,
// the overlay, and the frame capture tool used to find TAA (docs/FINDINGS.md).
//
// Frame capture: F10 records two frames of GPU work to VillageDLSS-frame-N.txt next to re8.exe:
//   - every compute dispatch, and every full-screen draw (3 or 4 vertices / 6 indices, one
//     instance), with a stable id of its shader, the viewport, render targets, root constants,
//     root constant buffers and the textures in its descriptor tables by shader register;
//   - other draws only as a count between those passes, plus render target binds, resource
//     state transitions, copies, and which queue each command list ran on.
// TAA is found from this: a pass that reads colour, depth, a two-channel velocity texture and
// last frame's output, and writes this frame's. Shader ids listed in ReShade.ini
// [VillageDLSS] DumpShaders=ps1234abcd,cs... also get their constant buffers dumped (jitter).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.hpp"
#include "version.h"
#include "../third_party/reshade/utils/descriptor_tracking.hpp"

extern "C" __declspec(dllexport) const char* NAME = RD_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION = "DLSS in place of " RD_GAME "'s TAA (in development).";

namespace {

using namespace reshade::api;

constexpr int kCaptureHotkey = VK_F10;
constexpr int kCaptureFrames = 2;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void LogV(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    g_log = _wfopen((g_exeDir + L"VillageDLSS.log").c_str(), L"w");
    Log(RD_PRODUCT " " RD_VERSION " loaded");
}

// ---------------------------------------------------------------- crash catcher
//
// Logs the first access violation / illegal instruction anywhere in the process (module +
// offset and a stack) and writes VillageDLSS-crash.dmp next to the exe. Handled exceptions the
// game expects would also be logged, so only the first few are reported.

HMODULE g_self = nullptr;
void* g_veh = nullptr;
std::atomic<int> g_faults{0};

std::string ModuleOffset(const void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m) {
        char b[32];
        snprintf(b, sizeof(b), "%p", addr);
        return b;
    }
    char name[MAX_PATH] = {};
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    char b[MAX_PATH + 32];
    snprintf(b, sizeof(b), "%s+0x%llx", base ? base + 1 : name,
             static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(addr) - reinterpret_cast<const uint8_t*>(m)));
    return b;
}

typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

struct DumpExceptionInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};

LONG CALLBACK OnException(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409 /* fail fast */)
        return EXCEPTION_CONTINUE_SEARCH;
    // Only faults with this add-on on the stack (frame 0 is this handler, so it's skipped):
    // others, such as REFramework's handled ones at startup, aren't ours.
    void* frames[32];
    const USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    bool ours = false;
    for (USHORT i = 1; i < count && !ours; ++i) {
        HMODULE m = nullptr;
        ours = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCWSTR>(frames[i]), &m) && m == g_self;
    }
    if (!ours) return EXCEPTION_CONTINUE_SEARCH;
    const int n = ++g_faults;
    if (n > 3) return EXCEPTION_CONTINUE_SEARCH;
    void* at = e->ExceptionRecord->ExceptionAddress;
    std::string where = ModuleOffset(at);
    Log("EXCEPTION 0x%08lx at %s (thread %lu), info %llx %llx", code, where.c_str(), GetCurrentThreadId(),
        static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[0]),
        static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]));
    for (USHORT i = 0; i < count; ++i) Log("  stack %2u: %s", i, ModuleOffset(frames[i]).c_str());
    if (n == 1) {
        if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
            auto write = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
            HANDLE f = CreateFileW((g_exeDir + L"VillageDLSS-crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (write && f != INVALID_HANDLE_VALUE) {
                DumpExceptionInfo info = {GetCurrentThreadId(), e, FALSE};
                // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
                write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40 | 0x1000, &info, nullptr, nullptr);
                Log("wrote VillageDLSS-crash.dmp");
            }
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- shader ids
//
// Pipeline handles change every run; a hash of the shader bytecode doesn't, so captures from
// different runs can be compared. Graphics pipelines are named by their pixel shader.

struct PipeIds {
    uint32_t cs = 0, ps = 0, vs = 0;
};

std::mutex g_pipeMutex;
std::unordered_map<uint64_t, PipeIds> g_pipes;
size_t g_computePipes = 0, g_graphicsPipes = 0;

uint32_t Fnv(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The registers a shader declares (from its bytecode), so a capture lists only what each pass
// reads and writes. RE Engine binds one large table (t0..t127) and bindless buffer tables to
// every pass; without this the capture is mostly unused slots.
struct Decl {
    char cls;  // 't', 'u', 'b' or 's'
    uint32_t space, lo, hi;
};

std::unordered_map<uint32_t, std::vector<Decl>> g_decls;  // shader id -> registers (g_pipeMutex)

// DXIL keeps its bindings in the PSV0 part, SM 5.x DXBC in RDEF.
std::vector<Decl> ParseDecls(const uint8_t* p, size_t n) {
    std::vector<Decl> out;
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        if (off + 4 <= n) memcpy(&v, p + off, 4);
        return v;
    };
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + i * 4 + 4 <= n; ++i) {
        const size_t part = u32(32 + i * 4);
        if (part + 8 > n) continue;
        const size_t data = part + 8, size = u32(part + 4);
        if (data + size > n) continue;
        if (memcmp(p + part, "PSV0", 4) == 0) {
            const uint32_t infoSize = u32(data);
            size_t at = data + 4 + infoSize;
            const uint32_t count = u32(at);
            if (!count) continue;
            const uint32_t stride = u32(at + 4);
            at += 8;
            for (uint32_t k = 0; k < count && at + 16 <= data + size; ++k, at += stride) {
                const uint32_t type = u32(at);  // 1 sampler, 2 CBV, 3-5 SRV, 6-9 UAV
                const char cls = type == 1 ? 's' : type == 2 ? 'b' : type <= 5 ? 't' : 'u';
                out.push_back({cls, u32(at + 4), u32(at + 8), u32(at + 12)});
            }
        } else if (memcmp(p + part, "RDEF", 4) == 0) {
            const uint32_t count = u32(data + 8), off = u32(data + 12);
            const uint32_t version = u32(data + 16);
            const bool sm51 = ((version >> 8) & 0xff) == 5 && (version & 0xff) == 1;
            const uint32_t stride = sm51 ? 40 : 32;
            for (uint32_t k = 0; k < count; ++k) {
                const size_t at = data + off + k * stride;
                if (at + stride > data + size) break;
                const uint32_t type = u32(at + 4);  // D3D_SHADER_INPUT_TYPE
                const char cls = type == 0 ? 'b' : type == 3 ? 's' : (type == 4 || type >= 6) && type != 7 ? 'u' : 't';
                const uint32_t lo = u32(at + 20), cnt = u32(at + 24);
                out.push_back({cls, sm51 ? u32(at + 32) : 0, lo, cnt ? lo + cnt - 1 : UINT32_MAX});
            }
        }
    }
    return out;
}

std::unordered_set<uint32_t> g_dumpPs, g_dumpCs, g_dumpVs;  // from ReShade.ini [VillageDLSS] DumpShaders

// Shaders in the dump list also get their bytecode saved as VillageDLSS-<id>.cso (once a run).
void SaveBytecode(const char* prefix, uint32_t id, const void* code, size_t size) {
    static std::mutex m;
    static std::unordered_set<uint32_t> saved;
    std::lock_guard<std::mutex> lock(m);
    if (!saved.insert(id).second) return;
    wchar_t name[64];
    swprintf(name, 64, L"VillageDLSS-%hs%08x.cso", prefix, id);
    if (FILE* f = _wfopen((g_exeDir + name).c_str(), L"wb")) {
        fwrite(code, 1, size, f);
        fclose(f);
        Log("saved %s%08x bytecode (%zu bytes)", prefix, id, size);
    }
}

uint32_t ShaderHash(const pipeline_subobject& s) {
    if (!s.data) return 0;
    const shader_desc& d = *static_cast<const shader_desc*>(s.data);
    if (!d.code || !d.code_size) return 0;
    const uint32_t id = Fnv(d.code, d.code_size);
    if (s.type == pipeline_subobject_type::pixel_shader && g_dumpPs.count(id)) SaveBytecode("ps", id, d.code, d.code_size);
    if (s.type == pipeline_subobject_type::compute_shader && g_dumpCs.count(id)) SaveBytecode("cs", id, d.code, d.code_size);
    if (s.type == pipeline_subobject_type::vertex_shader && g_dumpVs.count(id)) SaveBytecode("vs", id, d.code, d.code_size);
    std::vector<Decl> decls = ParseDecls(static_cast<const uint8_t*>(d.code), d.code_size);
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_decls[id] = std::move(decls);
    return id;
}

const std::vector<Decl>* Decls(uint32_t id) {  // the vectors are never erased, so the pointer stays valid
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_decls.find(id);
    return it == g_decls.end() || it->second.empty() ? nullptr : &it->second;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipe) {
    PipeIds ids;
    for (uint32_t i = 0; i < count; ++i) {
        switch (subobjects[i].type) {
            case pipeline_subobject_type::compute_shader: ids.cs = ShaderHash(subobjects[i]); break;
            case pipeline_subobject_type::pixel_shader: ids.ps = ShaderHash(subobjects[i]); break;
            case pipeline_subobject_type::vertex_shader: ids.vs = ShaderHash(subobjects[i]); break;
            default: break;
        }
    }
    static std::atomic<int> logged{0};
    if (logged < 4 && (ids.cs || ids.ps)) {
        ++logged;
        const uint32_t id = ids.cs ? ids.cs : ids.ps;
        const std::vector<Decl>* d = Decls(id);
        Log("pipeline %s%08x: %zu declared bindings", ids.cs ? "cs" : "ps", id, d ? d->size() : 0);
    }
    if (!ids.cs && !ids.ps && !ids.vs) return;
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_pipes[pipe.handle] = ids;
    ++(ids.cs ? g_computePipes : g_graphicsPipes);
}

void OnDestroyPipeline(device*, pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    g_pipes.erase(pipe.handle);
}

PipeIds Pipe(pipeline pipe) {
    std::lock_guard<std::mutex> lock(g_pipeMutex);
    auto it = g_pipes.find(pipe.handle);
    return it == g_pipes.end() ? PipeIds{} : it->second;
}

// Fills g_dumpPs / g_dumpCs: shaders whose constant buffers (and bytecode) get dumped.

void LoadDumpList() {
    char buf[2048] = {};
    size_t size = sizeof(buf);
    if (!reshade::get_config_value(nullptr, RD_PRODUCT, "DumpShaders", buf, &size)) return;
    std::string s(buf, size && size <= sizeof(buf) ? size : strlen(buf));  // ReShade returns a list split by NULs
    for (char& c : s)
        if (c == ' ' || c == ';' || c == '\0') c = ',';
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string t = s.substr(pos, end - pos);
        pos = end + 1;
        if (t.size() != 10) continue;
        const uint32_t id = static_cast<uint32_t>(strtoul(t.c_str() + 2, nullptr, 16));
        if (t.compare(0, 2, "ps") == 0) g_dumpPs.insert(id);
        else if (t.compare(0, 2, "cs") == 0) g_dumpCs.insert(id);
        else if (t.compare(0, 2, "vs") == 0) g_dumpVs.insert(id);  // bytecode only
    }
    Log("dumping %zu pixel, %zu compute and %zu vertex shaders", g_dumpPs.size(), g_dumpCs.size(), g_dumpVs.size());
}

// ---------------------------------------------------------------- resource descriptions

const char* FormatName(format f) {
    switch (static_cast<uint32_t>(f)) {
        case 2: return "rgba32f";
        case 10: return "rgba16f";
        case 11: return "rgba16unorm";
        case 13: return "rgba16snorm";
        case 16: return "rg32f";
        case 19: return "r32g8x24_typeless";
        case 20: return "d32s8";
        case 23: return "rgb10a2_typeless";
        case 24: return "rgb10a2unorm";
        case 25: return "rgb10a2uint";
        case 26: return "r11g11b10f";
        case 27: return "rgba8_typeless";
        case 28: return "rgba8unorm";
        case 29: return "rgba8srgb";
        case 33: return "rg16_typeless";
        case 34: return "rg16f";
        case 35: return "rg16unorm";
        case 36: return "rg16uint";
        case 37: return "rg16snorm";
        case 39: return "r32_typeless";
        case 40: return "d32f";
        case 41: return "r32f";
        case 42: return "r32uint";
        case 44: return "r24g8_typeless";
        case 45: return "d24s8";
        case 46: return "r24x8";
        case 49: return "rg8unorm";
        case 50: return "rg8uint";
        case 51: return "rg8snorm";
        case 53: return "r16_typeless";
        case 54: return "r16f";
        case 55: return "d16";
        case 56: return "r16unorm";
        case 57: return "r16uint";
        case 61: return "r8unorm";
        case 62: return "r8uint";
        case 87: return "bgra8unorm";
        default: return nullptr;
    }
}

// Live resources and views, recorded as the game creates and destroys them. Descriptor tracking
// can return views of resources that have since been freed; asking ReShade about those reads
// freed memory (0.0.2 crashed), so descriptions come from these maps instead.

struct ViewInfo {
    resource res;
    resource_view_desc desc;
};

std::mutex g_resMutex;
std::unordered_map<uint64_t, resource_desc> g_resources;
std::unordered_map<uint64_t, ViewInfo> g_views;

void OnInitResource(device*, const resource_desc& desc, const subresource_data*, resource_usage, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources[r.handle] = desc;
}

void OnDestroyResource(device*, resource r) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_resources.erase(r.handle);
}

void OnInitView(device*, resource r, resource_usage, const resource_view_desc& desc, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views[v.handle] = {r, desc};
}

void OnDestroyView(device*, resource_view v) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    g_views.erase(v.handle);
}

bool FindView(resource_view v, ViewInfo* out) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    auto it = g_views.find(v.handle);
    if (it == g_views.end()) return false;
    *out = it->second;
    return true;
}

// live: the resource is in use by the call being recorded, so ReShade may be asked about it
// when it isn't in the map (swap chain buffers).
std::string Describe(device* dev, resource r, bool live = true) {
    if (!r.handle) return "null";
    std::string id = Format("#%05llx", (r.handle >> 4) & 0xfffff);  // short, enough to follow one resource
    resource_desc d;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        auto it = g_resources.find(r.handle);
        if (it != g_resources.end()) d = it->second;
        else if (!live) return id + " (freed)";
        else d = dev->get_resource_desc(r);
    }
    if (d.type == resource_type::buffer) return id + Format(" buf %llu", d.buffer.size);
    const char* fn = FormatName(d.texture.format);
    std::string fmt = fn ? fn : Format("fmt%u", static_cast<uint32_t>(d.texture.format));
    std::string s = id + Format(" tex %ux%u %s", d.texture.width, d.texture.height, fmt.c_str());
    if (d.texture.depth_or_layers > 1) s += Format(" x%u", d.texture.depth_or_layers);
    if (d.texture.levels > 1) s += Format(" mips%u", d.texture.levels);
    return s;
}

const char* UsageName(resource_usage u) {
    switch (u) {
        case resource_usage::undefined: return "undef";
        case resource_usage::general: return "general";
        case resource_usage::depth_stencil: return "ds";
        case resource_usage::indirect_argument: return "indirect";
        case resource_usage::constant_buffer: return "cbv";
        case resource_usage::render_target: return "rt";
        case resource_usage::depth_stencil_write: return "dsv";
        case resource_usage::depth_stencil_read: return "dsv_ro";
        case resource_usage::unordered_access: return "uav";
        case resource_usage::shader_resource_pixel: return "srv_ps";
        case resource_usage::shader_resource_non_pixel: return "srv_cs";
        case resource_usage::shader_resource: return "srv";
        case resource_usage::copy_dest: return "copy_dst";
        case resource_usage::copy_source: return "copy_src";
        case resource_usage::present: return "present";
        default: return nullptr;
    }
}

std::string Usage(resource_usage u) {
    const char* n = UsageName(u);
    return n ? n : Format("0x%x", static_cast<uint32_t>(u));
}

// ---------------------------------------------------------------- capture state

enum class Capture { Idle, Recording, Writing };
std::atomic<Capture> g_capture{Capture::Idle};
int g_framesLeft = 0;
int g_captureCount = 0;
std::string g_status;

// What one pipeline stage (graphics or compute) of a command list has bound right now.
struct Bindings {
    pipeline_layout layout = {0};
    std::map<uint32_t, uint64_t> tables;                   // root param -> descriptor table
    std::map<uint32_t, buffer_range> cbvs;                 // root param -> root constant buffer
    std::map<uint32_t, std::vector<uint32_t>> constants;   // root param -> root constants
    void Use(pipeline_layout l) {
        if (l.handle == layout.handle) return;
        layout = l;  // a new root signature: earlier root arguments no longer apply
        tables.clear();
        cbvs.clear();
        constants.clear();
    }
};

struct ListLog {
    std::vector<std::string> lines;
    bool partial = false;  // reset before the capture began: its first events are missing
    uint32_t draws = 0;    // ordinary draws not yet written
    PipeIds gfxPipe, csPipe;
    Bindings gfx, cs;
    viewport vp = {};
    std::string rts;
};

std::mutex g_capMutex;
std::unordered_map<uint64_t, ListLog> g_lists;  // command list -> events since its reset
std::vector<std::string> g_frame;                // executed lists in order, with frame boundaries
std::unordered_map<uint64_t, uint64_t> g_listIds;
uint64_t g_nextListId = 1;
size_t g_passes = 0;

bool Recording() { return g_capture.load(std::memory_order_relaxed) == Capture::Recording; }

ListLog& List(command_list* cl) {  // g_capMutex held
    auto it = g_lists.find(cl->get_native());
    if (it != g_lists.end()) return it->second;
    ListLog& l = g_lists[cl->get_native()];
    l.partial = true;
    return l;
}

void FlushDraws(ListLog& l) {
    if (!l.draws) return;
    l.lines.push_back(Format("  draw x%u", l.draws));
    l.draws = 0;
}

void AddLocked(ListLog& l, std::string line) {
    FlushDraws(l);
    l.lines.push_back("  " + std::move(line));
}

void Add(command_list* cl, std::string line) {
    std::lock_guard<std::mutex> lock(g_capMutex);
    AddLocked(List(cl), std::move(line));
}

// ---------------------------------------------------------------- pass details

std::string Floats(const uint32_t* v, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        float f;
        memcpy(&f, &v[k], 4);
        const uint32_t e = (v[k] >> 23) & 0xff;
        if (v[k] == 0) s += "          0";
        else if (e > 90 && e < 165) s += Format(" %10.6g", f);  // a plausible float
        else s += Format("  0x%08x", v[k]);
    }
    return s;
}

// A constant buffer's contents as 32-bit values (float where plausible, else hex), up to 4 KB.
void DumpBuffer(device* dev, ListLog& l, const char* what, const buffer_range& b) {
    const resource_desc d = dev->get_resource_desc(b.buffer);
    if (d.type != resource_type::buffer || b.offset >= d.buffer.size) return;
    uint64_t size = d.buffer.size - b.offset;
    if (b.size != UINT64_MAX && b.size && b.size < size) size = b.size;
    if (size > 4096) size = 4096;
    void* data = nullptr;
    if (!dev->map_buffer_region(b.buffer, b.offset, size, map_access::read_only, &data) || !data) {
        AddLocked(l, Format("    %s: can't read (heap %u)", what, static_cast<uint32_t>(d.heap)));
        return;
    }
    std::vector<uint32_t> v(static_cast<size_t>(size / 4));
    memcpy(v.data(), data, v.size() * 4);
    dev->unmap_buffer_region(b.buffer);
    AddLocked(l, Format("    %s: %zu bytes", what, v.size() * 4));
    for (size_t i = 0; i < v.size(); i += 4)
        AddLocked(l, Format("      c %4zu:", i * 4) + Floats(&v[i], v.size() - i < 4 ? v.size() - i : 4));
}

char RegisterLetter(descriptor_type t) {
    switch (t) {
        case descriptor_type::sampler: return 's';
        case descriptor_type::constant_buffer: return 'b';
        case descriptor_type::buffer_unordered_access_view:
        case descriptor_type::texture_unordered_access_view: return 'u';
        default: return 't';
    }
}

// Everything bound to one stage, by shader register: tables resolved through ReShade's
// descriptor tracking, root constant buffers, root constants.
bool Declared(const std::vector<Decl>* decls, char cls, uint32_t space, uint32_t reg) {
    if (!decls) return true;  // shader unknown: list everything
    for (const Decl& d : *decls)
        if (d.cls == cls && d.space == space && reg >= d.lo && reg <= d.hi) return true;
    return false;
}

// Everything bound to one stage that the shader declares, by shader register: tables resolved
// through ReShade's descriptor tracking, root constant buffers, root constants. Without the
// shader's declarations, the first 16 slots of each range.
void WriteBindings(device* dev, ListLog& l, const Bindings& b, bool dump, const std::vector<Decl>* decls) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!decls) AddLocked(l, "    (shader declarations unknown: all slots shown)");
    for (const auto& [param, table] : b.tables) {
        const pipeline_layout_param p = dt ? dt->get_pipeline_layout_param(b.layout, param) : pipeline_layout_param{};
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) {
            AddLocked(l, Format("    table param %u: layout unknown (type %u)", param, static_cast<uint32_t>(p.type)));
            continue;
        }
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            if (range.type == descriptor_type::sampler) continue;
            const char cls = RegisterLetter(range.type);
            const bool unbounded = range.count == UINT32_MAX || range.count > 256;
            uint32_t n = decls ? (unbounded ? 256 : range.count) : (range.count > 16 ? 16 : range.count);
            uint32_t shown = 0;
            for (uint32_t k = 0; k < n; ++k) {
                if (!Declared(decls, cls, range.dx_register_space, range.dx_register_index + k)) continue;
                if (++shown > 24) {  // bindless arrays: a few are enough
                    AddLocked(l, Format("    table param %u: %c space%u: more declared slots not shown", param, cls,
                                        range.dx_register_space));
                    break;
                }
                descriptor_heap heap = {0};
                uint32_t offset = 0;
                dev->get_descriptor_heap_offset(descriptor_table{table}, range.binding + k, 0, &heap, &offset);
                std::string what;
                if (range.type == descriptor_type::constant_buffer) {
                    const buffer_range cb = dt->get_buffer_range(heap, offset);
                    if (!cb.buffer.handle) continue;
                    what = Format("cbv %s +%llu", Describe(dev, cb.buffer, false).c_str(), cb.offset);
                    AddLocked(l, Format("    table param %u: b%u space%u = %s", param, range.dx_register_index + k,
                                        range.dx_register_space, what.c_str()));
                    if (dump && what.find("(freed)") == std::string::npos)
                        DumpBuffer(dev, l, Format("b%u", range.dx_register_index + k).c_str(), cb);
                    continue;
                }
                const resource_view v = dt->get_resource_view(heap, offset);
                if (!v.handle) continue;
                ViewInfo info;
                if (!FindView(v, &info)) {
                    AddLocked(l, Format("    table param %u: %c%u space%u = view %llx (unknown)", param,
                                        RegisterLetter(range.type), range.dx_register_index + k, range.dx_register_space,
                                        v.handle));
                    continue;
                }
                const resource_view_desc& vd = info.desc;
                what = Describe(dev, info.res, false);
                if (const char* fn = FormatName(vd.format)) what += Format(" as %s", fn);
                if (vd.type == resource_view_type::texture_2d && vd.texture.first_level)
                    what += Format(" mip%u", vd.texture.first_level);
                AddLocked(l, Format("    table param %u: %c%u space%u = %s", param, RegisterLetter(range.type),
                                    range.dx_register_index + k, range.dx_register_space, what.c_str()));
            }
        }
    }
    for (const auto& [param, cb] : b.cbvs) {
        const pipeline_layout_param p = dt ? dt->get_pipeline_layout_param(b.layout, param) : pipeline_layout_param{};
        std::string reg = "b?";
        if (p.type == pipeline_layout_param_type::push_descriptors) {
            if (!Declared(decls, 'b', p.push_descriptors.dx_register_space, p.push_descriptors.dx_register_index)) continue;
            reg = Format("b%u space%u", p.push_descriptors.dx_register_index, p.push_descriptors.dx_register_space);
        }
        AddLocked(l, Format("    root cbv param %u (%s): %s +%llu", param, reg.c_str(), Describe(dev, cb.buffer).c_str(),
                            cb.offset));
        if (dump) DumpBuffer(dev, l, reg.c_str(), cb);
    }
    for (const auto& [param, v] : b.constants) {
        const pipeline_layout_param p = dt ? dt->get_pipeline_layout_param(b.layout, param) : pipeline_layout_param{};
        std::string reg = "b?";
        if (p.type == pipeline_layout_param_type::push_constants) {
            if (!Declared(decls, 'b', p.push_constants.dx_register_space, p.push_constants.dx_register_index)) continue;
            reg = Format("b%u space%u", p.push_constants.dx_register_index, p.push_constants.dx_register_space);
        }
        AddLocked(l, Format("    root constants param %u (%s):", param, reg.c_str()) +
                         Floats(v.data(), v.size() < 32 ? v.size() : 32));
    }
}

// ---------------------------------------------------------------- events

void OnReset(command_list* cl) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    g_lists[cl->get_native()] = {};
}

void OnExecute(command_queue* q, command_list* cl) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    auto it = g_lists.find(cl->get_native());
    uint64_t& id = g_listIds[cl->get_native()];
    if (!id) id = g_nextListId++;
    const command_queue_type t = q->get_type();
    const char* qn = (t & command_queue_type::graphics) != 0  ? "graphics"
                     : (t & command_queue_type::compute) != 0 ? "compute"
                                                              : "copy";
    if (it == g_lists.end()) {
        g_frame.push_back(Format("EXECUTE list L%llu on %s queue %p (not recorded)", id, qn, q));
        return;
    }
    FlushDraws(it->second);
    g_frame.push_back(Format("EXECUTE list L%llu on %s queue %p, %zu events%s", id, qn, q, it->second.lines.size(),
                             it->second.partial ? " (started before the capture: partial)" : ""));
    for (std::string& s : it->second.lines) g_frame.push_back(std::move(s));
    g_lists.erase(it);
}

void OnBarrier(command_list* cl, uint32_t count, const resource* res, const resource_usage* from, const resource_usage* to) {
    if (!Recording()) return;
    device* dev = cl->get_device();
    for (uint32_t i = 0; i < count; ++i)
        Add(cl, Format("barrier %s: %s -> %s", Describe(dev, res[i]).c_str(), Usage(from[i]).c_str(), Usage(to[i]).c_str()));
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if (!Recording()) return;
    const PipeIds ids = Pipe(pipe);
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    // D3D12 binds every pipeline with stages = all, so the shaders it has decide which it is.
    if (ids.cs) l.csPipe = ids;
    else if (ids.ps || ids.vs) l.gfxPipe = ids;
    else if (stages == pipeline_stage::compute_shader) l.csPipe = ids;
    else l.gfxPipe = ids;
}

void OnBindViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    if (!Recording() || first != 0 || !count) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    List(cl).vp = vps[0];
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (!Recording()) return;
    device* dev = cl->get_device();
    auto view = [dev](resource_view v) {
        ViewInfo info;
        return FindView(v, &info) ? Describe(dev, info.res) : Describe(dev, dev->get_resource_from_view(v));
    };
    std::string s;
    for (uint32_t i = 0; i < count; ++i) s += " " + (rtvs[i].handle ? view(rtvs[i]) : std::string("null"));
    if (dsv.handle) s += " depth " + view(dsv);
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    l.rts = s;
    AddLocked(l, "render targets:" + s);
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    Bindings& b = (stages & shader_stage::compute) != 0 ? l.cs : l.gfx;
    b.Use(layout);
    for (uint32_t i = 0; i < count; ++i) b.tables[first + i] = tables[i].handle;
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    Bindings& b = (stages & shader_stage::compute) != 0 ? l.cs : l.gfx;
    b.Use(layout);
    if (u.type == descriptor_type::constant_buffer && u.count)
        b.cbvs[param] = static_cast<const buffer_range*>(u.descriptors)[0];
    else
        AddLocked(l, Format("push %s param %u type %u (root view)", (stages & shader_stage::compute) != 0 ? "cs" : "gfx",
                            param, static_cast<uint32_t>(u.type)));
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    if (!Recording()) return;
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    Bindings& b = (stages & shader_stage::compute) != 0 ? l.cs : l.gfx;
    b.Use(layout);
    std::vector<uint32_t>& v = b.constants[param];
    if (v.size() < first + count) v.resize(first + count);
    memcpy(v.data() + first, values, count * 4);
}

void Pass(command_list* cl, bool compute, const std::string& head) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_capMutex);
    ListLog& l = List(cl);
    ++g_passes;
    if (compute) {
        AddLocked(l, Format("DISPATCH cs%08x ", l.csPipe.cs) + head);
        WriteBindings(dev, l, l.cs, g_dumpCs.count(l.csPipe.cs) != 0, Decls(l.csPipe.cs));
    } else {
        AddLocked(l, Format("FULLSCREEN ps%08x vs%08x ", l.gfxPipe.ps, l.gfxPipe.vs) + head +
                         Format(" viewport %gx%g at %g,%g ->%s", l.vp.width, l.vp.height, l.vp.x, l.vp.y, l.rts.c_str()));
        WriteBindings(dev, l, l.gfx, g_dumpPs.count(l.gfxPipe.ps) != 0, Decls(l.gfxPipe.ps));
    }
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (Recording()) Pass(cl, true, Format("%u x %u x %u", x, y, z));
    return false;
}

bool OnDraw(command_list* cl, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    const bool replaced = rd::UpscalerOnDraw(cl, vertices, instances);
    if (!Recording()) return replaced;
    if (vertices <= 4 && instances == 1) {
        Pass(cl, false, Format("draw %u verts%s", vertices, replaced ? " (replaced by DLSS)" : ""));
        return replaced;
    }
    std::lock_guard<std::mutex> lock(g_capMutex);
    ++List(cl).draws;
    return false;
}

bool OnDrawIndexed(command_list* cl, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    if (!Recording()) return false;
    if (indices <= 6 && instances == 1) {
        Pass(cl, false, Format("draw %u indices", indices));
        return false;
    }
    std::lock_guard<std::mutex> lock(g_capMutex);
    ++List(cl).draws;
    return false;
}

bool OnDispatchMesh(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    if (Recording()) Add(cl, Format("dispatch_mesh %u x %u x %u", x, y, z));
    return false;
}

bool OnExecuteIndirect(command_list* cl, indirect_command type, resource, uint64_t, uint32_t count, uint32_t) {
    if (!Recording()) return false;
    if (type == indirect_command::dispatch) Pass(cl, true, Format("indirect x%u", count));
    else Add(cl, Format("indirect type %u count %u", static_cast<uint32_t>(type), count));
    return false;
}

bool OnCopyResource(command_list* cl, resource src, resource dst) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("copy %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

bool OnCopyTexture(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                   const subresource_box*, filter_mode) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("copy region %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

bool OnResolve(command_list* cl, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t,
               uint32_t, uint32_t, format) {
    if (Recording()) {
        device* dev = cl->get_device();
        Add(cl, Format("resolve %s -> %s", Describe(dev, src).c_str(), Describe(dev, dst).c_str()));
    }
    return false;
}

// ---------------------------------------------------------------- capture control

void WriteCapture() {
    std::vector<std::string> frame;
    size_t passes;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        frame.swap(g_frame);
        g_lists.clear();
        passes = g_passes;
    }
    ++g_captureCount;
    std::wstring name = g_exeDir + L"VillageDLSS-frame-" + std::to_wstring(g_captureCount) + L".txt";
    FILE* f = _wfopen(name.c_str(), L"w");
    if (!f) {
        g_status = "couldn't write the capture file";
        return;
    }
    for (const std::string& s : frame) {
        fputs(s.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
    g_status = Format("Capture %d: %zu lines, %zu passes -> VillageDLSS-frame-%d.txt", g_captureCount, frame.size(),
                      passes, g_captureCount);
    Log("%s", g_status.c_str());
}

void StartCapture() {
    if (g_capture.load() != Capture::Idle) return;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        g_lists.clear();
        g_frame.clear();
        g_passes = 0;
        g_frame.push_back("==== capture start (lists reset before this point are marked partial)");
    }
    g_framesLeft = kCaptureFrames;
    g_capture = Capture::Recording;
    Log("capture started");
}

bool g_keyWasDown = false;
bool g_captureHotkey = false;  // F10, off unless ReShade.ini [VillageDLSS] CaptureHotkey=1

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (g_capture.load() == Capture::Recording) {
        resource back = sc->get_current_back_buffer();
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_frame.push_back(Format("==== PRESENT (back buffer %s, colour space %u), %d more",
                                     Describe(sc->get_device(), back).c_str(),
                                     static_cast<uint32_t>(sc->get_color_space()), g_framesLeft - 1));
        }
        if (--g_framesLeft <= 0) {
            g_capture = Capture::Writing;
            WriteCapture();
            g_capture = Capture::Idle;
        }
    }
    if (g_captureHotkey) {  // development only: ReShade.ini [VillageDLSS] CaptureHotkey=1
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kCaptureHotkey) & 0x8000);
        if (down && !g_keyWasDown) StartCapture();
        g_keyWasDown = down;
    }
    rd::UpscalerPresent(sc);
}

// ---------------------------------------------------------------- overlay

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(RD_PRODUCT " " RD_VERSION);
    rd::UpscalerOverlay();
    if (!ImGui::TreeNode("Development: frame capture")) return;
    ImGui::TextDisabled("Writes two frames of GPU work to VillageDLSS-frame-N.txt (for finding passes).");
    ImGui::BeginDisabled(g_capture.load() != Capture::Idle);
    if (ImGui::Button(g_captureHotkey ? "Capture frame (F10)" : "Capture frame")) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_capture.load() == Capture::Recording ? "recording..." : g_status.c_str());
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        ImGui::TextDisabled("%zu compute and %zu graphics pipelines known", g_computePipes, g_graphicsPipes);
    }
    ImGui::TreePop();
}

}  // namespace

namespace rd {

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

std::wstring ExeDir() { return g_exeDir; }

uint32_t PipelinePs(pipeline pipe) {
    const PipeIds ids = Pipe(pipe);
    return ids.ps ? ids.ps : ids.cs;
}

resource ViewResource(resource_view view) {
    ViewInfo info;
    return FindView(view, &info) ? info.res : resource{0};
}

}  // namespace rd

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            OpenLog();
            g_self = module;
            g_veh = AddVectoredExceptionHandler(1, &OnException);
            LoadDumpList();
            reshade::get_config_value(nullptr, RD_PRODUCT, "CaptureHotkey", g_captureHotkey);
            descriptor_tracking::register_events();
            reshade::register_event<reshade::addon_event::init_resource>(&OnInitResource);
            reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyResource);
            reshade::register_event<reshade::addon_event::init_resource_view>(&OnInitView);
            reshade::register_event<reshade::addon_event::destroy_resource_view>(&OnDestroyView);
            reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
            reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
            reshade::register_event<reshade::addon_event::reset_command_list>(&OnReset);
            reshade::register_event<reshade::addon_event::execute_command_list>(&OnExecute);
            reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
            reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
            reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
            reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::dispatch_mesh>(&OnDispatchMesh);
            reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnExecuteIndirect);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyTexture);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(RD_PRODUCT, &DrawOverlay);
            rd::UpscalerInit();
            break;
        case DLL_PROCESS_DETACH:
            rd::UpscalerShutdown();
            if (g_veh) RemoveVectoredExceptionHandler(g_veh);
            descriptor_tracking::unregister_events();
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
