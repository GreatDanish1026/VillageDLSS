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
#include "guide_cs.h"       // kGuideCs: guide.hlsl (Ray Reconstruction guides from the G-buffer)
#include "guide_view_cs.h"  // kGuideViewCs: guide_view.hlsl (guide debug view)
#include "rcas_cs.h"        // kRcasCs: rcas.hlsl (sharpening after DLSS)
#include "dlss.hpp"
#include "fsr.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

// Resident Evil Village's shaders (docs/FINDINGS.md).
constexpr uint32_t kTaaPs = 0xcf113c28;      // PreTonemap2_PS: t0 depth, t1 colour, t2 history, t3 velocity
constexpr uint32_t kTaaDepth = 0, kTaaColor = 1, kTaaMotion = 3;
// (No effect mask in this game's TAA.)
constexpr int kTaaMask = -1;
constexpr uint32_t kBloomPs = 0x77ae4c63;    // NewFinal
constexpr bool kBloomIntoTaaOnly = false;     // (RE7's bloom shader also draws its smaller levels)
// Village: the bloom is drawn into X as well, and the game's draw still goes into its render-size
// image (as in 1.0). Moving it to X instead left that image without bloom and changed the
// picture's tone when upscaling (1.1.0 test: the game reads it again, likely for its exposure).
constexpr bool kBloomAlsoAtRenderSize = true;
constexpr uint32_t kTonemapPs = 0x3cc6333b;  // LDRPostProcess_WithTonemap (t0 depth, colour t1)
constexpr uint32_t kTonemapPs2 = 4;            // (RE2/RE3: the other permutation; none known here)
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
    bool gameExposure = true;   // DLSS/FSR get the colour's real exposure (1.0) instead of guessing it
    float sharpness = 0;        // extra sharpening after DLSS/FSR (RCAS), 0 = off
    bool fullResReflections = false;  // REFramework script: ray traced reflections at full resolution
    bool effectMask = true;     // DLSS/FSR get the TAA's effect mask (games that have one)
    int mode = 0;          // DLSS mode setting the render scale through VillageDLSS.lua (0: the game's slider)
    // DLSS Ray Reconstruction in place of Super Resolution while the game ray traces (ini
    // RayReconstruction; off by default). rrPreset: NVSDK_NGX_RayReconstruction_Hint_Render_Preset_*
    // (ini RrModel, 0 = NVIDIA's default). rrDenoise (ini RrDenoise): which of the game's ray tracing
    // denoise passes are skipped: 0 none, 1 all but the spatial ones (they fill the sparse rays), 2 all.
    // guideView: debug view of one guide (not saved).
    bool rr = false;
    int rrPreset = 0;
    int rrDenoise = 0;
    int guideView = 0;
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
    reshade::get_config_value(nullptr, RD_PRODUCT, "GameExposure", g_set.gameExposure);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Sharpness", g_set.sharpness);
    if (!(g_set.sharpness >= 0 && g_set.sharpness <= 1)) g_set.sharpness = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "FullResReflections", g_set.fullResReflections);
    reshade::get_config_value(nullptr, RD_PRODUCT, "EffectMask", g_set.effectMask);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
    if (g_set.mode < 0 || g_set.mode >= kModeCount) g_set.mode = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "RayReconstruction", g_set.rr);
    reshade::get_config_value(nullptr, RD_PRODUCT, "RrModel", g_set.rrPreset);
    if (g_set.rrPreset != 0 && (g_set.rrPreset < 4 || g_set.rrPreset > 6)) g_set.rrPreset = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "RrDenoise", g_set.rrDenoise);
    if (g_set.rrDenoise < 0 || g_set.rrDenoise > 2) g_set.rrDenoise = 0;
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
    reshade::set_config_value(nullptr, RD_PRODUCT, "GameExposure", g_set.gameExposure);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Sharpness", g_set.sharpness);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FullResReflections", g_set.fullResReflections);
    reshade::set_config_value(nullptr, RD_PRODUCT, "EffectMask", g_set.effectMask);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Mode", g_set.mode);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RayReconstruction", g_set.rr);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RrModel", g_set.rrPreset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RrDenoise", g_set.rrDenoise);
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

// ---------------------------------------------------------------- reading constant buffers
//
// Constant buffers (SceneInfo) are read on the CPU when a pass is recorded. Usually they're in
// upload memory and can be mapped. Some systems (seen on Radeon cards: "camera constants
// (SceneInfo) not found") keep them in GPU-only memory, filled by a copy from upload memory:
// once a buffer can't be mapped, buffer copies are noted, and such a buffer is read from the
// upload buffer it was copied from (as in RE4DLSS 1.2.2).

std::atomic<bool> g_trackCopies{false};
struct CopySource {
    uint64_t size;
    resource src;
    uint64_t srcOffset;
};
std::mutex g_copyMutex;
std::map<uint64_t, std::map<uint64_t, CopySource>> g_copies;  // destination buffer -> its offset -> source
std::map<uint64_t, bool> g_copySources;                        // the source buffers seen
size_t g_copyCount = 0;

void NoteCopy(resource src, uint64_t srcOffset, resource dst, uint64_t dstOffset, uint64_t size) {
    std::lock_guard<std::mutex> lock(g_copyMutex);
    if (g_copyCount > 65536) {
        g_copies.clear();
        g_copySources.clear();
        g_copyCount = 0;
    }
    if (g_copies[dst.handle].insert_or_assign(dstOffset, CopySource{size, src, srcOffset}).second) ++g_copyCount;
    g_copySources[src.handle] = true;
}

bool CbOnCopyBuffer(command_list*, resource src, uint64_t srcOffset, resource dst, uint64_t dstOffset, uint64_t size) {
    if (g_trackCopies.load(std::memory_order_relaxed)) NoteCopy(src, srcOffset, dst, dstOffset, size);
    return false;
}

bool CbOnCopyResource(command_list* cl, resource src, resource dst) {
    if (!g_trackCopies.load(std::memory_order_relaxed)) return false;
    device* dev = cl->get_device();
    const resource_desc a = dev->get_resource_desc(src), b = dev->get_resource_desc(dst);
    if (a.type == resource_type::buffer && b.type == resource_type::buffer) NoteCopy(src, 0, dst, 0, b.buffer.size);
    return false;
}

// A destroyed buffer mustn't be mapped later: forget copies into it, and all of them if it was a source.
void CbOnDestroyResource(device*, resource r) {
    if (!g_trackCopies.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_copyMutex);
    if (g_copySources.count(r.handle)) {
        g_copies.clear();
        g_copySources.clear();
        g_copyCount = 0;
    } else if (auto it = g_copies.find(r.handle); it != g_copies.end()) {
        g_copyCount -= it->second.size();
        g_copies.erase(it);
    }
}

bool CopySourceOf(const buffer_range& r, uint64_t bytes, resource* src, uint64_t* srcOffset) {
    std::lock_guard<std::mutex> lock(g_copyMutex);
    const auto b = g_copies.find(r.buffer.handle);
    if (b == g_copies.end()) return false;
    auto it = b->second.upper_bound(r.offset);
    if (it == b->second.begin()) return false;
    --it;
    const uint64_t end = it->second.size == UINT64_MAX ? UINT64_MAX : it->first + it->second.size;
    if (r.offset + bytes > end) return false;
    *src = it->second.src;
    *srcOffset = it->second.srcOffset + (r.offset - it->first);
    return true;
}

