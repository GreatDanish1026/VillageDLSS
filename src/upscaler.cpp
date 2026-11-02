// DLSS in place of the game's TAA (docs/FINDINGS.md).
//
// The TAA pass (pixel shader pscf113c28) is a full-screen draw reading t0 depth (reverse-Z), t1
// the scene colour before exposure, t2 last frame's TAA output and t3 the velocity (NDC offset
// from the current to the previous position, y up), with the camera constants (SceneInfo) in
// root CBV b0. DLSS takes the same inputs:
//   - motion vectors: the game's velocity texture as it is, scaled by (0.5 w, -0.5 h) to pixels;
//   - jitter: SceneInfo.projElement[0].zw (clip space) times half the render size.
//
// DLAA (render size = TAA target size, Image Quality 1.0 or above): the TAA draw is skipped and
// DLSS's output is copied into the TAA target. Everything after it runs as the game does.
//
// Upscaling (render size below the output size): everything after TAA runs at render size, and
// one pass (CopyImageWithDepthPS) stretches it to the output and writes a full-size depth. So:
//   1. TAA: DLSS writes an output-size image (X). The game's TAA still runs at render size, so
//      its bloom input and TAA history stay as they are.
//   2. Bloom composite (NewFinal, additive, texture coordinates): drawn again into X.
//   3. Tonemap (LDRPostProcess_WithTonemap): reads t1 by pixel position and SceneInfo's screen
//      size, so it's drawn only at output size, into Y: t1 rewritten to X in its descriptor
//      table, SceneInfo replaced by a copy with the output size. The render-size tonemap and the
//      sharpening after it are unused then.
//   4. CopyImageWithDepthPS: the game's draw (for its depth), then Y copied over its colour.
// The HUD and the final output pass (and RenoDX) follow as usual.
//
// NGX records its own compute state and descriptor heaps on the game's command list, so the
// game's bindings are tracked per command list and put back afterwards (as in ReunionDLSS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "dlss.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

constexpr uint32_t kTaaPs = 0xcf113c28;      // PreTonemap2_PS
constexpr uint32_t kBloomPs = 0x77ae4c63;    // NewFinal
constexpr uint32_t kTonemapPs = 0x3cc6333b;  // LDRPostProcess_WithTonemap
constexpr uint32_t kCopyPs = 0x48155d5e;     // CopyImageWithDepthPS
constexpr int kToggleHotkey = VK_F9;

// SceneInfo (cb0 of the TAA and tonemap shaders), as float indices.
constexpr int kViewProj = 0;           // row_major float4x4, byte 0
constexpr int kProjElement0 = 40;      // byte 160: x scale, y scale, jitter x, jitter y (clip space)
constexpr int kPrevViewProj = 72;      // byte 288
constexpr int kScreenSize = 92;        // byte 368: width, height, 1 / width, 1 / height
constexpr int kSceneInfoFloats = 132;  // 528 bytes
constexpr uint32_t kSceneInfoSlot = 768;  // 256-byte aligned
constexpr uint32_t kSceneInfoSlots = 192;

// ---------------------------------------------------------------- settings

struct Settings {
    bool enabled = false;  // F9; remembered across launches
    int preset = 11;       // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer); 0 = DLSS default
    bool flipJitterX = false, flipJitterY = false;  // testing: jitter sign
    bool upscale = true;   // below the output size: DLSS to output size (else DLAA at render size)
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscale", g_set.upscale);
}

std::mutex g_statusMutex;
std::string g_lastError;  // why the last TAA pass wasn't replaced
std::string g_mode;       // "DLAA 3840x2160" / "upscaling 1920x1080 -> 3840x2160"
uint64_t g_replaced = 0;

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastError) Log("DLSS: %s", why.c_str());
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

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    ListState* s = State(cl);
    if (!s) return;
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
    bool bloomDone = false, tonemapDone = false;
    uint64_t lastFrame = UINT64_MAX;  // last present count with upscaling
} g_up;

// Descriptor slots rewritten for the tonemap, put back when upscaling stops in case the game
// keeps that table from frame to frame (if it builds it per frame, the game rewrites it anyway).
struct Patch {
    Slot slot;
    uint64_t firstFrame, lastFrame;
};
std::map<uint64_t, Patch> g_patches;  // by table handle

