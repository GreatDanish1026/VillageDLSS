// DLSS or AMD FSR 3.1 / FSR 4 in place of the game's TAA, and frame generation (docs/FINDINGS.md,
// docs/FRAMEGEN.md).
//
// Resident Evil Village (the first of these add-ons; RE2DLSS, RE3DLSS and VillageDLSS are forks).
//
// The TAA pass (PreTonemap2_PS) is a full-screen draw reading depth (reverse-Z), the scene colour
// before exposure, last frame's TAA output and the velocity (NDC offset from the current to the
// previous position, y up), with the camera constants (SceneInfo) in root CBV b0. DLSS takes the
// same inputs:
//   - motion vectors: the game's velocity texture as it is, scaled by (0.5 w, -0.5 h) to pixels;
//   - jitter: SceneInfo.projElement[0].zw (clip space) times half the render size.
//
// DLAA (render size = TAA target size, Image Quality 1.0 or above): the TAA draw is skipped and
// DLSS's output is copied into the TAA target. Everything after it runs as the game does.
//
// Upscaling (render size below the output size): everything after TAA runs at render size, and a
// full-screen pass (CopyImageWithDepthPS) stretches it to the output and writes a full-size depth.
// So:
//   1. TAA: DLSS writes an output-size image (X). The game's TAA still runs at render size, so
//      the passes after it at render size and the TAA history stay as they are.
//   2. Bloom composite (NewFinal, additive): the game's draw goes into X.
//   3. Tonemap (LDRPostProcess_WithTonemap): reads t1 by pixel position and SceneInfo's screen
//      size: the game's draw goes into Y at output size, t1 rewritten to X in its descriptor
//      table, SceneInfo replaced by a copy with the output size.
//   4. Sharpening (Sharpness_PS, the game's setting): the game's draw goes into Z from Y.
//   5. CopyImageWithDepthPS: the game's draw (for its depth), then the final image is drawn over
//      its colour (our pass converts formats).
// The HUD and the final output pass (and RenoDX) follow as usual. (Up to 1.0.1 the bloom, tonemap
// and sharpening were drawn a second time by the add-on; now the game's own draws are redirected,
// as in RE2DLSS/RE3DLSS/VillageDLSS.)
//
// FSR 3.1/4 (AMD's FidelityFX runtime) takes the same inputs into the same output (X), plus the
// camera's near plane and field of view from SceneInfo.
//
// Frame generation (framegen.cpp) gets the TAA's depth and velocity and SceneInfo at the TAA draw,
// whoever upscales, and the picture before the HUD: the HUD is drawn into the output-size texture
// that the output pass writes to the back buffer. That texture is copied ("snapshot") once the
// scene is in it (after the game's CAS_CS or our blit into it, else just before the first HUD draw);
// after the output pass, the snapshot goes back into it and the output pass is repeated into a
// target of ours (with all the game's and RenoDX's state): the screen without the HUD.
//
// NGX and FFX record their own compute state and descriptor heaps on the game's command list, so
// the game's bindings are tracked per command list and put back afterwards (as in ReunionDLSS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "blit_ps.h"  // kBlitPs, kBlitVs: blit.hlsl compiled by build.ps1
#include "blit_vs.h"
#include "dlss.hpp"
#include "fsr.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

// Resident Evil Village's shaders (docs/FINDINGS.md).
constexpr uint32_t kTaaPs = 0xcf113c28;      // PreTonemap2_PS: t0 depth, t1 colour, t2 history, t3 velocity
constexpr uint32_t kTaaDepth = 0, kTaaColor = 1, kTaaMotion = 3;
constexpr uint32_t kBloomPs = 0x77ae4c63;    // NewFinal
constexpr bool kBloomIntoTaaOnly = false;     // (RE7's bloom shader also draws its smaller levels)
constexpr uint32_t kTonemapPs = 0x3cc6333b;  // LDRPostProcess_WithTonemap (t0 depth, colour t1)
constexpr uint32_t kTonemapColor = 1;
constexpr uint32_t kCopyPs = 0x48155d5e;     // CopyImageWithDepthPS: stretch to output size + depth
constexpr uint32_t kSharpenPs = 0x8195a264;  // Sharpness_PS (t0, the game's sharpness setting)
constexpr bool kSharpenOptional = true;
constexpr uint32_t kFinalCs = 3;              // CAS_CS: not used in Village (the stretch is CopyImageWithDepthPS)
constexpr uint32_t kOutputPs = 0xaf8e9bff;   // the HUD texture (t0) -> back buffer (RenoDX's HDR output)
constexpr int kToggleHotkey = VK_F9;

// SceneInfo (cb0 of the TAA and tonemap shaders), as float indices.
constexpr int kViewProj = 0;           // row_major float4x4, byte 0
constexpr int kProjElement0 = 40;      // byte 160: x scale, y scale, jitter x, jitter y (clip space)
constexpr int kPrevViewProj = 72;      // byte 288
constexpr int kScreenSize = 92;        // byte 368: width, height, 1 / width, 1 / height
constexpr int kNearPlane = 98;         // byte 392: cameraNearPlane
constexpr int kSceneInfoFloats = 132;  // 528 bytes
constexpr uint32_t kSceneInfoSlot = 768;  // 256-byte aligned
constexpr uint32_t kSceneInfoSlots = 192;

// ---------------------------------------------------------------- settings

enum class Tech { Auto = 0, Dlss = 1, Fsr = 2 };

struct Settings {
    bool enabled = true;   // F9; remembered across launches (on for a new install)
    int upscaler = 0;      // Tech: Auto = DLSS where NGX runs (RTX), else FSR
    int preset = dlss::kPresetAuto;  // DLSS model: Automatic, or NVSDK_NGX_DLSS_Hint_Render_Preset_* (0 = NVIDIA's)
    bool flipJitterX = false, flipJitterY = false;  // testing: jitter sign
    bool upscale = true;   // below the output size: DLSS to output size (else DLAA at render size)
    bool sharpen = true;   // the game's sharpening at output size when upscaling
    bool textureDetail = true;  // negative mip bias on material samplers (applied at game start)
    int mode = 0;          // DLSS mode setting the render scale through VillageDLSS.lua (0: the game's slider)
} g_set;

struct Mode {
    const char* name;
    float scale;
};
const Mode kModes[] = {{"Game's Image Quality slider", 0},  {"Native (100%): DLAA / FSR native AA", 1.0f},
                       {"Quality (67%)", 0.667f},          {"Balanced (58%)", 0.58f},
                       {"Performance (50%)", 0.5f},        {"Ultra Performance (33%)", 0.333f}};
constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Sharpen", g_set.sharpen);
    reshade::get_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_set.textureDetail);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
    if (g_set.mode < 0 || g_set.mode >= kModeCount) g_set.mode = 0;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Sharpen", g_set.sharpen);
    reshade::set_config_value(nullptr, RD_PRODUCT, "TextureDetail", g_set.textureDetail);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
}

std::mutex g_statusMutex;
std::string g_lastError;  // why the last TAA pass wasn't replaced
std::string g_mode;       // "DLSS DLAA 3840x2160" / "FSR upscaling 1920x1080 -> 3840x2160"
Tech g_active = Tech::Auto;  // the upscaler that ran last
const char* TechName(int t) { return t == 1 ? "DLSS" : t == 2 ? "FSR 3.1 / FSR 4" : "Auto"; }
uint64_t g_replaced = 0;

std::string g_lastLogged;  // logged once per change, not every frame (a success in between doesn't count)

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastLogged) Log("Upscaler: %s", why.c_str());
    g_lastLogged = why;
    g_lastError = why;
}

// ---------------------------------------------------------------- per command list bindings

struct StageState {
    pipeline_layout layout = {0};
    std::vector<descriptor_table> tables;
    struct Root {
        shader_stage stages;
        descriptor_type type;
        buffer_range range;
    };
    std::map<uint32_t, Root> roots;  // root descriptors by parameter
    struct Constants {
        shader_stage stages;
        std::vector<uint32_t> values;
    };
    std::map<uint32_t, Constants> constants;  // root constants by parameter