// Reads <bytes> of a buffer: mapped when the CPU can, else through the copy that filled it.
// *how says which (or why neither worked).
bool ReadBuffer(device* dev, const buffer_range& r, uint32_t bytes, void* out, std::string* how) {
    const resource_desc d = dev->get_resource_desc(r.buffer);
    char msg[200];
    void* data = nullptr;
    if (d.heap != memory_heap::default_ && dev->map_buffer_region(r.buffer, r.offset, bytes, map_access::read_only, &data) && data) {
        memcpy(out, data, bytes);
        dev->unmap_buffer_region(r.buffer);
        snprintf(msg, sizeof(msg), "mapped (memory heap %d)", static_cast<int>(d.heap));
        *how = msg;
        return true;
    }
    g_trackCopies = true;
    resource src = {0};
    uint64_t srcOffset = 0;
    if (CopySourceOf(r, bytes, &src, &srcOffset)) {
        const resource_desc sd = dev->get_resource_desc(src);
        data = nullptr;
        if (sd.heap != memory_heap::default_ && dev->map_buffer_region(src, srcOffset, bytes, map_access::read_only, &data) && data) {
            memcpy(out, data, bytes);
            dev->unmap_buffer_region(src);
            snprintf(msg, sizeof(msg), "memory heap %d, read from the buffer copied into it (heap %d)",
                     static_cast<int>(d.heap), static_cast<int>(sd.heap));
            *how = msg;
            return true;
        }
        snprintf(msg, sizeof(msg), "memory heap %d can't be read, nor the buffer copied into it (heap %d)",
                 static_cast<int>(d.heap), static_cast<int>(sd.heap));
        *how = msg;
        return false;
    }
    snprintf(msg, sizeof(msg), "memory heap %d (buffer size %llu) can't be read, and no copy into it was seen",
             static_cast<int>(d.heap), static_cast<unsigned long long>(d.buffer.size));
    *how = msg;
    return false;
}

// Where SceneInfo came from, logged when that changes (and why it couldn't be read).
void NoteSceneInfoRead(const std::string& how, bool ok) {
    static std::mutex m;
    static std::string last;
    std::lock_guard<std::mutex> lock(m);
    const std::string now = (ok ? "camera constants (SceneInfo): " : "camera constants (SceneInfo) not readable: ") + how;
    if (now == last) return;
    last = now;
    Log("%s", now.c_str());
}