void UnpatchAll(device* dev) {
    for (auto& [handle, p] : g_patches) {
        if (p.lastFrame - p.firstFrame < 3 || !p.slot.view.handle) continue;  // rebuilt every frame: leave it
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

// ---------------------------------------------------------------- passes

bool OnTaa(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    const resource depth = PixelSrv(dev, s.graphics, 0);
    const resource color = PixelSrv(dev, s.graphics, 1);
    const resource motion = PixelSrv(dev, s.graphics, 3);
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
    const resource_desc cd = dev->get_resource_desc(color), td = dev->get_resource_desc(target);
    uint32_t renderW = cd.texture.width, renderH = cd.texture.height;
    const float sw = scene[kScreenSize], sh = scene[kScreenSize + 1];
    if (sw >= 64 && sh >= 64 && sw <= renderW && sh <= renderH) {  // the viewport may be smaller than the texture
        renderW = static_cast<uint32_t>(sw);
        renderH = static_cast<uint32_t>(sh);
    }

    std::string err;
    if (!dlss::Init(reinterpret_cast<ID3D12Device*>(dev->get_native()), err)) {
        Fail(err);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_resMutex);
    const bool upscale = g_set.upscale && g_outW && g_outH && renderW < g_outW && renderH < g_outH &&
                         renderW * 3 >= g_outW && renderH * 3 >= g_outH;  // DLSS goes down to a third
    const uint32_t outW = upscale ? g_outW : td.texture.width, outH = upscale ? g_outH : td.texture.height;
    const resource_usage usage = resource_usage::unordered_access | resource_usage::copy_source |
                                 resource_usage::shader_resource | resource_usage::render_target;
    if (!Ensure(dev, g_x, outW, outH, td.texture.format, usage, resource_usage::unordered_access, "DLSS output")) {
        Fail("no DLSS output texture");
        return false;
    }

    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(g_x.res.handle);
    in.renderWidth = renderW, in.renderHeight = renderH;
    in.outputWidth = outW, in.outputHeight = outH;
    // The projection is shifted by the clip-space jitter; DLSS wants the sample's offset from the
    // pixel centre in render pixels (x right, y down). Confirmed in game with DLAA (0.1.0).
    in.jitterX = -scene[kProjElement0 + 2] * renderW * 0.5f * (g_set.flipJitterX ? -1 : 1);
    in.jitterY = scene[kProjElement0 + 3] * renderH * 0.5f * (g_set.flipJitterY ? -1 : 1);
    in.motionScaleX = 0.5f * renderW;
    in.motionScaleY = -0.5f * renderH;
    in.reset = CameraCut(scene);
    in.preset = g_set.preset;

    Transition(cl, g_x, resource_usage::unordered_access);
    if (!dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err)) {
        Restore(cl, s);
        Fail(err);
        return false;
    }

    std::string mode = upscale ? "upscaling " + std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " +
                                     std::to_string(outW) + "x" + std::to_string(outH)
                               : "DLAA " + std::to_string(renderW) + "x" + std::to_string(renderH);
    bool replaced;
    if (upscale) {
        // The game's TAA still runs at render size (bloom input, history); the passes after it
        // use X.
        Restore(cl, s);
        g_up.active = true;
        g_up.bloomDone = g_up.tonemapDone = false;
        g_up.lastFrame = g_frame;
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
        Log("DLSS running: %s, jitter %.3f %.3f px%s", mode.c_str(), in.jitterX, in.jitterY, in.reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    return replaced;
}

// Bloom: the game's draw adds the bloom to its render-size TAA output; the same draw (additive,
// texture coordinates) is recorded once more into X first.
void OnBloom(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || g_up.bloomDone || !g_x.rtv.handle) return;
    Transition(cl, g_x, resource_usage::render_target);
    BindFullTarget(cl, g_x.rtv, g_x.w, g_x.h);
    cl->draw(3, 1, 0, 0);
    RestoreTargets(cl, s);
    g_up.bloomDone = true;
}

// Tonemap at output size into Y, from X; the game's render-size draw is skipped.
bool OnTonemap(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || g_up.tonemapDone || !g_x.srv.handle) return false;
    Slot slot;
    float scene[kSceneInfoFloats];
    if (!PixelSrvSlot(dev, s.graphics, 1, &slot) || !ReadSceneInfo(dev, s.graphics, scene)) {
        Fail("tonemap inputs not found: game's upscaling used");
        return false;
    }
    const resource target = ViewResource(s.rtvs[0]);
    if (!target.handle) return false;
    const format fmt = dev->get_resource_desc(target).texture.format;
    if (!Ensure(dev, g_y, g_x.w, g_x.h, fmt,
                resource_usage::render_target | resource_usage::copy_source | resource_usage::shader_resource,
                resource_usage::render_target, "tonemapped"))
        return false;
    buffer_range cb;
    if (!SceneInfoCopy(dev, scene, g_x.w, g_x.h, &cb)) return false;

    // The colour slot now shows X (the render-size draw that read it is skipped).
    auto it = g_patches.find(slot.table.handle);
    if (it == g_patches.end()) it = g_patches.emplace(slot.table.handle, Patch{slot, g_frame, g_frame}).first;
    it->second.lastFrame = g_frame;
    descriptor_table_update u = {};
    u.table = slot.table;
    u.binding = slot.binding;
    u.count = 1;
    u.type = slot.type;
    u.descriptors = &g_x.srv;
    dev->update_descriptor_tables(1, &u);

    Transition(cl, g_x, resource_usage::shader_resource);
    Transition(cl, g_y, resource_usage::render_target);
    for (uint32_t param : SceneInfoParams(dev, s.graphics)) {
        descriptor_table_update cu = {};
        cu.count = 1;
        cu.type = descriptor_type::constant_buffer;
        cu.descriptors = &cb;
        cl->push_descriptors(s.graphics.roots.at(param).stages, s.graphics.layout, param, cu);
    }
    BindFullTarget(cl, g_y.rtv, g_y.w, g_y.h);
    cl->draw(3, 1, 0, 0);
    Restore(cl, s);  // puts the game's SceneInfo back
    RestoreTargets(cl, s);
    g_up.tonemapDone = true;
    return true;
}

// The game's stretch to output size: its draw runs (it also writes the output-size depth), then
// Y goes over its colour.
bool OnCopy(command_list* cl, ListState& s) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (!g_up.active || !g_up.tonemapDone || !g_y.res.handle) return false;
    const resource target = ViewResource(s.rtvs[0]);
    if (!target.handle) return false;
    const resource_desc td = cl->get_device()->get_resource_desc(target);
    if (td.texture.width != g_y.w || td.texture.height != g_y.h || td.texture.format != g_y.fmt) {
        Fail("output target differs from the tonemapped image: game's upscaling used");
        return false;
    }
    cl->draw(3, 1, 0, 0);
    Transition(cl, g_y, resource_usage::copy_source);
    cl->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
    cl->copy_resource(g_y.res, target);
    cl->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
    Transition(cl, g_y, resource_usage::render_target);
    g_up.active = false;
    return true;
}