    void Layout(pipeline_layout l) {
        if (l == layout) return;
        layout = l;  // a new root signature resets every binding
        tables.clear();
        roots.clear();
        constants.clear();
    }
};

struct __declspec(uuid("8b3e5d71-2c4a-4f09-9e6d-1a7b3c5e9f20")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    StageState compute, graphics;
    uint32_t rtvCount = 0;
    resource_view rtvs[8] = {};
    resource_view dsv = {0};
    viewport vp = {};
    bool hasVp = false;
    rect scissor = {};
    bool hasScissor = false;
    // A game draw was redirected to output size (target, viewport, SceneInfo): the game's own
    // state is put back before the next draw we don't handle, so nothing else is drawn with ours
    // (from RE3DLSS, where an unrecognised pass after a redirect drew a zoomed-in picture).
    bool redirected = false;
    // The picture before the HUD: a snapshot of the HUD texture to take at the next event (after
    // the pass that filled it), and the output pass to repeat after it is recorded.
    resource snapTex = {0};
    resource_usage snapState = resource_usage::undefined;
    resource hudTex = {0};  // the HUD texture snapshotted on this list, and its state since
    resource_usage hudState = resource_usage::undefined;
    bool hudPending = false;
    uint32_t hudVertices = 0, hudInstances = 0;
};

ListState* State(command_list* cl) { return cl->get_private_data<ListState>(); }

StageState* Stage(ListState* s, shader_stage stages) {
    if ((stages & shader_stage::all_compute) != 0) return &s->compute;
    if ((stages & shader_stage::all_graphics) != 0) return &s->graphics;
    return nullptr;
}

void OnInitList(command_list* cl) { cl->create_private_data<ListState>(); }
void OnDestroyList(command_list* cl) { cl->destroy_private_data<ListState>(); }

void OnResetList(command_list* cl) {
    if (ListState* s = State(cl)) *s = ListState();
}

void RunPending(command_list* cl, ListState& s, bool stateIntact);

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    ListState* s = State(cl);
    if (!s) return;
    RunPending(cl, *s, true);
    s->pipe = pipe;
    s->pipeStages = stages;
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (st->tables.size() < first + count) st->tables.resize(first + count, descriptor_table{0});
    for (uint32_t i = 0; i < count; ++i) st->tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (u.count != 1 || u.table.handle) return;
    if (u.type == descriptor_type::constant_buffer || u.type == descriptor_type::buffer_shader_resource_view ||
        u.type == descriptor_type::buffer_unordered_access_view)
        st->roots[param] = {stages, u.type, *static_cast<const buffer_range*>(u.descriptors)};
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    StageState::Constants& c = st->constants[param];
    c.stages = stages;
    if (c.values.size() < first + count) c.values.resize(first + count);
    memcpy(c.values.data() + first, values, count * 4);
}

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view dsv) {
    if (ListState* ls = State(cl)) RunPending(cl, *ls, true);
    ListState* s = State(cl);
    if (!s) return;
    s->rtvCount = count > 8 ? 8 : count;
    for (uint32_t i = 0; i < s->rtvCount; ++i) s->rtvs[i] = rtvs[i];
    s->dsv = dsv;
}

void OnBindViewports(command_list* cl, uint32_t first, uint32_t count, const viewport* vps) {
    ListState* s = State(cl);
    if (!s || first != 0 || !count) return;
    s->vp = vps[0];
    s->hasVp = true;
}

void OnBindScissors(command_list* cl, uint32_t first, uint32_t count, const rect* rects) {
    ListState* s = State(cl);
    if (!s || first != 0 || !count) return;
    s->scissor = rects[0];
    s->hasScissor = true;
}

// Puts the game's bindings back after DLSS. A bind with no tables makes ReShade set the game's
// descriptor heaps and root signature again (NGX changed both behind its back).
void RestoreStage(command_list* cl, const StageState& st, shader_stage stages) {
    if (!st.layout.handle) return;
    cl->bind_descriptor_tables(stages, st.layout, 0, 0, nullptr);
    for (uint32_t i = 0; i < st.tables.size(); ++i)
        if (st.tables[i].handle) cl->bind_descriptor_tables(stages, st.layout, i, 1, &st.tables[i]);
    for (const auto& [param, r] : st.roots) {
        descriptor_table_update u = {};
        u.count = 1;
        u.type = r.type;
        u.descriptors = &r.range;
        cl->push_descriptors(r.stages, st.layout, param, u);
    }
    for (const auto& [param, c] : st.constants)
        cl->push_constants(c.stages, st.layout, param, 0, static_cast<uint32_t>(c.values.size()), c.values.data());
}

void Restore(command_list* cl, const ListState& s) {
    RestoreStage(cl, s.compute, shader_stage::all_compute);
    RestoreStage(cl, s.graphics, shader_stage::all_graphics);  // last: the game draws next
    if (s.pipe.handle) cl->bind_pipeline(s.pipeStages, s.pipe);
}

void RestoreTargets(command_list* cl, const ListState& s) {
    cl->bind_render_targets_and_depth_stencil(s.rtvCount, s.rtvs, s.dsv);
    if (s.hasVp) cl->bind_viewports(0, 1, &s.vp);
    if (s.hasScissor) cl->bind_scissor_rects(0, 1, &s.scissor);
}

void BindFullTarget(command_list* cl, resource_view rtv, uint32_t w, uint32_t h) {
    cl->bind_render_targets_and_depth_stencil(1, &rtv, resource_view{0});
    const viewport vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    cl->bind_viewports(0, 1, &vp);
    const rect r = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
    cl->bind_scissor_rects(0, 1, &r);
}

// ---------------------------------------------------------------- game resources

// Where pixel shader register t<reg> space0 is in the bound graphics tables.
struct Slot {
    descriptor_table table = {0};
    uint32_t binding = 0;
    descriptor_type type = descriptor_type::shader_resource_view;
    resource_view view = {0};  // what the game put there
};

bool PixelSrvSlot(device* dev, const StageState& st, uint32_t reg, Slot* out) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return false;
    for (uint32_t i = 0; i < st.tables.size(); ++i) {
        if (!st.tables[i].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, i);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            if (range.type == descriptor_type::sampler || range.type == descriptor_type::constant_buffer ||
                range.type == descriptor_type::unordered_access_view ||
                range.type == descriptor_type::texture_unordered_access_view ||
                range.type == descriptor_type::buffer_unordered_access_view)
                continue;
            if (range.dx_register_space != 0 || (range.visibility & shader_stage::pixel) == 0) continue;
            if (reg < range.dx_register_index || reg - range.dx_register_index >= range.count) continue;
            out->table = st.tables[i];
            out->binding = range.binding + (reg - range.dx_register_index);
            out->type = range.type;
            descriptor_heap heap = {0};
            uint32_t offset = 0;
            dev->get_descriptor_heap_offset(st.tables[i], out->binding, 0, &heap, &offset);
            out->view = dt->get_resource_view(heap, offset);
            return true;
        }
    }
    return false;
}

resource PixelSrv(device* dev, const StageState& st, uint32_t reg) {
    Slot s;
    return PixelSrvSlot(dev, st, reg, &s) ? ViewResource(s.view) : resource{0};
}

// The root CBV parameters holding SceneInfo (b0 space0), any shader stage.
std::vector<uint32_t> SceneInfoParams(device* dev, const StageState& st) {
    std::vector<uint32_t> out;
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return out;
    for (const auto& [param, r] : st.roots) {
        if (r.type != descriptor_type::constant_buffer) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type == pipeline_layout_param_type::push_descriptors && p.push_descriptors.dx_register_index == 0 &&
            p.push_descriptors.dx_register_space == 0)
            out.push_back(param);
    }
    return out;
}

bool ReadSceneInfo(device* dev, const StageState& st, float* out) {
    for (uint32_t param : SceneInfoParams(dev, st)) {
        const buffer_range& b = st.roots.at(param).range;
        void* data = nullptr;
        if (!dev->map_buffer_region(b.buffer, b.offset, kSceneInfoFloats * 4, map_access::read_only, &data) || !data)
            continue;
        memcpy(out, data, kSceneInfoFloats * 4);
        dev->unmap_buffer_region(b.buffer);
        return true;
    }
    return false;
}