bool ReadSceneInfo(device* dev, const StageState& st, float* out) {
    std::string how = "no SceneInfo constant buffer bound";
    for (uint32_t param : SceneInfoParams(dev, st)) {
        const buffer_range& b = st.roots.at(param).range;
        if (ReadBuffer(dev, b, kSceneInfoFloats * 4, out, &how)) {
            NoteSceneInfoRead(how, true);
            return true;
        }
    }
    NoteSceneInfoRead(how, false);
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

// ---------------------------------------------------------------- Ray Reconstruction
//
// The game ray traces in compute shaders at half the render size and denoises the result itself
// (docs/FINDINGS.md "Ray tracing chain"); rayTracingUpscale then adds it onto the scene colour at
// full size, before the TAA. For Ray Reconstruction the game's denoise passes are skipped and the
// signals from before them (the shading's own, or after the spatial passes that fill the sparse
// rays) are copied into the textures rayTracingUpscale reads. At the TAA, guides are made from
// the G-buffer that rayTracingUpscale reads, and DLSS-D runs in place of DLSS.

// The game's ray tracing passes (docs/FINDINGS.md; names from RE2's DXIL, RE2DLSS docs/shaders/rt).
// Village runs RE3/RE7's chain (no VFX signal: rayTracingUpscale reads three signals) with its own
// shader builds and extra passes (ray binning, a denoise work queue); names and registers from the
// dumps in docs/shaders/rt (RE3 id in brackets).
constexpr uint32_t kRtShadeDiffuse = 0x7afe99a0;     // rayTracingShading: u0 shY, u1 cocg (fc23ed5b)
constexpr uint32_t kRtShadeSpecular = 0x244a4781;    // rayTracingShading: u0 specular (de6f8914)
constexpr uint32_t kRtShadeVfx = 4;                  // (RE2 only)
constexpr uint32_t kRtSpatialDiffuse = 0x7f4c08ac;   // rayTracingSpatialAccumulation: u0 shY, u1 cocg (d6792963)
constexpr uint32_t kRtSpatialSpecular = 0x8e128757;  // rayTracingSpatialAccumulation: u0 specular (25501ade)
constexpr uint32_t kRtSpatialVfx = 5;                // (RE2 only)
constexpr uint32_t kRtVfxMedian = 6;                 // (RE2 only)
constexpr uint32_t kRtMeanVariance = 0x01ea4198;     // rayTracingMeanVariance (0af21e0f)
constexpr uint32_t kRtTemporalSpecular = 0x946de26d;  // rayTracingTemporalAccumulation, specular (a4fd0f93)
constexpr uint32_t kRtTemporalDiffuse = 0x9f40a58c;   // rayTracingTemporalAccumulation, diffuse (94984fd1)
constexpr uint32_t kRtDeflicker = 0xc374e480;         // rayTracingDeflickerSpecular (091f2a20)
constexpr uint32_t kRtBilateral = 0x9514a55e;         // rayTracingBilateralFilter (905f94bf)
constexpr uint32_t kRtTemporalVfx = 7;                // (RE2 only)
constexpr uint32_t kRtUpscale = 0x4206d838;           // rayTracingUpscale: t2 base colour, t3 normal, t5-t7 signals (4e928452)
constexpr uint32_t kUpBase = 2, kUpNormal = 3, kUpSignal = 5;  // t5 shY, t6 cocg, t7 specular
constexpr uint32_t kRtSignals = 3;

const char* const kRrDenoiseNames[] = {"Game's denoiser, Ray Reconstruction on top (recommended)",
                                       "Skip all but the spatial passes (experimental)", "Skip all: raw rays (experimental)"};
const char* const kGuideViewNames[] = {"Off", "Diffuse albedo", "Specular albedo", "Normals", "Roughness"};
constexpr int kGuideViewCount = 5;

std::mutex g_rtMutex;  // everything in this section
struct RtState {
    uint64_t signalFrame = UINT64_MAX;  // g_frame when the signals below were seen
    resource signal[4] = {};            // shY, cocg, specular, VFX specular: what replaces the denoised ones
    uint64_t upscaleFrame = UINT64_MAX;  // g_frame when rayTracingUpscale ran
    resource base = {0}, normal = {0};   // its G-buffer
    uint64_t rrFrame = UINT64_MAX;       // g_frame when Ray Reconstruction last ran
    uint64_t rrFrames = 0, skipped = 0, copied = 0;
    std::string error;                   // why Ray Reconstruction isn't running (overlay)
    std::map<uint64_t, resource_usage> states;  // last recorded state of the textures above
    std::map<std::pair<uint64_t, uint32_t>, resource_view> views;  // ours, by resource and kind/format
    // Guides (render size), ours.
    resource guide[3] = {};
    resource_view guideUav[3] = {};
    uint32_t guideW = 0, guideH = 0;
    pipeline_layout layout = {0}, viewLayout = {0};
    pipeline pipe = {0}, viewPipe = {0};
    bool pipesFailed = false;
    bool loggedMatrices = false;
} g_rt;

void RtWatch(resource r) {
    if (r.handle && !g_rt.states.count(r.handle)) g_rt.states[r.handle] = resource_usage::undefined;
}

void RtOnBarrier(uint32_t count, const resource* res, const resource_usage* after) {
    std::lock_guard<std::mutex> lock(g_rtMutex);
    if (g_rt.states.empty()) return;
    for (uint32_t i = 0; i < count; ++i) {
        auto it = g_rt.states.find(res[i].handle);
        if (it != g_rt.states.end()) it->second = after[i];
    }
}

void RtOnDestroyResource(device* dev, resource r) {
    std::lock_guard<std::mutex> lock(g_rtMutex);
    g_rt.states.erase(r.handle);
    for (auto it = g_rt.views.begin(); it != g_rt.views.end();) {
        if (it->first.first == r.handle) {
            dev->destroy_resource_view(it->second);
            it = g_rt.views.erase(it);
        } else {
            ++it;
        }
    }
    for (resource& s : g_rt.signal)
        if (s == r) s = {0};
    if (g_rt.base == r) g_rt.base = {0};
    if (g_rt.normal == r) g_rt.normal = {0};
}

// A view of ours on <r> (cached until the resource goes).
resource_view RtView(device* dev, resource r, format f, bool uav) {
    const auto key = std::make_pair(r.handle, (static_cast<uint32_t>(f) << 1) | (uav ? 1u : 0u));
    auto it = g_rt.views.find(key);
    if (it != g_rt.views.end()) return it->second;
    resource_view v = {0};
    if (!dev->create_resource_view(r, uav ? resource_usage::unordered_access : resource_usage::shader_resource,
                                   resource_view_desc(f), &v))
        return {0};
    g_rt.views[key] = v;
    return v;
}

// The resource at register <reg> (t# or u#) of the bound compute tables.
resource ComputeView(device* dev, const StageState& st, uint32_t reg, bool uav) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
    for (uint32_t i = 0; i < st.tables.size(); ++i) {
        if (!st.tables[i].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, i);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            const bool isUav = range.type == descriptor_type::unordered_access_view ||
                               range.type == descriptor_type::texture_unordered_access_view;
            const bool isSrv = range.type == descriptor_type::shader_resource_view ||
                               range.type == descriptor_type::texture_shader_resource_view;
            if ((uav ? !isUav : !isSrv) || range.dx_register_space != 0) continue;
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

// Ray Reconstruction is chosen and can run with the upscaler in use.
bool RrChosen() {
    std::string why;
    return g_set.enabled && g_set.rr && g_active == Tech::Dlss && dlss::RrAvailable(&why);
}

// The game's denoise passes are skipped while Ray Reconstruction ran last frame (so the first frame
// with it, and every frame after it stops, keeps the game's).
bool RtSkipping(uint64_t frame) {
    return RrChosen() && g_set.rrDenoise > 0 && g_rt.rrFrame != UINT64_MAX && g_rt.rrFrame + 2 >= frame;
}

bool RtIsSkipped(uint32_t id) {
    switch (id) {
        case kRtMeanVariance: case kRtTemporalSpecular: case kRtTemporalDiffuse: case kRtDeflicker: case kRtBilateral:
        case kRtTemporalVfx:
            return true;
        case kRtSpatialDiffuse: case kRtSpatialSpecular: case kRtSpatialVfx: case kRtVfxMedian:
            return g_set.rrDenoise == 2;
        default:
            return false;
    }
}

// Copies <src> over <dst> when they match, in their recorded states (left as they were).
bool RtCopy(command_list* cl, resource src, resource dst) {
    device* dev = cl->get_device();
    if (!src.handle || !dst.handle || src == dst) return false;
    const resource_desc sd = dev->get_resource_desc(src), dd = dev->get_resource_desc(dst);
    if (sd.texture.width != dd.texture.width || sd.texture.height != dd.texture.height ||
        sd.texture.format != dd.texture.format) {
        static int logged = 0;
        if (logged++ < 4)
            Log("Ray Reconstruction: signal %ux%u format %u doesn't match the game's %ux%u format %u", sd.texture.width,
                sd.texture.height, static_cast<unsigned>(sd.texture.format), dd.texture.width, dd.texture.height,
                static_cast<unsigned>(dd.texture.format));
        return false;
    }
    auto si = g_rt.states.find(src.handle), di = g_rt.states.find(dst.handle);
    const resource_usage ss = si != g_rt.states.end() ? si->second : resource_usage::undefined;
    // rayTracingUpscale is about to read dst: an SRV state, non-pixel if nothing better is known.
    resource_usage ds = di != g_rt.states.end() ? di->second : resource_usage::undefined;
    if (ds == resource_usage::undefined) ds = resource_usage::shader_resource_non_pixel;
    if (ss == resource_usage::undefined) return false;  // not seen in a barrier yet: next frame
    cl->barrier(src, ss, resource_usage::copy_source);
    cl->barrier(dst, ds, resource_usage::copy_dest);
    cl->copy_resource(src, dst);
    cl->barrier(src, resource_usage::copy_source, ss);
    cl->barrier(dst, resource_usage::copy_dest, ds);
    return true;
}

// Every compute dispatch: notes the signals and the G-buffer, skips the game's denoise passes, and
// copies the signals in before rayTracingUpscale. True: the dispatch is skipped.
bool RtOnDispatch(command_list* cl, ListState& s, uint32_t id) {
    if (!g_set.rr || !g_set.enabled) return false;
    device* dev = cl->get_device();
    uint64_t frame;
    {
        std::lock_guard<std::mutex> lock(g_resMutex);
        frame = g_frame;
    }
    std::lock_guard<std::mutex> lock(g_rtMutex);
    const bool raw = g_set.rrDenoise == 2;
    auto note = [&](int slot, resource r) {
        if (g_rt.signalFrame != frame) {
            for (resource& x : g_rt.signal) x = {0};
            g_rt.signalFrame = frame;
        }
        g_rt.signal[slot] = r;
        RtWatch(r);
    };
    switch (id) {
        case kRtShadeDiffuse: if (raw) note(0, ComputeView(dev, s.compute, 0, true)), note(1, ComputeView(dev, s.compute, 1, true)); break;
        case kRtShadeSpecular: if (raw) note(2, ComputeView(dev, s.compute, 0, true)); break;
        case kRtShadeVfx: if (raw) note(3, ComputeView(dev, s.compute, 0, true)); break;
        case kRtSpatialDiffuse: if (!raw) note(0, ComputeView(dev, s.compute, 0, true)), note(1, ComputeView(dev, s.compute, 1, true)); break;
        case kRtSpatialSpecular: if (!raw) note(2, ComputeView(dev, s.compute, 0, true)); break;
        case kRtSpatialVfx: if (!raw) note(3, ComputeView(dev, s.compute, 0, true)); break;
        default: break;
    }
    const bool skipping = RtSkipping(frame);
    if (id == kRtUpscale) {
        g_rt.upscaleFrame = frame;
        g_rt.base = ComputeView(dev, s.compute, kUpBase, false);
        g_rt.normal = ComputeView(dev, s.compute, kUpNormal, false);
        if (skipping && g_rt.signalFrame == frame) {
            int copied = 0;
            for (uint32_t i = 0; i < kRtSignals; ++i) {
                const resource dst = ComputeView(dev, s.compute, kUpSignal + i, false);
                RtWatch(dst);
                copied += RtCopy(cl, g_rt.signal[i], dst);
            }
            if (copied && !g_rt.copied++)
                Log("Ray Reconstruction: %d of %u ray tracing signals handed over un-denoised", copied, kRtSignals);
        }
        return false;
    }
    if (skipping && RtIsSkipped(id)) {
        if (!g_rt.skipped++) Log("Ray Reconstruction: skipping the game's ray tracing denoise passes (%s)", kRrDenoiseNames[g_set.rrDenoise]);
        return true;
    }
    return false;
}

bool RtCreatePipelines(device* dev) {
    if (g_rt.pipe.handle) return true;
    if (g_rt.pipesFailed) return false;
    constant_range c = {};
    c.count = 24;
    c.visibility = shader_stage::all_compute;
    descriptor_range srv = {};
    srv.count = 3;
    srv.visibility = shader_stage::all_compute;
    srv.type = descriptor_type::shader_resource_view;
    descriptor_range uav = srv;
    uav.type = descriptor_type::unordered_access_view;
    const pipeline_layout_param params[3] = {pipeline_layout_param(c), pipeline_layout_param(srv), pipeline_layout_param(uav)};
    shader_desc cs = {};
    cs.code = kGuideCs;
    cs.code_size = sizeof(kGuideCs);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    constant_range vc = c;
    vc.count = 8;
    descriptor_range vsrv = srv, vuav = uav;
    vsrv.count = 1, vuav.count = 1;
    const pipeline_layout_param vparams[3] = {pipeline_layout_param(vc), pipeline_layout_param(vsrv), pipeline_layout_param(vuav)};
    shader_desc vcs = {};
    vcs.code = kGuideViewCs;
    vcs.code_size = sizeof(kGuideViewCs);
    pipeline_subobject vsub = {pipeline_subobject_type::compute_shader, 1, &vcs};
    if (!dev->create_pipeline_layout(3, params, &g_rt.layout) || !dev->create_pipeline(g_rt.layout, 1, &sub, &g_rt.pipe) ||
        !dev->create_pipeline_layout(3, vparams, &g_rt.viewLayout) ||
        !dev->create_pipeline(g_rt.viewLayout, 1, &vsub, &g_rt.viewPipe)) {
        g_rt.pipesFailed = true;
        Log("couldn't create the Ray Reconstruction guide shader");
        return false;
    }
    Log("Ray Reconstruction guide shader created");
    return true;
}

// The caller holds g_resMutex (g_old) and g_rtMutex.
bool RtGuideTextures(device* dev, uint32_t w, uint32_t h) {
    if (g_rt.guide[0].handle && g_rt.guideW == w && g_rt.guideH == h) return true;
    const format fmts[3] = {format::r8g8b8a8_unorm, format::r8g8b8a8_unorm, format::r16g16b16a16_float};
    for (int i = 0; i < 3; ++i)
        if (g_rt.guide[i].handle) g_old.push_back({g_rt.guide[i], {0}, g_rt.guideUav[i], g_frame});  // the GPU may still use them
    for (int i = 0; i < 3; ++i) {
        g_rt.guide[i] = {0}, g_rt.guideUav[i] = {0};
        const resource_desc d(w, h, 1, 1, fmts[i], 1, memory_heap::default_,
                              resource_usage::shader_resource | resource_usage::unordered_access);
        if (!dev->create_resource(d, nullptr, resource_usage::shader_resource_non_pixel, &g_rt.guide[i]) ||
            !dev->create_resource_view(g_rt.guide[i], resource_usage::unordered_access, resource_view_desc(fmts[i]),
                                       &g_rt.guideUav[i])) {
            Log("couldn't create the Ray Reconstruction guide textures %ux%u", w, h);
            return false;
        }
    }
    g_rt.guideW = w, g_rt.guideH = h;
    Log("Ray Reconstruction guide textures %ux%u", w, h);
    return true;
}

// The G-buffer's shader resource format: sRGB base colour decodes to linear.
// A depth view format for <f>: RE2/RE3 have 32-bit depth with stencil, RE7 plain 32-bit depth (a
// view in the wrong one lost the device).
format RtDepthSrvFormat(format f) {
    switch (f) {
        case format::r32_g8_typeless: case format::d32_float_s8_uint: case format::r32_float_x8_uint:
            return format::r32_float_x8_uint;
        case format::r32_typeless: case format::d32_float: case format::r32_float:
            return format::r32_float;
        case format::r24_g8_typeless: case format::d24_unorm_s8_uint: case format::r24_unorm_x8_uint:
            return format::r24_unorm_x8_uint;
        case format::r16_typeless: case format::d16_unorm: case format::r16_unorm:
            return format::r16_unorm;
        default:
            return format::unknown;
    }
}

format RtSrvFormat(format f) {
    if (f == format::r8g8b8a8_typeless) return format::r8g8b8a8_unorm_srgb;
    if (f == format::b8g8r8a8_typeless) return format::b8g8r8a8_unorm_srgb;
    return format_to_default_typed(f, 0);
}

// 4x4 inverse (row-major); false if singular.
bool Inverse4(const float* m, float* out) {
    float a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12f) return false;
        if (p != c)
            for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        const float inv = 1 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const float f = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
}

// Ray Reconstruction's camera: world-to-view from SceneInfo.transposeViewMat (byte 64, the view
// matrix's columns as rows) and view-to-clip = inverse(world-to-view) * viewProjMat.
// A projection: no x/y crosstalk (row-vector form: [0][1], [0][2], [1][0], [1][2] are 0).
bool LooksLikeProjection(const float* p) {
    const float s = std::fmax(std::fabs(p[0]), std::fabs(p[5]));
    return s > 0.05f && std::fabs(p[1]) < s * 1e-3f && std::fabs(p[2]) < s * 1e-3f && std::fabs(p[4]) < s * 1e-3f &&
           std::fabs(p[6]) < s * 1e-3f;
}

bool RtMatrices(const float* scene, float* worldToView, float* viewToClip) {
    const float* t = scene + 16;  // 3 rows of 4
    const float* vp = scene + kViewProj;
    // The view matrix's columns as rows (as the name says); if that doesn't give a projection, rows.
    static int layout = 0;  // 0 transposed, 1 as is (found on the first frame that fits)
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int l = attempt == 0 ? layout : 1 - layout;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float v = r == 3 && c == 3 ? 1.0f : 0.0f;
                if (c < 3 && r < 4) v = l == 0 ? t[c * 4 + r] : (r < 3 ? t[r * 4 + c] : t[c * 4 + 3]);
                worldToView[r * 4 + c] = v;
            }
        float inv[16];
        if (!Inverse4(worldToView, inv)) continue;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += inv[r * 4 + k] * vp[k * 4 + c];
                viewToClip[r * 4 + c] = sum;
            }
        if (LooksLikeProjection(viewToClip)) {
            if (l != layout) Log("Ray Reconstruction camera: SceneInfo.transposeViewMat read %s", l ? "as rows" : "as columns");
            layout = l;
            break;
        }
        if (attempt == 1) {
            static int logged = 0;
            if (logged++ < 4) Log("Ray Reconstruction camera: neither reading of transposeViewMat gives a projection");
            return false;
        }
    }
    // The rotation must be orthonormal.
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i)
        for (int j = 0; j < 3; ++j) {
            float d = 0;
            for (int k = 0; k < 3; ++k) d += worldToView[i * 4 + k] * worldToView[j * 4 + k];
            if (std::fabs(d - (i == j ? 1.0f : 0.0f)) > 5e-3f) ok = false;
        }
    if (!g_rt.loggedMatrices || !ok) {
        static int logged = 0;
        if (logged++ < 4) {
            const float* w = worldToView;
            const float* p = viewToClip;
            Log("Ray Reconstruction camera %s: world-to-view rows (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f "
                "%.3f %.3f) (%.3f %.3f %.3f %.3f); view-to-clip rows (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f "
                "%.4f %.4f %.4f) (%.4f %.4f %.4f %.4f)",
                ok ? "ok" : "NOT orthonormal", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                w[12], w[13], w[14], w[15], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
                p[12], p[13], p[14], p[15]);
        }
        g_rt.loggedMatrices = true;
    }
    return ok;
}