void DestroyAll(device* dev) {
    dlss::Shutdown();
    for (Texture* t : {&g_x, &g_y}) Retire(*t);
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
    g_dev = nullptr;
}

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::mutex> lock(g_resMutex);
    if (dev == g_dev) DestroyAll(dev);
}

bool g_keyWasDown = false;
bool g_wasEnabled = false;

}  // namespace

bool UpscalerOnDraw(command_list* cl, uint32_t vertices, uint32_t instances) {
    if (!g_set.enabled || vertices != 3 || instances != 1) return false;
    ListState* s = State(cl);
    if (!s) return false;
    switch (PipelinePs(s->pipe)) {
        case kTaaPs: return OnTaa(cl, *s);
        case kBloomPs: OnBloom(cl, *s); return false;
        case kTonemapPs: return OnTonemap(cl, *s);
        case kCopyPs: return OnCopy(cl, *s);
        default: return false;
    }
}

void UpscalerPresent(swapchain* sc) {
    dlss::EndFrame();
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
        Log("DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    g_wasEnabled = g_set.enabled;
}

void UpscalerOverlay() {
    bool changed = ImGui::Checkbox("DLSS in place of TAA (F9)", &g_set.enabled);
    changed |= ImGui::Checkbox("Upscale when Image Quality is below 1.0", &g_set.upscale);
    ImGui::TextDisabled("Image Quality sets the render size: 0.67 = DLSS Quality, 0.58 Balanced, 0.5 Performance.");
    const char* presets[] = {"DLSS default", "J (transformer)", "K (transformer, recommended)"};
    int idx = g_set.preset == 10 ? 1 : g_set.preset == 11 ? 2 : 0;
    if (ImGui::Combo("Model preset", &idx, presets, 3)) {
        g_set.preset = idx == 1 ? 10 : idx == 2 ? 11 : 0;
        changed = true;
    }
    if (ImGui::TreeNode("Testing")) {
        changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
        ImGui::TreePop();
    }
    if (changed) SaveSettings();
    std::string err, mode;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        err = g_lastError;
        mode = g_mode;
    }
    ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
    if (g_set.enabled && !mode.empty()) ImGui::TextDisabled("Mode: %s", mode.c_str());
    if (g_set.enabled && !err.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", err.c_str());
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
    Log("DLSS %s at start (F9 toggles), upscaling %s", g_set.enabled ? "on" : "off", g_set.upscale ? "on" : "off");
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
}

}  // namespace rd