// A camera cut: the rotation part of this frame's and last frame's view-projection differ a lot.
bool CameraCut(const float* scene) {
    float diff = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            diff = std::fmax(diff, std::fabs(scene[kViewProj + r * 4 + c] - scene[kPrevViewProj + r * 4 + c]));
    return diff > 1.0f || !std::isfinite(diff);
}

// ---------------------------------------------------------------- our resources

std::mutex g_resMutex;  // everything in this section
device* g_dev = nullptr;
uint64_t g_frame = 0;
uint64_t g_lastRun = 0;  // g_frame + 1 when an upscaler last ran
uint32_t g_outW = 0, g_outH = 0;  // the swap chain's size

struct Texture {
    resource res = {0};
    resource_view srv = {0}, rtv = {0};
    uint32_t w = 0, h = 0;
    format fmt = format::unknown;
    resource_usage state = resource_usage::undefined;
};
Texture g_x;  // DLSS output (UAV, render target for bloom, shader resource for the tonemap)
Texture g_y;  // tonemapped output size image (upscaling)
Texture g_z;  // sharpened output size image

struct Old {
    resource res;
    resource_view srv, rtv;
    uint64_t frame;
};
std::vector<Old> g_old;  // the GPU may still use these for a few frames

void Retire(Texture& t) {
    if (t.res.handle) g_old.push_back({t.res, t.srv, t.rtv, g_frame});
    t = Texture();
}

bool Ensure(device* dev, Texture& t, uint32_t w, uint32_t h, format fmt, resource_usage usage, resource_usage initial,
            const char* name) {
    if (t.res.handle && t.w == w && t.h == h && t.fmt == fmt) return true;
    if (&t == &g_x && t.res.handle) dlss::ReleaseFeatures();  // a new output: new histories
    Retire(t);
    const resource_desc d(w, h, 1, 1, fmt, 1, memory_heap::default_, usage);
    if (!dev->create_resource(d, nullptr, initial, &t.res)) {
        Log("couldn't create the %s texture %ux%u (format %u)", name, w, h, static_cast<unsigned>(fmt));
        t = Texture();
        return false;
    }
    const resource_view_desc vd(resource_view_type::texture_2d, fmt, 0, 1, 0, 1);
    if ((usage & resource_usage::shader_resource) != 0) dev->create_resource_view(t.res, resource_usage::shader_resource, vd, &t.srv);
    if ((usage & resource_usage::render_target) != 0) dev->create_resource_view(t.res, resource_usage::render_target, vd, &t.rtv);
    g_dev = dev;
    t.w = w, t.h = h, t.fmt = fmt, t.state = initial;
    Log("%s texture %ux%u (format %u)", name, w, h, static_cast<unsigned>(fmt));
    return true;
}

void Transition(command_list* cl, Texture& t, resource_usage to) {
    if (t.state == to) return;
    cl->barrier(t.res, t.state, to);
    t.state = to;
}

// SceneInfo copies with the output size, in a ring in an upload buffer.
resource g_cb = {0};
uint8_t* g_cbData = nullptr;
uint32_t g_cbNext = 0;

bool SceneInfoCopy(device* dev, const float* scene, uint32_t w, uint32_t h, buffer_range* out) {
    if (!g_cb.handle) {
        const resource_desc d(uint64_t(kSceneInfoSlot) * kSceneInfoSlots, memory_heap::upload,
                              resource_usage::constant_buffer);
        if (!dev->create_resource(d, nullptr, resource_usage::cpu_access, &g_cb)) {
            Log("couldn't create the SceneInfo buffer");
            g_cb = {0};
            return false;
        }
        void* p = nullptr;
        if (!dev->map_buffer_region(g_cb, 0, UINT64_MAX, map_access::write_only, &p) || !p) {
            dev->destroy_resource(g_cb);
            g_cb = {0};
            return false;
        }
        g_cbData = static_cast<uint8_t*>(p);
        g_dev = dev;
    }
    float copy[kSceneInfoFloats];
    memcpy(copy, scene, sizeof(copy));
    copy[kScreenSize] = static_cast<float>(w);
    copy[kScreenSize + 1] = static_cast<float>(h);
    copy[kScreenSize + 2] = 1.0f / w;
    copy[kScreenSize + 3] = 1.0f / h;
    const uint32_t offset = (g_cbNext++ % kSceneInfoSlots) * kSceneInfoSlot;
    memcpy(g_cbData + offset, copy, sizeof(copy));
    *out = {g_cb, offset, kSceneInfoSlot};
    return true;
}

// The frame being upscaled: set at TAA, used by the passes after it (one command list, in order).
struct UpscaleFrame {
    bool active = false;  // set at TAA, cleared after the copy (or at the next TAA)
    bool bloomDone = false, tonemapDone = false, sharpenDone = false;
    Texture* final = nullptr;  // what goes over the game's stretch
    uint64_t lastFrame = UINT64_MAX;  // last present count with upscaling
    resource taaOut = {0};     // the game's TAA target (bloom's last upsample adds into it)
} g_up;

// Descriptor slots rewritten for the tonemap, put back when upscaling stops in case the game
// keeps that table from frame to frame. RE Village builds it per frame in a ring (a different
// table most frames, 0.2.0 log): then the game rewrites the slot itself, and writing an old
// value back could hit a slot the ring has since given to another draw, so nothing is put back.
struct Patch {
    Slot slot;
    uint64_t firstFrame, lastFrame;
};
std::map<uint64_t, Patch> g_patches;  // by table handle
bool g_tonemapRing = false;           // more than a few different tables seen

void NotePatch(const Slot& slot, uint64_t frame) {
    if (g_tonemapRing) return;
    auto it = g_patches.find(slot.table.handle);
    if (it == g_patches.end()) it = g_patches.emplace(slot.table.handle, Patch{slot, frame, frame}).first;
    it->second.lastFrame = frame;
    if (g_patches.size() > 4) {
        g_tonemapRing = true;
        g_patches.clear();
        Log("tonemap descriptor tables are built per frame (ring): nothing to put back");
    }
}

void UnpatchAll(device* dev) {
    for (auto& [handle, p] : g_patches) {
        if (p.lastFrame - p.firstFrame < 3 || !p.slot.view.handle) continue;  // not kept across frames
        descriptor_table_update u = {};
        u.table = p.slot.table;
        u.binding = p.slot.binding;
        u.count = 1;
        u.type = p.slot.type;
        u.descriptors = &p.slot.view;
        dev->update_descriptor_tables(1, &u);
        Log("tonemap colour slot put back (table %llx)", handle);
    }
    g_patches.clear();
}

// ---------------------------------------------------------------- render size, frame generation

// The render size: the colour texture's, or SceneInfo's screen size when the viewport is smaller.
void RenderSize(device* dev, resource color, const float* scene, uint32_t* w, uint32_t* h) {
    const resource_desc cd = dev->get_resource_desc(color);
    *w = cd.texture.width, *h = cd.texture.height;
    const float sw = scene[kScreenSize], sh = scene[kScreenSize + 1];
    if (sw >= 64 && sh >= 64 && sw <= *w && sh <= *h) *w = static_cast<uint32_t>(sw), *h = static_cast<uint32_t>(sh);
}

// The projection is shifted by the clip-space jitter; DLSS and FSR want the sample's offset from
// the pixel centre in render pixels (x right, y down). Confirmed in game with DLAA (0.1.0).
float JitterX(const float* scene, uint32_t w) { return -scene[kProjElement0 + 2] * w * 0.5f * (g_set.flipJitterX ? -1 : 1); }
float JitterY(const float* scene, uint32_t h) { return scene[kProjElement0 + 3] * h * 0.5f * (g_set.flipJitterY ? -1 : 1); }