// Records guide.hlsl on the TAA's list (the caller puts the game's state back after DLSS). The
// G-buffer is in a shader resource state at the TAA (the lighting passes read it).
bool RtBuildGuides(command_list* cl, resource depth, const float* scene, uint32_t w, uint32_t h) {
    device* dev = cl->get_device();
    if (!g_rt.base.handle || !g_rt.normal.handle) return g_rt.error = "G-buffer not seen", false;
    const resource_desc bd = dev->get_resource_desc(g_rt.base), nd = dev->get_resource_desc(g_rt.normal);
    if (bd.texture.width < w || bd.texture.height < h || nd.texture.width < w || nd.texture.height < h)
        return g_rt.error = "G-buffer smaller than the render size", false;
    const format depthFormat = RtDepthSrvFormat(dev->get_resource_desc(depth).texture.format);
    if (depthFormat == format::unknown) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("Ray Reconstruction: depth format %u not handled",
                static_cast<unsigned>(dev->get_resource_desc(depth).texture.format));
        }
        return g_rt.error = "depth format not handled (see the log)", false;
    }
    if (!RtCreatePipelines(dev) || !RtGuideTextures(dev, w, h)) return g_rt.error = "guide shader or textures failed", false;
    const resource_view srvs[3] = {RtView(dev, depth, depthFormat, false),
                                   RtView(dev, g_rt.base, RtSrvFormat(bd.texture.format), false),
                                   RtView(dev, g_rt.normal, RtSrvFormat(nd.texture.format), false)};
    for (const resource_view& v : srvs)
        if (!v.handle) return g_rt.error = "couldn't read the G-buffer (views)", false;
    static bool loggedFormats = false;
    if (!loggedFormats) {
        loggedFormats = true;
        Log("Ray Reconstruction G-buffer: base colour %ux%u format %u, normal %ux%u format %u, depth view format %u",
            bd.texture.width, bd.texture.height, static_cast<unsigned>(bd.texture.format), nd.texture.width,
            nd.texture.height, static_cast<unsigned>(nd.texture.format), static_cast<unsigned>(depthFormat));
    }
    float params[24] = {};
    memcpy(params, scene + 56, 16 * sizeof(float));  // viewProjInvMat, byte 224
    const float* tvi = scene + 28;                     // transposeViewInvMat, byte 112: .w of each row = camera
    params[16] = tvi[3], params[17] = tvi[7], params[18] = tvi[11];
    params[20] = static_cast<float>(w), params[21] = static_cast<float>(h), params[22] = 1.0f / w, params[23] = 1.0f / h;
    const resource_usage from[3] = {resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel,
                                    resource_usage::shader_resource_non_pixel};
    const resource_usage to[3] = {resource_usage::unordered_access, resource_usage::unordered_access,
                                  resource_usage::unordered_access};
    cl->barrier(3, g_rt.guide, from, to);
    cl->bind_pipeline(pipeline_stage::all_compute, g_rt.pipe);
    cl->push_constants(shader_stage::all_compute, g_rt.layout, 0, 0, 24, params);
    descriptor_table_update u = {};
    u.count = 3;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = srvs;
    cl->push_descriptors(shader_stage::all_compute, g_rt.layout, 1, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = g_rt.guideUav;
    cl->push_descriptors(shader_stage::all_compute, g_rt.layout, 2, u);
    cl->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl->barrier(3, g_rt.guide, to, from);
    return true;
}