// Frame generation's inputs, at the TAA draw (whoever upscales): the depth (depth read + shader
// resource, 0xE0), the velocity (shader resource, 0xC0; NDC offsets, y up: UV = (0.5, -0.5) x),
// SceneInfo, jitter, camera cut.
void GiveFrameGen(command_list* cl, ListState& s) {
    if (!FrameGenWantsCamera()) return;
    device* dev = cl->get_device();
    const resource depth = PixelSrv(dev, s.graphics, kTaaDepth);
    const resource color = PixelSrv(dev, s.graphics, kTaaColor);
    const resource motion = PixelSrv(dev, s.graphics, kTaaMotion);
    float scene[kSceneInfoFloats];
    if (!depth.handle || !color.handle || !motion.handle || !ReadSceneInfo(dev, s.graphics, scene)) return;
    uint32_t w, h;
    RenderSize(dev, color, scene, &w, &h);
    FrameGenSetSceneInfo(scene);
    constexpr unsigned kDepthRead = 0x20 | 0x40 | 0x80, kShaderResource = 0x40 | 0x80;  // D3D12 resource states
    FrameGenTag(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), reinterpret_cast<ID3D12Resource*>(depth.handle),
                kDepthRead, reinterpret_cast<ID3D12Resource*>(motion.handle), kShaderResource, w, h, JitterX(scene, w),
                JitterY(scene, h), 0.5f, -0.5f, CameraCut(scene));
}

// ---------------------------------------------------------------- the picture before the HUD

Texture g_hudScene;                    // the snapshot (g_resMutex)
resource g_hudSceneOf = {0};           // the HUD texture it was taken from
uint64_t g_hudSceneFrame = UINT64_MAX;  // when
uint64_t g_hudlessDrawn = 0;

// Copies the HUD texture (in <state>) into the snapshot, before any HUD is drawn into it.
void Snapshot(command_list* cl, ListState& s, resource tex, resource_usage state) {
    if (!FrameGenWantsHudless() || !tex.handle || state == resource_usage::undefined) return;
    device* dev = cl->get_device();
    const resource_desc d = dev->get_resource_desc(tex);
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!Ensure(dev, g_hudScene, d.texture.width, d.texture.height, d.texture.format,
                resource_usage::copy_source | resource_usage::copy_dest, resource_usage::copy_source, "picture before the HUD"))
        return;
    Transition(cl, g_hudScene, resource_usage::copy_dest);
    cl->barrier(tex, state, resource_usage::copy_source);
    cl->copy_resource(tex, g_hudScene.res);
    cl->barrier(tex, resource_usage::copy_source, state);
    Transition(cl, g_hudScene, resource_usage::copy_source);
    g_hudSceneOf = tex, g_hudSceneFrame = g_frame;
    s.hudTex = tex, s.hudState = state;
}

// After the output pass: the snapshot back into the HUD texture, the same draw into our target,
// the game's render target back.
void DrawHudless(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    resource tex = {0};
    resource_view rtv = {0};
    if (!s.rtvs[0].handle || s.hudState == resource_usage::undefined || !FrameGenHudlessTarget(dev, &tex, &rtv)) return;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        if (!g_hudScene.res.handle || g_hudSceneOf != s.hudTex) return;
        Transition(cl, g_hudScene, resource_usage::copy_source);
        cl->barrier(s.hudTex, s.hudState, resource_usage::copy_dest);
        cl->copy_resource(g_hudScene.res, s.hudTex);
        cl->barrier(s.hudTex, resource_usage::copy_dest, s.hudState);
    }
    cl->barrier(tex, resource_usage::shader_resource, resource_usage::render_target);
    cl->bind_render_targets_and_depth_stencil(1, &rtv);
    cl->draw(s.hudVertices, s.hudInstances, 0, 0);
    cl->bind_render_targets_and_depth_stencil(s.rtvCount, s.rtvs, s.dsv);
    cl->barrier(tex, resource_usage::render_target, resource_usage::shader_resource);
    FrameGenHudlessDrawn(cl);
    if (++g_hudlessDrawn == 1) Log("frame generation: picture before the HUD drawn (output pass repeated without the HUD)");
}

// What's waiting on this list: a snapshot (any time before the HUD), and the repeated output pass
// (only while the game's state is still the output pass's: <stateIntact>).
void RunPending(command_list* cl, ListState& s, bool stateIntact) {
    if (s.snapTex.handle) {
        const resource tex = s.snapTex;
        const resource_usage state = s.snapState;
        s.snapTex = {0};
        Snapshot(cl, s, tex, state);
    }
    if (s.hudPending) {
        s.hudPending = false;
        if (stateIntact) DrawHudless(cl, s);
    }
}

// The HUD texture: what the output pass read last. When nothing took a snapshot of it yet this frame,
// one is taken just before the first draw into it that isn't a full-screen triangle (the HUD): the
// scene is in it then, whichever pass put it there (the stretch at render size, or the tonemap or
// sharpening at native size).
std::atomic<uint64_t> g_hudTexSeen{0};

void SnapshotBeforeHud(command_list* cl, ListState& s) {
    const uint64_t tex = g_hudTexSeen.load(std::memory_order_relaxed);
    if (!tex || !FrameGenWantsHudless() || ViewResource(s.rtvs[0]).handle != tex) return;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        if (g_hudSceneFrame == g_frame && g_hudSceneOf.handle == tex) return;
    }
    Snapshot(cl, s, resource{tex}, resource_usage::render_target);
}

// The output pass is about to be recorded: repeat it afterwards if this frame has a snapshot of
// the texture it reads.
void NoteOutputPass(command_list* cl, ListState& s, uint32_t vertices, uint32_t instances) {
    g_hudTexSeen = PixelSrv(cl->get_device(), s.graphics, 0).handle;
    if (!s.hudTex.handle || !FrameGenWantsHudless()) return;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        if (g_hudSceneFrame != g_frame || g_hudSceneOf != s.hudTex) return;
    }
    if (PixelSrv(cl->get_device(), s.graphics, 0) != s.hudTex) return;
    s.hudPending = true;
    s.hudVertices = vertices, s.hudInstances = instances;
}

void OnBarrier(command_list* cl, uint32_t count, const resource* res, const resource_usage*, const resource_usage* after) {
    ListState* s = State(cl);
    if (!s) return;
    RunPending(cl, *s, true);
    for (uint32_t i = 0; i < count; ++i)
        if (res[i] == s->hudTex) s->hudState = after[i];
}

// ---------------------------------------------------------------- passes