// Debug view: guide <which> (1 diffuse, 2 specular, 3 normals, 4 roughness) over <output> (UAV state).
void RtShowGuide(command_list* cl, resource output, int which, uint32_t renderW, uint32_t renderH) {
    device* dev = cl->get_device();
    if (!g_rt.viewPipe.handle || which < 1 || which > 4) return;
    const resource g = g_rt.guide[which == 1 ? 0 : which == 2 ? 1 : 2];
    const resource_desc od = dev->get_resource_desc(output);
    const resource_view srv = RtView(dev, g, which <= 2 ? format::r8g8b8a8_unorm : format::r16g16b16a16_float, false);
    const resource_view uav = RtView(dev, output, od.texture.format, true);
    if (!srv.handle || !uav.handle) return;
    float params[8] = {static_cast<float>(od.texture.width), static_cast<float>(od.texture.height),
                       static_cast<float>(renderW) / od.texture.width, static_cast<float>(renderH) / od.texture.height};
    const uint32_t mode = which <= 2 ? 1 : which == 3 ? 2 : 3;
    memcpy(&params[4], &mode, 4);
    cl->bind_pipeline(pipeline_stage::all_compute, g_rt.viewPipe);
    cl->push_constants(shader_stage::all_compute, g_rt.viewLayout, 0, 0, 8, params);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = &srv;
    cl->push_descriptors(shader_stage::all_compute, g_rt.viewLayout, 1, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = &uav;
    cl->push_descriptors(shader_stage::all_compute, g_rt.viewLayout, 2, u);
    cl->dispatch((od.texture.width + 7) / 8, (od.texture.height + 7) / 8, 1);
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
}

void RtDestroy(device* dev) {
    std::lock_guard<std::mutex> lock(g_rtMutex);
    for (auto& [k, v] : g_rt.views) dev->destroy_resource_view(v);
    for (int i = 0; i < 3; ++i) {
        if (g_rt.guideUav[i].handle) dev->destroy_resource_view(g_rt.guideUav[i]);
        if (g_rt.guide[i].handle) dev->destroy_resource(g_rt.guide[i]);
    }
    if (g_rt.pipe.handle) dev->destroy_pipeline(g_rt.pipe);
    if (g_rt.viewPipe.handle) dev->destroy_pipeline(g_rt.viewPipe);
    if (g_rt.layout.handle) dev->destroy_pipeline_layout(g_rt.layout);
    if (g_rt.viewLayout.handle) dev->destroy_pipeline_layout(g_rt.viewLayout);
    g_rt = RtState();
}

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
    RtOnBarrier(count, res, after);
}

// ---------------------------------------------------------------- passes

// The exposure DLSS and FSR are told about: the TAA's colour is already exposed (the tonemapper
// takes it as it is), so 1.0, in a 1x1 r32f texture made once. Without it they estimate an
// exposure of their own (auto-exposure), which can pump and ghost when the lighting changes.
// ---------------------------------------------------------------- sharpening (RCAS)
//
// With the Sharpening slider above 0, DLSS / Ray Reconstruction write into g_xs and AMD's RCAS
// (rcas.hlsl) writes the sharpened image into g_x, where DLSS's output normally goes; everything
// after it is unchanged. FSR uses its own RCAS (the same slider). g_resMutex held.
Texture g_xs;  // DLSS's output before sharpening
pipeline_layout g_rcasLayout = {0};
pipeline g_rcasPipe = {0};
bool g_rcasFailed = false;
resource_view g_xUav = {0};
uint64_t g_xUavOf = 0;

bool RcasReady(device* dev) {
    if (g_rcasPipe.handle) return true;
    if (g_rcasFailed) return false;
    constant_range c = {};
    c.count = 8;
    c.visibility = shader_stage::all_compute;
    descriptor_range srv = {};
    srv.count = 1;
    srv.visibility = shader_stage::all_compute;
    srv.type = descriptor_type::shader_resource_view;
    descriptor_range uav = srv;
    uav.type = descriptor_type::unordered_access_view;
    const pipeline_layout_param params[3] = {pipeline_layout_param(c), pipeline_layout_param(srv), pipeline_layout_param(uav)};
    shader_desc cs = {};
    cs.code = kRcasCs;
    cs.code_size = sizeof(kRcasCs);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline_layout(3, params, &g_rcasLayout) || !dev->create_pipeline(g_rcasLayout, 1, &sub, &g_rcasPipe)) {
        g_rcasFailed = true;
        g_rcasPipe = {0};
        Log("couldn't create the sharpening shader: DLSS isn't sharpened");
        return false;
    }
    Log("sharpening shader (RCAS) created");
    return true;
}

// g_xs (DLSS's output) -> g_x, sharpened.
void Sharpen(command_list* cl, device* dev) {
    if (g_xUavOf != g_x.res.handle) {
        if (g_xUav.handle) dev->destroy_resource_view(g_xUav);
        g_xUav = {0};
        dev->create_resource_view(g_x.res, resource_usage::unordered_access, resource_view_desc(g_x.fmt), &g_xUav);
        g_xUavOf = g_x.res.handle;
    }
    if (!g_xUav.handle || !g_xs.srv.handle) return;
    Transition(cl, g_xs, resource_usage::shader_resource);
    Transition(cl, g_x, resource_usage::unordered_access);
    uint32_t params[8] = {};
    const float sharpness = g_set.sharpness;
    memcpy(&params[0], &sharpness, 4);
    params[4] = g_x.w, params[5] = g_x.h;
    cl->bind_pipeline(pipeline_stage::all_compute, g_rcasPipe);
    cl->push_constants(shader_stage::all_compute, g_rcasLayout, 0, 0, 8, params);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = &g_xs.srv;
    cl->push_descriptors(shader_stage::all_compute, g_rcasLayout, 1, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = &g_xUav;
    cl->push_descriptors(shader_stage::all_compute, g_rcasLayout, 2, u);
    cl->dispatch((g_x.w + 7) / 8, (g_x.h + 7) / 8, 1);
    Transition(cl, g_xs, resource_usage::unordered_access);  // DLSS writes it next frame
    static bool logged = false;
    if (!logged) logged = true, Log("DLSS output sharpened (RCAS, strength %.2f)", sharpness);
}

std::atomic<float> g_renderScale{1};  // render width / output width at the last TAA (1 when not upscaling)
resource g_exposureOne = {0};

ID3D12Resource* ExposureOne(device* dev) {
    if (!g_set.gameExposure) return nullptr;
    if (!g_exposureOne.handle) {
        float one = 1.0f;
        subresource_data data = {&one, 4, 4};
        if (!dev->create_resource(resource_desc(1, 1, 1, 1, format::r32_float, 1, memory_heap::default_,
                                                resource_usage::shader_resource),
                                  &data, resource_usage::shader_resource, &g_exposureOne)) {
            g_exposureOne = {0};
            static bool logged = false;
            if (!logged) logged = true, Log("couldn't create the exposure texture: auto-exposure used");
            return nullptr;
        }
        Log("DLSS/FSR exposure: the game's (1.0, the colour is already exposed)");
    }
    return reinterpret_cast<ID3D12Resource*>(g_exposureOne.handle);
}

// The TAA's effect mask, if this game has one and it's wanted.
ID3D12Resource* EffectMask(device* dev, const ListState& s) {
    if (kTaaMask < 0 || !g_set.effectMask) return nullptr;
    const resource m = PixelSrv(dev, s.graphics, static_cast<uint32_t>(kTaaMask));
    static bool logged = false;
    if (!logged) logged = true, Log("DLSS/FSR effect mask: %s", m.handle ? "the TAA's EffectMaskTex" : "not found");
    return reinterpret_cast<ID3D12Resource*>(m.handle);
}

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
    g_renderScale = g_outW ? static_cast<float>(renderW) / g_outW : 1.0f;

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
    const bool sharpenDlss = tech == Tech::Dlss && g_set.sharpness >= 0.005f && RcasReady(dev) &&
                             Ensure(dev, g_xs, outW, outH, td.texture.format, usage, resource_usage::unordered_access,
                                    "DLSS output before sharpening");
    if (sharpenDlss) Transition(cl, g_xs, resource_usage::unordered_access);
    bool ok;
    bool usedRr = false;
    if (tech == Tech::Dlss) {
        dlss::Inputs in;
        in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
        in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
        in.output = reinterpret_cast<ID3D12Resource*>((sharpenDlss ? g_xs : g_x).res.handle);
        in.key = &g_x;  // one history, sharpened or not
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = 0.5f * renderW;  // NDC offsets, y up -> render pixels, y down
        in.motionScaleY = -0.5f * renderH;
        in.exposure = ExposureOne(dev);
        in.biasMask = EffectMask(dev, s);
        in.reset = reset;
        in.preset = g_set.preset;
        // Ray Reconstruction while the game ray traces (rayTracingUpscale ran in the last second or
        // so: Village skips it on some frames, and switching DLSS features every few frames would
        // restart their history each time).
        std::lock_guard<std::mutex> rtLock(g_rtMutex);
        static float worldToView[16], viewToClip[16];
        constexpr uint64_t kRtGraceFrames = 60;
        if (g_set.rr) {
            std::string why;
            if (!dlss::RrAvailable(&why)) g_rt.error = why;
            else if (g_rt.upscaleFrame == UINT64_MAX || g_rt.upscaleFrame + kRtGraceFrames < g_frame)
                g_rt.error = "the game isn't ray tracing (turn on ray-traced reflections)";
            else if (!RtMatrices(scene, worldToView, viewToClip)) g_rt.error = "camera matrices not as expected (see the log)";
            else if (RtBuildGuides(cl, depth, scene, renderW, renderH)) {
                in.rr = true;
                in.rrPreset = g_set.rrPreset;
                in.diffuseAlbedo = reinterpret_cast<ID3D12Resource*>(g_rt.guide[0].handle);
                in.specularAlbedo = reinterpret_cast<ID3D12Resource*>(g_rt.guide[1].handle);
                in.normalRoughness = reinterpret_cast<ID3D12Resource*>(g_rt.guide[2].handle);
                in.worldToView = worldToView;
                in.viewToClip = viewToClip;
            }
        }
        ok = dlss::Evaluate(list, in, err);
        if (ok && sharpenDlss) Sharpen(cl, dev);
        if (ok && in.rr) {
            usedRr = true;
            g_rt.error.clear();
            g_rt.rrFrame = g_frame;
            if (!g_rt.rrFrames++) Log("DLSS Ray Reconstruction: first frame (%ux%u -> %ux%u)", renderW, renderH, outW, outH);
            if (g_set.guideView > 0) {
                cl->bind_descriptor_tables(shader_stage::all_compute, s.compute.layout, 0, 0, nullptr);  // ReShade's heaps back
                RtShowGuide(cl, g_x.res, g_set.guideView, renderW, renderH);
            }
        }
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
        in.exposure = ExposureOne(dev);
        in.reactive = EffectMask(dev, s);
        const float nearZ = scene[kNearPlane], yScale = scene[kProjElement0 + 1];
        in.cameraNear = nearZ > 0 && nearZ < 10 ? nearZ : 0.01f;
        in.cameraFar = 0;  // RE Engine: reversed Z, infinite far plane
        in.cameraFovY = yScale > 0.05f ? 2 * std::atan(1 / yScale) : 1.0f;
        in.metersPerUnit = 1.0f;  // RE Engine: metres
        in.sharpness = g_set.sharpness;
        in.reset = reset;
        ok = fsr::Evaluate(list, in, err);
    }
    if (!ok) {
        Restore(cl, s);
        Fail(err);
        return false;
    }

    std::string mode = std::string(usedRr ? "DLSS Ray Reconstruction " : tech == Tech::Dlss ? "DLSS " : "FSR ") +
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
    g_up.bloomDone = true;
    if (kBloomAlsoAtRenderSize) {  // drawn into X here; the game's own draw follows as usual
        cl->draw(3, 1, 0, 0);
        RestoreTargets(cl, s);
        return;
    }
    s.redirected = true;
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
    RtDestroy(dev);
    for (auto& [res, rtv] : g_targetRtvs)
        if (rtv.handle) dev->destroy_resource_view(rtv);
    g_targetRtvs.clear();
    dlss::Shutdown();
    fsr::Shutdown();
    for (Texture* t : {&g_x, &g_xs, &g_y, &g_z, &g_hudScene}) Retire(*t);
    if (g_xUav.handle) dev->destroy_resource_view(g_xUav);
    g_xUav = {0}, g_xUavOf = 0;
    if (g_rcasPipe.handle) dev->destroy_pipeline(g_rcasPipe);
    if (g_rcasLayout.handle) dev->destroy_pipeline_layout(g_rcasLayout);
    g_rcasPipe = {0}, g_rcasLayout = {0};
    if (g_exposureOne.handle) dev->destroy_resource(g_exposureOne);
    g_exposureOne = {0};
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

std::atomic<float> g_mipBias{0};  // what new material samplers get now
std::atomic<uint32_t> g_samplersBiased{0}, g_samplersOther{0};

// Material samplers made so far, by handle (D3D12: the CPU descriptor), with their own bias: when
// the wanted bias changes they're written again with the new one (games that make their samplers
// once, at the start). Games that make them every frame get the new bias from OnCreateSampler.
std::mutex g_samplerMutex;
std::map<uint64_t, sampler_desc> g_materialSamplers;
thread_local float t_biasAdded = 0;  // OnCreateSampler -> OnInitSampler on the same thread
float g_biasWritten = 0;             // the bias the recorded samplers have

bool IsMaterialSampler(const sampler_desc& d) {
    return (d.filter == filter_mode::anisotropic || d.filter == filter_mode::min_mag_mip_linear) &&
           d.address_u == texture_address_mode::wrap && d.max_lod > d.min_lod;
}

// The bias wanted now: log2 of the render scale while upscaling (DLSS's guidance), else none.
float WantedMipBias() {
    if (!g_set.enabled || !g_set.upscale || !g_set.textureDetail) return 0;
    const float scale = g_renderScale.load();
    return scale < 0.99f && scale > 0.2f ? std::log2(scale) : 0.0f;
}

bool OnCreateSampler(device*, sampler_desc& d) {
    t_biasAdded = 0;
    const float bias = g_mipBias.load();
    if (!IsMaterialSampler(d)) {
        ++g_samplersOther;
        return false;
    }
    ++g_samplersBiased;
    if (bias == 0) return false;
    d.mip_lod_bias += bias;
    t_biasAdded = bias;
    return true;
}

void OnInitSampler(device*, const sampler_desc& d, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    if (!IsMaterialSampler(d)) {
        g_materialSamplers.erase(s.handle);  // a slot used for something else now
        return;
    }
    sampler_desc base = d;
    base.mip_lod_bias -= t_biasAdded;  // the game's own
    g_materialSamplers[s.handle] = base;
    t_biasAdded = 0;
}

void OnDestroySampler(device*, sampler s) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    g_materialSamplers.erase(s.handle);
}