bool OnTaa(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    const resource depth = PixelSrv(dev, s.graphics, kTaaDepth);
    const resource color = PixelSrv(dev, s.graphics, kTaaColor);
    const resource motion = PixelSrv(dev, s.graphics, kTaaMotion);
    const resource target = ViewResource(s.rtvs[0]);
    if (!depth.handle || !color.handle || !motion.handle || !target.handle) {
        Fail("TAA inputs not found (depth, colour, velocity or render target)");
        return false;
    }
    float scene[kSceneInfoFloats];
    if (!ReadSceneInfo(dev, s.graphics, scene)) {
        Fail("camera constants (SceneInfo) not found");
        return false;
    }
    const resource_desc td = dev->get_resource_desc(target);
    uint32_t renderW, renderH;
    RenderSize(dev, color, scene, &renderW, &renderH);

    // Which upscaler: Auto takes DLSS when NGX starts (RTX cards), FSR 3/4 otherwise.
    ID3D12Device* d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    std::string err;
    Tech tech = static_cast<Tech>(g_set.upscaler);
    if (tech == Tech::Auto) {
        std::string dlssErr;
        tech = dlss::Init(d3d, dlssErr) ? Tech::Dlss : Tech::Fsr;
    }
    if (tech == Tech::Dlss ? !dlss::Init(d3d, err) : !fsr::Init(d3d, err)) {
        Fail(err);
        return false;
    }
    if (tech != g_active) {  // free the other upscaler's history textures
        if (g_active == Tech::Dlss) dlss::ReleaseFeatures();
        if (g_active == Tech::Fsr) fsr::ReleaseContexts();
        Log("Upscaler: %s", tech == Tech::Dlss ? "DLSS" : "FSR 3/4");
        g_active = tech;
    }

    std::lock_guard<std::mutex> lock(g_resMutex);
    const bool upscale = g_set.upscale && g_outW && g_outH && renderW < g_outW && renderH < g_outH &&
                         renderW * 3 + 16 >= g_outW && renderH * 3 + 16 >= g_outH;  // DLSS goes down to a third (1278 x 3 = 3834)
    const uint32_t outW = upscale ? g_outW : td.texture.width, outH = upscale ? g_outH : td.texture.height;
    const resource_usage usage = resource_usage::unordered_access | resource_usage::copy_source |
                                 resource_usage::shader_resource | resource_usage::render_target;
    if (!Ensure(dev, g_x, outW, outH, td.texture.format, usage, resource_usage::unordered_access, "DLSS output")) {
        Fail("no DLSS output texture");
        return false;
    }

    const float jitterX = JitterX(scene, renderW), jitterY = JitterY(scene, renderH);
    const bool reset = CameraCut(scene);
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    Transition(cl, g_x, resource_usage::unordered_access);
    bool ok;
    if (tech == Tech::Dlss) {
        dlss::Inputs in;
        in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
        in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
        in.output = reinterpret_cast<ID3D12Resource*>(g_x.res.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = 0.5f * renderW;  // NDC offsets, y up -> render pixels, y down
        in.motionScaleY = -0.5f * renderH;
        in.reset = reset;
        in.preset = g_set.preset;
        ok = dlss::Evaluate(list, in, err);
    } else {
        fsr::Inputs in;
        in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
        in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
        in.output = reinterpret_cast<ID3D12Resource*>(g_x.res.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = 0.5f * renderW;
        in.motionScaleY = -0.5f * renderH;
        const float nearZ = scene[kNearPlane], yScale = scene[kProjElement0 + 1];
        in.cameraNear = nearZ > 0 && nearZ < 10 ? nearZ : 0.01f;
        in.cameraFar = 0;  // RE Engine: reversed Z, infinite far plane
        in.cameraFovY = yScale > 0.05f ? 2 * std::atan(1 / yScale) : 1.0f;
        in.metersPerUnit = 1.0f;  // RE Engine: metres
        in.sharpness = 0;
        in.reset = reset;
        ok = fsr::Evaluate(list, in, err);
    }
    if (!ok) {
        Restore(cl, s);
        Fail(err);
        return false;
    }

    std::string mode = std::string(tech == Tech::Dlss ? "DLSS " : "FSR ") +
                       (upscale ? std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " + std::to_string(outW) +
                                      "x" + std::to_string(outH)
                                : (tech == Tech::Dlss ? "DLAA " : "native AA ") + std::to_string(renderW) + "x" +
                                      std::to_string(renderH));
    bool replaced;
    if (upscale) {
        // The game's TAA still runs at render size (bloom input, history); the passes after it
        // use X.
        Restore(cl, s);
        g_up.active = true;
        g_up.bloomDone = g_up.tonemapDone = g_up.sharpenDone = false;
        g_up.final = nullptr;
        g_up.lastFrame = g_frame;
        g_up.taaOut = target;
        replaced = false;
    } else {
        Transition(cl, g_x, resource_usage::copy_source);
        cl->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
        cl->copy_resource(g_x.res, target);
        cl->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
        Transition(cl, g_x, resource_usage::unordered_access);
        Restore(cl, s);
        g_up.active = false;
        replaced = true;
    }

    std::lock_guard<std::mutex> slock(g_statusMutex);
    if (!g_replaced++ || !g_lastError.empty() || mode != g_mode)
        Log("Upscaler running: %s, jitter %.3f %.3f px%s", mode.c_str(), jitterX, jitterY, reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    g_lastRun = g_frame + 1;
    return replaced;
}

// The bloom and tonemap passes are redirected in place: their state is changed and the game's own
// draw then runs at output size. Drawing them a second time from here (as VillageDLSS does) loses
// RenoDX's per-draw settings when this add-on's callbacks run before RenoDX's (VillageDLSS sorts before
// renodx-*: washed-out image in 0.1.0). The render-size results aren't needed: the tonemap reads
// the output-size image, and its render-size target only feeds the stretch we replace. The game's
// state is put back at that stretch (OnFinalCompute).

// Bloom (additive, texture coordinates): the game's draw into its TAA output goes into X.
void OnBloom(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || g_up.bloomDone || !g_x.rtv.handle) return;
    if (kBloomIntoTaaOnly && ViewResource(s.rtvs[0]) != g_up.taaOut) return;
    Transition(cl, g_x, resource_usage::render_target);
    BindFullTarget(cl, g_x.rtv, g_x.w, g_x.h);
    s.redirected = true;
    g_up.bloomDone = true;
}

// Draws the game's bound full-screen pass at output size instead of render size: pixel shader
// register t<reg> rewritten (in its descriptor table) to src, SceneInfo replaced by a copy with
// the output size, render target dst (created like the game's target). The game's render-size
// draw is skipped. g_resMutex held.
// inPlace: set everything up and return, so the game's own draw runs at output size (see above).
bool RedrawAtOutputSize(command_list* cl, ListState& s, uint32_t reg, Texture& src, Texture& dst, const char* name,
                        bool inPlace = false) {
    device* dev = cl->get_device();
    Slot slot;
    float scene[kSceneInfoFloats];
    if (!PixelSrvSlot(dev, s.graphics, reg, &slot) || !ReadSceneInfo(dev, s.graphics, scene)) {
        Fail(std::string(name) + " inputs not found: game's upscaling used");
        return false;
    }
    const resource target = ViewResource(s.rtvs[0]);
    if (!target.handle) return false;
    const format fmt = dev->get_resource_desc(target).texture.format;
    if (!Ensure(dev, dst, src.w, src.h, fmt,
                resource_usage::render_target | resource_usage::copy_source | resource_usage::shader_resource,
                resource_usage::render_target, name))
        return false;
    buffer_range cb;
    if (!SceneInfoCopy(dev, scene, src.w, src.h, &cb)) return false;

    // The slot now shows src (the render-size draw that read it is skipped).
    NotePatch(slot, g_frame);
    descriptor_table_update u = {};
    u.table = slot.table;
    u.binding = slot.binding;
    u.count = 1;
    u.type = slot.type;
    u.descriptors = &src.srv;
    dev->update_descriptor_tables(1, &u);

    Transition(cl, src, resource_usage::shader_resource);
    Transition(cl, dst, resource_usage::render_target);
    for (uint32_t param : SceneInfoParams(dev, s.graphics)) {
        descriptor_table_update cu = {};
        cu.count = 1;
        cu.type = descriptor_type::constant_buffer;
        cu.descriptors = &cb;
        cl->push_descriptors(s.graphics.roots.at(param).stages, s.graphics.layout, param, cu);
    }
    BindFullTarget(cl, dst.rtv, dst.w, dst.h);
    if (inPlace) {  // the game's draw runs next, into dst; its state comes back before the one after
        s.redirected = true;
        return true;
    }
    cl->draw(3, 1, 0, 0);
    Restore(cl, s);  // puts the game's SceneInfo back
    RestoreTargets(cl, s);
    return true;
}

// Tonemap (reads t0) at output size into Y, from X: the game's draw, redirected.
bool OnTonemap(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || g_up.tonemapDone || !g_x.srv.handle) return false;
    if (!RedrawAtOutputSize(cl, s, kTonemapColor, g_x, g_y, "tonemapped", true)) return false;
    g_up.tonemapDone = true;
    g_up.final = &g_y;
    return false;  // the game's draw runs, at output size
}

// The game's sharpening (Sharpness_PS, reads t0, the game's Sharpness setting): the game's draw
// redirected to output size, into Z from Y.
bool OnSharpen(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || !g_up.tonemapDone || g_up.sharpenDone || !g_y.srv.handle) return false;
    if (kSharpenOptional && !g_set.sharpen) return false;
    if (!RedrawAtOutputSize(cl, s, 0, g_y, g_z, "sharpened", true)) return false;
    g_up.sharpenDone = true;
    g_up.final = &g_z;
    return false;  // the game's draw runs, at output size
}