// Once a frame: a changed bias goes to new samplers and is written into the recorded ones.
void UpdateMipBias(device* dev) {
    const float want = WantedMipBias();
    if (std::fabs(want - g_mipBias.load()) < 0.05f) return;
    g_mipBias = want;
    ID3D12Device* d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    for (const auto& [handle, d] : g_materialSamplers) {
        D3D12_SAMPLER_DESC n = {};
        n.Filter = static_cast<D3D12_FILTER>(d.filter);  // ReShade's values are D3D12's
        n.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address_u);
        n.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address_v);
        n.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(d.address_w);
        n.MipLODBias = d.mip_lod_bias + want;
        n.MaxAnisotropy = static_cast<UINT>(d.max_anisotropy);
        n.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(static_cast<uint32_t>(d.compare_op) + 1);
        for (int i = 0; i < 4; ++i) n.BorderColor[i] = d.border_color[i];
        n.MinLOD = d.min_lod;
        n.MaxLOD = d.max_lod;
        d3d->CreateSampler(&n, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(handle)});
    }
    g_biasWritten = want;
    Log("texture mip bias now %.2f (render scale %.2f): %zu recorded samplers rewritten", want, g_renderScale.load(),
        g_materialSamplers.size());
}

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

// Full-resolution ray traced reflections, set by the REFramework script (VillageDLSS.lua): the wanted
// SpecularResolution (0 = full, -1 = the game's) in reframework/data/VillageDLSS_rt.txt.
void WriteWantedRt() {
    static int written = -2;
    const int want = g_set.fullResReflections ? 0 : -1;
    if (want == written) return;
    const std::wstring fw = ExeDir() + L"reframework";
    if (GetFileAttributesW(fw.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // no REFramework
    CreateDirectoryW((fw + L"\\data").c_str(), nullptr);
    if (FILE* f = _wfopen((fw + L"\\data\\VillageDLSS_rt.txt").c_str(), L"w")) {
        fprintf(f, "%d\n", want);
        fclose(f);
        written = want;
        Log("reflections for VillageDLSS.lua: %s", want == 0 ? "full resolution" : "the game's resolution");
    }
}

// The script's report: the reflections' resolution now (0 full, 1 half), or -1 if unknown.
int RtResolutionNow() {
    FILE* f = _wfopen((ExeDir() + L"reframework\\data\\VillageDLSS_rt_status.txt").c_str(), L"r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

bool g_keyWasDown = false;
bool g_wasEnabled = false;
bool g_samplerStatsLogged = false;

}  // namespace

bool UpscalerOnDispatch(command_list* cl) {
    ListState* s = State(cl);
    if (!s) return false;
    RunPending(cl, *s, false);
    const uint32_t id = PipelinePs(s->pipe);
    if (RtOnDispatch(cl, *s, id)) return true;  // a skipped ray tracing denoise pass
    if (id != kFinalCs) return false;
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
        case kTonemapPs2: return OnTonemap(cl, *s);
        case kSharpenPs: return OnSharpen(cl, *s);
        case kCopyPs: return OnCopy(cl, *s);
        default: return false;
    }
}

void UpscalerPresent(swapchain* sc) {
    UpdateMipBias(sc->get_device());
    WriteWantedRt();
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
        Log("samplers: %u material ones biased by %.2f, %u others", g_samplersBiased.load(), g_mipBias.load(), g_samplersOther.load());
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
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::SliderFloat("Sharpening", &g_set.sharpness, 0.0f, 1.0f, g_set.sharpness < 0.005f ? "Off" : "%.2f"))
            changed = true;
        Help("Extra sharpening of the upscaled image (AMD's RCAS, contrast-adaptive: crisper edges without "
             "halos). 0 = off. For DLSS and Ray Reconstruction it runs after them; for FSR it's FSR's own.");
        ImGui::SeparatorText("Ray tracing");
        {
            ImGui::BeginDisabled(ResolvedTech() == Tech::Fsr);
            if (ImGui::Checkbox("Ray Reconstruction", &g_set.rr)) {
                changed = true;
                Log("Ray Reconstruction setting: %s", g_set.rr ? "on" : "off");
            }
            Help("NVIDIA's DLSS Ray Reconstruction in place of DLSS Super Resolution while the game ray traces:\n"
                 "it upscales and cleans up what's left of the noise in the ray-traced reflections and lighting,\n"
                 "on top of the game's own ray tracing denoiser. Needs the game's ray tracing on (Super Resolution\n"
                 "otherwise). The Advanced tab can skip parts of the game's denoiser for testing; the\n"
                 "game traces at half resolution, which Ray Reconstruction isn't made for, so that looked worse.");
            if (g_set.rr) {
                static const char* const kRrNames[] = {"NVIDIA's default", "D (transformer)", "E (latest transformer)",
                                                       "F (RR 2)"};
                static const int kRrIds[] = {0, 4, 5, 6};
                int cur = g_set.rrPreset == 4 ? 1 : g_set.rrPreset == 5 ? 2 : g_set.rrPreset == 6 ? 3 : 0;
                ImGui::SetNextItemWidth(ControlWidth());
                if (ImGui::Combo("Ray Reconstruction model", &cur, kRrNames, 4)) {
                    g_set.rrPreset = kRrIds[cur];
                    changed = true;
                    Log("Ray Reconstruction model setting: %d", g_set.rrPreset);
                }
                Help("D, E or F, or NVIDIA's default. A Ray Reconstruction override set in the NVIDIA App takes\n"
                     "precedence (set it to \"Use app default\" there for this choice to apply).");
                std::string rrErr;
                {
                    std::lock_guard<std::mutex> lock(g_rtMutex);
                    rrErr = g_rt.error;
                }
                if (!rrErr.empty() && g_set.enabled) ImGui::TextColored(kWarn, "Ray Reconstruction: %s", rrErr.c_str());
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox("Full-resolution reflections", &g_set.fullResReflections);
        Help("Ray traces reflections at the full render resolution instead of the game's reduced one: sharper "
             "reflections, especially with Ray Reconstruction, at a GPU cost. Needs REFramework (the bundled "
             "VillageDLSS.lua sets it) and the game's ray tracing on.");
        if (g_set.fullResReflections) {
            const int now = RtResolutionNow();
            ImGui::TextDisabled("Reflections now: %s", now == 0 ? "full resolution" : now == 1 ? "half resolution" : "unknown (is REFramework running?)");
        }
        DrawFrameGenSettings();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Status")) {
        ImGui::SeparatorText("Upscaling");
        ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
        ImGui::TextDisabled("FSR: %s", fsr::Status().c_str());
        ImGui::TextDisabled("Texture mip bias: %.2f (%u samplers)", g_mipBias.load(), g_samplersBiased.load());
        std::string err;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            err = g_lastError;
        }
        if (g_set.enabled && !err.empty()) ImGui::TextColored(kWarn, "Last problem: %s", err.c_str());
        if (g_set.rr) {
            std::lock_guard<std::mutex> lock(g_rtMutex);
            ImGui::TextDisabled("Ray Reconstruction frames %llu; game's ray tracing denoise passes skipped %llu, "
                                "signals handed over %llu times",
                                static_cast<unsigned long long>(g_rt.rrFrames), static_cast<unsigned long long>(g_rt.skipped),
                                static_cast<unsigned long long>(g_rt.copied));
        }
        DrawFrameGenStatus();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
        ImGui::SeparatorText("Image quality");
        changed |= ImGui::Checkbox("Upscale below 100% render resolution", &g_set.upscale);
        Help("On: below 100%, DLSS/FSR upscale to the screen and the game's post-processing runs at full "
             "resolution. Off: DLSS/FSR anti-alias at the render size and the game stretches the picture.");
        if (kSharpenPs > 2 && kSharpenOptional) {
            changed |= ImGui::Checkbox("Game sharpening when upscaling", &g_set.sharpen);
            Help("Below 100%, the game's sharpening (its Sharpness setting) runs on the upscaled image. Off: "
                 "only DLSS/FSR's own result.");
        }
        changed |= ImGui::Checkbox("Use the game's exposure", &g_set.gameExposure);
        Help("Tells DLSS and FSR the picture's real exposure instead of letting them estimate it: steadier "
             "brightness and less ghosting when the lighting changes.");
        changed |= ImGui::Checkbox("Sharper textures at lower render resolutions", &g_set.textureDetail);
        Help("A mip bias of log2(render scale) on the game's material textures, as NVIDIA recommends. It follows "
             "the render resolution as it changes, without a restart.");
        ImGui::SeparatorText("Ray Reconstruction testing");
        ImGui::SetNextItemWidth(ControlWidth() * 1.4f);
        if (ImGui::Combo("Game's ray tracing denoiser", &g_set.rrDenoise, kRrDenoiseNames, 3)) {
            changed = true;
            Log("Ray Reconstruction denoising setting: %s", kRrDenoiseNames[g_set.rrDenoise]);
        }
        Help("While Ray Reconstruction runs, which of the game's own ray tracing denoise passes still run.\n"
             "Game's denoiser (recommended, looked best in testing): everything stays; Ray Reconstruction\n"
             "upscales and cleans up the result. Skip all but the spatial passes: the game's frame-to-frame\n"
             "(temporal) filtering, variance and deflicker are skipped; its spatial passes still fill in the\n"
             "sparse rays (global illumination is traced with a quarter of the rays). Skip all: Ray\n"
             "Reconstruction gets the raw rays. The game traces at half resolution, so the skipping choices\n"
             "come out blotchier.");
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::Combo("Show guide", &g_set.guideView, kGuideViewNames, kGuideViewCount))
            Log("diagnosis: show guide %s", kGuideViewNames[g_set.guideView]);
        Help("Shows one of the surface pictures Ray Reconstruction gets from the game in place of the image:\n"
             "surface colour, reflectivity, normals or roughness. For checking; off at each launch.");
        DrawFrameGenAdvanced();
        ImGui::SeparatorText("Development");
        changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
        Help("Testing only: the sign of the camera jitter given to DLSS and FSR. Leave both off.");
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
    reshade::register_event<reshade::addon_event::init_sampler>(&OnInitSampler);
    reshade::register_event<reshade::addon_event::destroy_sampler>(&OnDestroySampler);
    reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
    reshade::register_event<reshade::addon_event::destroy_resource>(&RtOnDestroyResource);
    reshade::register_event<reshade::addon_event::copy_buffer_region>(&CbOnCopyBuffer);
    reshade::register_event<reshade::addon_event::copy_resource>(&CbOnCopyResource);
    reshade::register_event<reshade::addon_event::destroy_resource>(&CbOnDestroyResource);
    RegisterFrameGen();
    const float scale = g_set.enabled && g_set.mode > 0 ? kModes[g_set.mode].scale : StartupRenderScale();
    g_renderScale = scale;
    if (g_set.enabled && g_set.upscale && g_set.textureDetail && scale < 0.99f) g_mipBias = std::log2(scale);
    Log("texture mip bias %.2f (Image Quality %.2f at start; follows the render scale from now on)", g_mipBias.load(), scale);
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
    reshade::unregister_event<reshade::addon_event::init_sampler>(&OnInitSampler);
    reshade::unregister_event<reshade::addon_event::destroy_sampler>(&OnDestroySampler);
    reshade::unregister_event<reshade::addon_event::barrier>(&OnBarrier);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&RtOnDestroyResource);
    reshade::unregister_event<reshade::addon_event::copy_buffer_region>(&CbOnCopyBuffer);
    reshade::unregister_event<reshade::addon_event::copy_resource>(&CbOnCopyResource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&CbOnDestroyResource);
    UnregisterFrameGen();
}

}  // namespace rd