// The game's stretch to output size: its draw runs (it also writes the output-size depth), then
// the output-size image (Y, or Z when sharpened) goes over its colour.
//
// It's drawn with our own full-screen pass (blit.hlsl), not copied: at some render scales the
// game's post chain is r11g11b10f while its output target is rgba16f, and only a render target
// write converts formats. g_resMutex held for these.

pipeline_layout g_blitLayout = {0};
std::map<format, pipeline> g_blitPipes;  // one per render target format

pipeline BlitPipeline(device* dev, format fmt) {
    if (!g_blitLayout.handle) {
        descriptor_range r = {};
        r.count = 1;
        r.array_size = 1;
        r.visibility = shader_stage::pixel;
        r.type = descriptor_type::texture_shader_resource_view;
        const pipeline_layout_param p(r);
        if (!dev->create_pipeline_layout(1, &p, &g_blitLayout)) {
            g_blitLayout = {0};
            Log("couldn't create the blit pipeline layout");
            return {0};
        }
    }
    auto it = g_blitPipes.find(fmt);
    if (it != g_blitPipes.end()) return it->second;
    shader_desc vs = {}, ps = {};
    vs.code = kBlitVs, vs.code_size = sizeof(kBlitVs);
    ps.code = kBlitPs, ps.code_size = sizeof(kBlitPs);
    rasterizer_desc rs = {};
    rs.cull_mode = cull_mode::none;
    depth_stencil_desc ds = {};
    ds.depth_enable = false;
    ds.stencil_enable = false;
    primitive_topology topology = primitive_topology::triangle_list;
    format rt = fmt;
    const pipeline_subobject subs[] = {{pipeline_subobject_type::vertex_shader, 1, &vs},
                                       {pipeline_subobject_type::pixel_shader, 1, &ps},
                                       {pipeline_subobject_type::rasterizer_state, 1, &rs},
                                       {pipeline_subobject_type::depth_stencil_state, 1, &ds},
                                       {pipeline_subobject_type::primitive_topology, 1, &topology},
                                       {pipeline_subobject_type::render_target_formats, 1, &rt}};
    pipeline p = {0};
    if (!dev->create_pipeline(g_blitLayout, 6, subs, &p)) {
        Log("couldn't create the blit pipeline for format %u", static_cast<unsigned>(fmt));
        p = {0};
    }
    g_blitPipes[fmt] = p;
    return p;
}

bool OnCopy(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || !g_up.tonemapDone || !g_up.final || !g_up.final->srv.handle) return false;
    Texture& src = *g_up.final;
    device* dev = cl->get_device();
    const resource target = ViewResource(s.rtvs[0]);
    if (!target.handle) return false;
    const resource_desc td = dev->get_resource_desc(target);
    const pipeline blit = BlitPipeline(dev, td.texture.format);
    if (td.texture.width < src.w || td.texture.height < src.h || td.texture.samples > 1 || !blit.handle) {
        Fail("output target " + std::to_string(td.texture.width) + "x" + std::to_string(td.texture.height) + " format " +
             std::to_string(static_cast<unsigned>(td.texture.format)) + " doesn't take the " + std::to_string(src.w) +
             "x" + std::to_string(src.h) + " image: game's upscaling used");
        return false;
    }
    cl->draw(3, 1, 0, 0);  // the game's pass (output-size depth)
    Transition(cl, src, resource_usage::shader_resource);
    cl->bind_pipeline(pipeline_stage::all_graphics, blit);
    const dynamic_state topoState = dynamic_state::primitive_topology;
    const uint32_t topo = static_cast<uint32_t>(primitive_topology::triangle_list);
    cl->bind_pipeline_states(1, &topoState, &topo);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::texture_shader_resource_view;
    u.descriptors = &src.srv;
    cl->push_descriptors(shader_stage::pixel, g_blitLayout, 0, u);
    BindFullTarget(cl, s.rtvs[0], src.w, src.h);
    cl->draw(3, 1, 0, 0);
    Restore(cl, s);
    RestoreTargets(cl, s);
    g_up.active = false;
    return true;
}

// The stretch to output size is a compute pass (CAS_CS, u0 = the output-size target the HUD is
// drawn into). When upscaling, it's skipped and the output-size image is drawn into that target
// with our own pass (it converts formats), through a render target view of our own.

std::map<uint64_t, resource_view> g_targetRtvs;  // by resource; g_resMutex

resource ComputeUav(device* dev, const StageState& st, uint32_t reg) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
    for (uint32_t i = 0; i < st.tables.size(); ++i) {
        if (!st.tables[i].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, i);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            const bool uav = range.type == descriptor_type::unordered_access_view ||
                             range.type == descriptor_type::texture_unordered_access_view;
            if (!uav || range.dx_register_space != 0) continue;
            if (reg < range.dx_register_index || reg - range.dx_register_index >= range.count) continue;
            descriptor_heap heap = {0};
            uint32_t offset = 0;
            dev->get_descriptor_heap_offset(st.tables[i], range.binding + (reg - range.dx_register_index), 0, &heap,
                                            &offset);
            return ViewResource(dt->get_resource_view(heap, offset));
        }
    }
    return {0};
}

bool OnFinalCompute(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || !g_up.tonemapDone || !g_up.final || !g_up.final->srv.handle) return false;
    Texture& src = *g_up.final;
    device* dev = cl->get_device();
    const resource target = ComputeUav(dev, s.compute, 0);
    if (!target.handle) return false;
    const resource_desc td = dev->get_resource_desc(target);
    const pipeline blit = BlitPipeline(dev, td.texture.format);
    resource_view& rtv = g_targetRtvs[target.handle];
    if (!rtv.handle && (td.usage & resource_usage::render_target) != 0)
        dev->create_resource_view(target, resource_usage::render_target,
                                  resource_view_desc(resource_view_type::texture_2d, td.texture.format, 0, 1, 0, 1), &rtv);
    if (td.texture.width < src.w || td.texture.height < src.h || !blit.handle || !rtv.handle) {
        Fail("output target " + std::to_string(td.texture.width) + "x" + std::to_string(td.texture.height) + " format " +
             std::to_string(static_cast<unsigned>(td.texture.format)) + " doesn't take the " + std::to_string(src.w) +
             "x" + std::to_string(src.h) + " image: game's upscaling used");
        return false;
    }
    Transition(cl, src, resource_usage::shader_resource);
    cl->barrier(target, resource_usage::unordered_access, resource_usage::render_target);
    cl->bind_pipeline(pipeline_stage::all_graphics, blit);
    const dynamic_state topoState = dynamic_state::primitive_topology;
    const uint32_t topo = static_cast<uint32_t>(primitive_topology::triangle_list);
    cl->bind_pipeline_states(1, &topoState, &topo);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::texture_shader_resource_view;
    u.descriptors = &src.srv;
    cl->push_descriptors(shader_stage::pixel, g_blitLayout, 0, u);
    BindFullTarget(cl, rtv, src.w, src.h);
    cl->draw(3, 1, 0, 0);
    cl->barrier(target, resource_usage::render_target, resource_usage::unordered_access);  // as the dispatch left it
    Restore(cl, s);
    RestoreTargets(cl, s);
    s.redirected = false;  // everything is the game's again
    g_up.active = false;
    s.snapTex = target, s.snapState = resource_usage::unordered_access;  // the scene before the HUD
    return true;
}

void DestroyAll(device* dev) {
    for (auto& [res, rtv] : g_targetRtvs)
        if (rtv.handle) dev->destroy_resource_view(rtv);
    g_targetRtvs.clear();
    dlss::Shutdown();
    fsr::Shutdown();
    for (Texture* t : {&g_x, &g_y, &g_z, &g_hudScene}) Retire(*t);
    for (Old& o : g_old) {
        if (o.srv.handle) dev->destroy_resource_view(o.srv);
        if (o.rtv.handle) dev->destroy_resource_view(o.rtv);
        dev->destroy_resource(o.res);
    }
    g_old.clear();
    if (g_cb.handle) {
        dev->unmap_buffer_region(g_cb);
        dev->destroy_resource(g_cb);
    }
    g_cb = {0};
    g_cbData = nullptr;
    for (auto& [fmt, p] : g_blitPipes)
        if (p.handle) dev->destroy_pipeline(p);
    g_blitPipes.clear();
    if (g_blitLayout.handle) dev->destroy_pipeline_layout(g_blitLayout);
    g_blitLayout = {0};
    g_dev = nullptr;
}

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (dev == g_dev) DestroyAll(dev);
}

// ---------------------------------------------------------------- texture detail
//
// At a lower render size the GPU picks smaller mip levels, so textures look blurrier than at
// native. DLSS's guidance: a mip bias of log2(render width / output width). Samplers are made
// when the game starts, so the bias comes from the render scale at that time (the Image Quality
// in config.ini) and a change needs a restart. Only material samplers get it: anisotropic or
// trilinear with wrapping addressing (post-processing uses clamped ones).

float g_mipBias = 0;
std::atomic<uint32_t> g_samplersBiased{0}, g_samplersOther{0};

float StartupRenderScale() {
    FILE* f = _wfopen((ExeDir() + L"config.ini").c_str(), L"r");
    if (!f) return 1;
    char line[256];
    float scale = 1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "ImageQualityRate=", 17)) scale = static_cast<float>(atof(line + 17));
    fclose(f);
    return scale > 0.2f && scale < 4 ? scale : 1;
}

bool OnCreateSampler(device*, sampler_desc& d) {
    const bool material = (d.filter == filter_mode::anisotropic || d.filter == filter_mode::min_mag_mip_linear) &&
                          d.address_u == texture_address_mode::wrap && d.max_lod > d.min_lod;
    if (!material || g_mipBias == 0) {
        ++g_samplersOther;
        return false;
    }
    d.mip_lod_bias += g_mipBias;
    ++g_samplersBiased;
    return true;
}

// ---------------------------------------------------------------- DLSS mode -> render scale
//
// The game's render scale is via.render.RenderConfig.ImageQualityRate (the Image Quality
// slider). The REFramework script VillageDLSS.lua sets it: this side writes the wanted scale
// (0: leave the slider alone) to reframework/data/VillageDLSS_mode.txt and reads back
// VillageDLSS_status.txt ("<current scale> <frame>").

std::wstring DataDir() { return ExeDir() + L"reframework\\data\\"; }

float WantedScale() { return g_set.enabled && g_set.mode > 0 ? kModes[g_set.mode].scale : 0; }

float g_writtenScale = -1;

void WriteWantedScale() {
    const float want = WantedScale();
    if (want == g_writtenScale) return;
    CreateDirectoryW((ExeDir() + L"reframework").c_str(), nullptr);
    CreateDirectoryW(DataDir().c_str(), nullptr);
    if (FILE* f = _wfopen((DataDir() + L"VillageDLSS_mode.txt").c_str(), L"w")) {
        fprintf(f, "%.4f\n", want);
        fclose(f);
        g_writtenScale = want;
        Log("render scale for VillageDLSS.lua: %s", want > 0 ? std::to_string(want).c_str() : "the game's slider");
    }
}

// The script's last report: the current scale, and whether it's recent (the script is running).
bool ScriptStatus(float* scale) {
    const std::wstring path = DataDir() + L"VillageDLSS_status.txt";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t t = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (n > t && n - t > 50000000ull) return false;  // older than 5 s
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    float v = -1;
    const int got = fscanf(f, "%f", &v);
    fclose(f);
    if (got != 1) return false;
    *scale = v;
    return true;
}

bool g_keyWasDown = false;
bool g_wasEnabled = false;
bool g_samplerStatsLogged = false;

}  // namespace

bool UpscalerOnDispatch(command_list* cl) {
    ListState* s = State(cl);
    if (!s) return false;
    RunPending(cl, *s, false);
    if (PipelinePs(s->pipe) != kFinalCs) return false;
    if (g_set.enabled && OnFinalCompute(cl, *s)) return true;
    // The game's CAS runs: the HUD texture (u0) holds the scene after it.
    s->snapTex = ComputeUav(cl->get_device(), s->compute, 0), s->snapState = resource_usage::unordered_access;
    return false;
}

// Puts the game's own targets, viewport and bindings back after a redirected draw.
void EndRedirect(command_list* cl, ListState& s) {
    if (!s.redirected) return;
    s.redirected = false;
    Restore(cl, s);
    RestoreTargets(cl, s);
}

void UpscalerBeforeDraw(command_list* cl) {
    if (ListState* s = State(cl)) {
        RunPending(cl, *s, false);
        EndRedirect(cl, *s);
        SnapshotBeforeHud(cl, *s);
    }
}

bool UpscalerOnDraw(command_list* cl, uint32_t vertices, uint32_t instances) {
    ListState* s = State(cl);
    if (!s) return false;
    RunPending(cl, *s, false);
    EndRedirect(cl, *s);
    if (vertices != 3 || instances != 1) {
        SnapshotBeforeHud(cl, *s);
        return false;
    }
    const uint32_t id = PipelinePs(s->pipe);
    if (id == kTaaPs) GiveFrameGen(cl, *s);
    if (id == kOutputPs) NoteOutputPass(cl, *s, vertices, instances);
    if (!g_set.enabled) return false;
    switch (id) {
        case kTaaPs: return OnTaa(cl, *s);
        case kBloomPs: OnBloom(cl, *s); return false;
        case kTonemapPs: return OnTonemap(cl, *s);
        case kSharpenPs: return OnSharpen(cl, *s);
        case kCopyPs: return OnCopy(cl, *s);
        default: return false;
    }
}

void UpscalerPresent(swapchain* sc) {
    dlss::EndFrame();
    fsr::EndFrame();
    if (g_frame % 600 == 599) {
        const std::string d = FrameGenDiag();
        if (!d.empty() && d.find("frames untagged 0,") == std::string::npos) Log("frame generation diag:%s", d.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        ++g_frame;
        const resource_desc bd = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        g_outW = bd.texture.width, g_outH = bd.texture.height;
        for (size_t i = 0; i < g_old.size();) {
            if (g_frame - g_old[i].frame < 8 || !g_dev) {
                ++i;
                continue;
            }
            if (g_old[i].srv.handle) g_dev->destroy_resource_view(g_old[i].srv);
            if (g_old[i].rtv.handle) g_dev->destroy_resource_view(g_old[i].rtv);
            g_dev->destroy_resource(g_old[i].res);
            g_old.erase(g_old.begin() + i);
        }
        // Upscaling stopped (DLSS off, or back to DLAA): put back tonemap slots the game may keep.
        const bool upscaling = g_set.enabled && g_up.lastFrame != UINT64_MAX && g_up.lastFrame + 2 >= g_frame;
        if (!upscaling && !g_patches.empty() && g_dev) UnpatchAll(g_dev);
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        SaveSettings();
        Log("Upscaling %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    g_wasEnabled = g_set.enabled;
    WriteWantedScale();
    if (!g_samplerStatsLogged && g_frame > 600) {
        g_samplerStatsLogged = true;
        Log("samplers: %u material ones biased by %.2f, %u others", g_samplersBiased.load(), g_mipBias, g_samplersOther.load());
    }
}

const ImVec4 kWarn(1, 0.6f, 0.3f, 1);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void RestoreListState(command_list* cl) {
    if (ListState* s = State(cl)) Restore(cl, *s);
}

namespace {

// DLSS models offered (NVSDK_NGX_DLSS_Hint_Render_Preset_*; dlss::kPresetAuto = per render scale).
const char* const kModelNames[] = {"Automatic (recommended)", "K (transformer)", "J (transformer)",
                                   "L (transformer 2, DLSS 4.5)", "M (transformer 2, DLSS 4.5)", "NVIDIA's default"};
const int kModelIds[] = {dlss::kPresetAuto, 11, 10, 12, 13, 0};
constexpr int kModelCount = 6;
int ModelIndex(int id) {
    for (int i = 0; i < kModelCount; ++i)
        if (kModelIds[i] == id) return i;
    return 0;
}

// The upscaler that runs with the current settings (Auto resolves once DLSS was tried).
Tech ResolvedTech() {
    if (g_set.upscaler == static_cast<int>(Tech::Dlss)) return Tech::Dlss;
    if (g_set.upscaler == static_cast<int>(Tech::Fsr)) return Tech::Fsr;
    return g_active != Tech::Auto ? g_active : Tech::Dlss;
}

// One line for the top of the overlay: what replaces the TAA right now; *warn when it's a problem.
std::string UpscalerHeadline(bool* warn) {
    *warn = false;
    std::string err, mode;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        err = g_lastError;
        mode = g_mode;
    }
    if (!g_set.enabled) return "Upscaling off (F9): the game's TAA";
    uint64_t frame, lastRun;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        frame = g_frame, lastRun = g_lastRun;
    }
    if (lastRun && frame < lastRun + 30) return "Upscaling: " + mode;
    *warn = true;
    if (!err.empty()) return "Upscaling not running: " + err;
    return "Upscaling waits for the game's TAA: keep Anti-Aliasing on TAA (menus don't use it)";
}

}  // namespace

void UpscalerOverlay() {
    ImGui::TextDisabled(RD_PRODUCT " " RD_VERSION);
    bool warn = false;
    const std::string up = UpscalerHeadline(&warn);
    if (warn) ImGui::TextColored(kWarn, "%s", up.c_str());
    else ImGui::TextUnformatted(up.c_str());
    const std::string fg = FrameGenHeadline(&warn);
    if (warn) ImGui::TextColored(kWarn, "%s", fg.c_str());
    else ImGui::TextUnformatted(fg.c_str());
    ImGui::Spacing();

    if (!ImGui::BeginTabBar("##villagedlss")) return;
    bool changed = false;
    if (ImGui::BeginTabItem("Settings")) {
        ImGui::SeparatorText("Upscaling");
        changed |= ImGui::Checkbox("Upscaling (F9)", &g_set.enabled);
        Help("Puts DLSS or FSR 3/4 in place of the game's TAA: at 100% render resolution DLAA (or FSR's native "
             "anti-aliasing), below it upscaling to the screen, with the game's bloom, tonemapping (RenoDX's HDR), sharpening and HUD at "
             "full resolution. Keep the game's Anti-Aliasing on TAA.\n\nF9 switches it on and off.");
        ImGui::BeginDisabled(!g_set.enabled);
        const char* techs[] = {"Auto", "DLSS", "FSR 3.1 / FSR 4"};
        if (g_set.upscaler < 0 || g_set.upscaler > 2) g_set.upscaler = 0;
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::Combo("Upscaler", &g_set.upscaler, techs, 3)) {
            changed = true;
            Log("Upscaler setting: %s", TechName(g_set.upscaler));
        }
        Help("Auto: DLSS on NVIDIA RTX cards, FSR on others.\nDLSS: NVIDIA DLSS Super Resolution (RTX cards).\n"
             "FSR 3.1 / FSR 4: AMD's upscaler, any DirectX 12 card; AMD's runtime picks FSR 4 on Radeon cards that "
             "support it.\n\nSwitches right away.");
        const char* names[kModeCount];
        for (int i = 0; i < kModeCount; ++i) names[i] = kModes[i].name;
        ImGui::SetNextItemWidth(ControlWidth());
        changed |= ImGui::Combo("Render resolution", &g_set.mode, names, kModeCount);
        Help("Sets the game's render scale (its Image Quality setting) while upscaling is on, and keeps it there. "
             "Needs REFramework (the bundled VillageDLSS.lua sets it). \"Game's Image Quality slider\" leaves it to the "
             "game's menu.");
        float now = 0;
        if (ScriptStatus(&now))
            ImGui::TextDisabled("Render scale now: %.0f%%%s", now * 100,
                                g_set.mode > 0 && g_set.enabled ? "" : " (the game's slider)");
        else if (g_set.mode > 0)
            ImGui::TextColored(kWarn, "VillageDLSS.lua isn't running: is REFramework installed?");
        {
            ImGui::BeginDisabled(ResolvedTech() == Tech::Fsr);
            int model = ModelIndex(g_set.preset);
            ImGui::SetNextItemWidth(ControlWidth());
            if (ImGui::Combo("DLSS model", &model, kModelNames, kModelCount)) {
                g_set.preset = kModelIds[model];
                changed = true;
                Log("DLSS model setting: %s", kModelNames[model]);
            }
            ImGui::EndDisabled();
            Help("Which DLSS model upscales. Automatic follows NVIDIA's recommendation for the render "
                 "resolution: K from 100% down to Balanced, M at Performance (50%), L at Ultra "
                 "Performance (33%). K, J, L or M use that model at every resolution; L and M (DLSS 4.5's "
                 "second-generation transformer) cost more, especially on RTX 20 and 30 cards. NVIDIA's "
                 "default leaves it to the driver. A DLSS override set in the NVIDIA App takes "
                 "precedence.\n\nOnly used when DLSS upscales (greyed out while FSR does).");
        }
        ImGui::EndDisabled();
        DrawFrameGenSettings();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Status")) {
        ImGui::SeparatorText("Upscaling");
        ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
        ImGui::TextDisabled("FSR: %s", fsr::Status().c_str());
        ImGui::TextDisabled("Texture mip bias: %.2f (%u samplers)", g_mipBias, g_samplersBiased.load());
        std::string err;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            err = g_lastError;
        }
        if (g_set.enabled && !err.empty()) ImGui::TextColored(kWarn, "Last problem: %s", err.c_str());
        DrawFrameGenStatus();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
        ImGui::SeparatorText("Upscaling");
        changed |= ImGui::Checkbox("Upscale below 100% render resolution", &g_set.upscale);
        Help("On: below 100%, DLSS/FSR upscale to the screen and the game's post-processing runs at full "
             "resolution. Off: DLSS/FSR anti-alias at the render size and the game stretches the picture.");
        if (kSharpenPs > 2 && kSharpenOptional) {
            changed |= ImGui::Checkbox("Game sharpening when upscaling", &g_set.sharpen);
            Help("Below 100%, the game's sharpening (its Sharpness setting) runs on the upscaled image. Off: "
                 "only DLSS/FSR's own result.");
        }
        changed |= ImGui::Checkbox("Sharper textures at lower render resolutions", &g_set.textureDetail);
        Help("A mip bias of log2(render scale) on the game's material textures, as NVIDIA recommends. Set when "
             "the game starts: restart after changing the render resolution.");
        changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
        Help("Testing only: the sign of the camera jitter given to DLSS and FSR. Leave both off.");
        DrawFrameGenAdvanced();
        ImGui::SeparatorText("Development");
        DrawCaptureTool();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (changed) SaveSettings();
}

void UpscalerInit() {
    LoadSettings();
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
    reshade::register_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
    reshade::register_event<reshade::addon_event::bind_scissor_rects>(&OnBindScissors);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::create_sampler>(&OnCreateSampler);
    reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
    RegisterFrameGen();
    const float scale = g_set.enabled && g_set.mode > 0 ? kModes[g_set.mode].scale : StartupRenderScale();
    if (g_set.enabled && g_set.upscale && g_set.textureDetail && scale < 0.99f) g_mipBias = std::log2(scale);
    Log("texture mip bias %.2f (Image Quality %.2f at start)", g_mipBias, scale);
    Log("Upscaling %s at start (F9 toggles), upscaler setting %d (0 auto, 1 DLSS, 2 FSR), upscaling below 100%% %s",
        g_set.enabled ? "on" : "off", g_set.upscaler, g_set.upscale ? "on" : "off");
}

void UpscalerShutdown() {
    reshade::unregister_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
    reshade::unregister_event<reshade::addon_event::bind_viewports>(&OnBindViewports);
    reshade::unregister_event<reshade::addon_event::bind_scissor_rects>(&OnBindScissors);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::create_sampler>(&OnCreateSampler);
    reshade::unregister_event<reshade::addon_event::barrier>(&OnBarrier);
    UnregisterFrameGen();
}

}  // namespace rd
