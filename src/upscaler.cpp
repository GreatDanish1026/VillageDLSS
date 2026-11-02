// Replaces the game's TAA pass (pixel shader pscf113c28, docs/FINDINGS.md) with DLSS.
//
// The TAA pass is a full-screen draw reading t0 depth (reverse-Z), t1 the scene colour before
// exposure, t2 last frame's TAA output and t3 the velocity (NDC offset from the current to the
// previous position, y up), with the camera constants (SceneInfo) in root CBV b0. When it is
// about to be recorded, the draw is skipped and DLSS runs on the same inputs instead:
//   - motion vectors: the game's velocity texture as it is, scaled by (0.5 w, -0.5 h) to pixels;
//   - jitter: SceneInfo.projElement[0].zw (clip space) times half the render size;
//   - output: our own UAV texture (the game's render target may not allow UAVs), copied into the
//     TAA render target afterwards. The game then copies that into its TAA history as usual.
// NGX records its own compute state and descriptor heaps on the game's command list, so the
// game's bindings are tracked per command list and put back afterwards (as in ReunionDLSS).
//
// 0.1.0 is DLAA: the render size is the game's, so at Image Quality 1.0 DLSS runs at native
// resolution. Upscaling needs the post-processing after TAA at output size (later).

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

constexpr uint32_t kTaaPs = 0xcf113c28;  // PreTonemap2_PS
constexpr int kToggleHotkey = VK_F9;

// SceneInfo (cb0 of the TAA shader), as float indices.
constexpr int kViewProj = 0;        // row_major float4x4, byte 0
constexpr int kProjElement0 = 40;   // byte 160: x scale, y scale, jitter x, jitter y (clip space)
constexpr int kPrevViewProj = 72;   // byte 288
constexpr int kScreenSize = 92;     // byte 368: width, height
constexpr int kSceneInfoFloats = 132;  // 528 bytes

// ---------------------------------------------------------------- settings

struct Settings {
    bool enabled = false;  // F9; remembered across launches
    int preset = 11;       // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer); 0 = DLSS default
    bool flipJitterX = false, flipJitterY = false;  // testing: jitter sign
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
}

std::mutex g_statusMutex;
std::string g_lastError;  // why the last TAA pass wasn't replaced
uint64_t g_replaced = 0;  // TAA passes replaced so far

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastError) Log("TAA pass kept: %s", why.c_str());
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
    resource_view rtv0 = {0};
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

void OnBindRenderTargets(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view) {
    if (ListState* s = State(cl)) s->rtv0 = count ? rtvs[0] : resource_view{0};
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

// ---------------------------------------------------------------- TAA inputs

// The texture at pixel shader register t<reg> space0 in the bound graphics tables.
resource PixelSrv(device* dev, const StageState& st, uint32_t reg) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
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
            descriptor_heap heap = {0};
            uint32_t offset = 0;
            dev->get_descriptor_heap_offset(st.tables[i], range.binding + (reg - range.dx_register_index), 0, &heap,
                                            &offset);
            return ViewResource(dt->get_resource_view(heap, offset));
        }
    }
    return {0};
}

// SceneInfo: root CBV b0 space0 visible to the pixel shader.
bool SceneInfo(device* dev, const StageState& st, float* out) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return false;
    for (const auto& [param, r] : st.roots) {
        if (r.type != descriptor_type::constant_buffer) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type != pipeline_layout_param_type::push_descriptors || p.push_descriptors.dx_register_index != 0 ||
            p.push_descriptors.dx_register_space != 0 || (p.push_descriptors.visibility & shader_stage::pixel) == 0)
            continue;
        void* data = nullptr;
        if (!dev->map_buffer_region(r.range.buffer, r.range.offset, kSceneInfoFloats * 4, map_access::read_only, &data) ||
            !data)
            return false;
        memcpy(out, data, kSceneInfoFloats * 4);
        dev->unmap_buffer_region(r.range.buffer);
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
    return diff > 0.5f || !std::isfinite(diff);
}

// ---------------------------------------------------------------- DLSS output texture

std::mutex g_texMutex;
device* g_dev = nullptr;
resource g_tex = {0};
uint32_t g_texW = 0, g_texH = 0;
format g_texFormat = format::unknown;
struct OldTexture {
    resource res;
    uint64_t frame;
};
std::vector<OldTexture> g_oldTextures;  // the GPU may still use these for a few frames
uint64_t g_frame = 0;

resource OutputTexture(device* dev, const resource_desc& target) {
    std::lock_guard<std::mutex> lock(g_texMutex);
    if (g_tex.handle && g_texW == target.texture.width && g_texH == target.texture.height &&
        g_texFormat == target.texture.format)
        return g_tex;
    if (g_tex.handle) {
        g_oldTextures.push_back({g_tex, g_frame});
        dlss::ReleaseFeatures();  // a new output: new histories
    }
    g_tex = {0};
    const resource_desc d(target.texture.width, target.texture.height, 1, 1, target.texture.format, 1,
                          memory_heap::default_,
                          resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource);
    if (!dev->create_resource(d, nullptr, resource_usage::unordered_access, &g_tex)) {
        Log("couldn't create the %ux%u DLSS output texture (format %u)", d.texture.width, d.texture.height,
            static_cast<unsigned>(d.texture.format));
        g_tex = {0};
        return g_tex;
    }
    g_dev = dev;
    g_texW = target.texture.width, g_texH = target.texture.height, g_texFormat = target.texture.format;
    Log("DLSS output texture %ux%u (format %u)", g_texW, g_texH, static_cast<unsigned>(g_texFormat));
    return g_tex;
}

// ---------------------------------------------------------------- the TAA pass

bool ReplaceTaa(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    const resource depth = PixelSrv(dev, s.graphics, 0);
    const resource color = PixelSrv(dev, s.graphics, 1);
    const resource motion = PixelSrv(dev, s.graphics, 3);
    const resource target = ViewResource(s.rtv0);
    if (!depth.handle || !color.handle || !motion.handle || !target.handle) {
        Fail("inputs not found (depth, colour, velocity or render target)");
        return false;
    }
    float scene[kSceneInfoFloats];
    if (!SceneInfo(dev, s.graphics, scene)) {
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
    const resource tex = OutputTexture(dev, td);
    if (!tex.handle) {
        Fail("no DLSS output texture");
        return false;
    }

    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(motion.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(tex.handle);
    in.renderWidth = renderW, in.renderHeight = renderH;
    in.outputWidth = td.texture.width, in.outputHeight = td.texture.height;
    // The projection is shifted by the clip-space jitter; DLSS wants the sample's offset from the
    // pixel centre in render pixels (x right, y down): opposite in x, and clip y is up.
    in.jitterX = -scene[kProjElement0 + 2] * renderW * 0.5f * (g_set.flipJitterX ? -1 : 1);
    in.jitterY = scene[kProjElement0 + 3] * renderH * 0.5f * (g_set.flipJitterY ? -1 : 1);
    in.motionScaleX = 0.5f * renderW;
    in.motionScaleY = -0.5f * renderH;
    in.reset = CameraCut(scene);
    in.preset = g_set.preset;

    if (!dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err)) {
        Restore(cl, s);
        Fail(err);
        return false;
    }
    cl->barrier(tex, resource_usage::unordered_access, resource_usage::copy_source);
    cl->barrier(target, resource_usage::render_target, resource_usage::copy_dest);
    cl->copy_resource(tex, target);
    cl->barrier(target, resource_usage::copy_dest, resource_usage::render_target);
    cl->barrier(tex, resource_usage::copy_source, resource_usage::unordered_access);
    Restore(cl, s);

    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_replaced++ || !g_lastError.empty())
        Log("TAA replaced by DLSS: %ux%u -> %ux%u, jitter %.3f %.3f px%s", renderW, renderH, in.outputWidth,
            in.outputHeight, in.jitterX, in.jitterY, in.reset ? ", reset" : "");
    g_lastError.clear();
    return true;
}

void OnDestroyDevice(device* dev) {
    std::lock_guard<std::mutex> lock(g_texMutex);
    if (dev != g_dev) return;
    dlss::Shutdown();
    if (g_tex.handle) dev->destroy_resource(g_tex);
    for (OldTexture& t : g_oldTextures) dev->destroy_resource(t.res);
    g_oldTextures.clear();
    g_tex = {0};
    g_dev = nullptr;
}

bool g_keyWasDown = false;

}  // namespace

bool UpscalerOnDraw(command_list* cl, uint32_t vertices, uint32_t instances) {
    if (!g_set.enabled || vertices != 3 || instances != 1) return false;
    ListState* s = State(cl);
    if (!s || PipelinePs(s->pipe) != kTaaPs) return false;
    return ReplaceTaa(cl, *s);
}

void UpscalerPresent() {
    dlss::EndFrame();
    {
        std::lock_guard<std::mutex> lock(g_texMutex);
        ++g_frame;
        for (size_t i = 0; i < g_oldTextures.size();) {
            if (g_frame - g_oldTextures[i].frame < 8 || !g_dev) {
                ++i;
                continue;
            }
            g_dev->destroy_resource(g_oldTextures[i].res);
            g_oldTextures.erase(g_oldTextures.begin() + i);
        }
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
}

void UpscalerOverlay() {
    bool changed = ImGui::Checkbox("DLSS in place of TAA (F9)", &g_set.enabled);
    const char* presets[] = {"DLSS default", "J (transformer)", "K (transformer, recommended)"};
    int idx = g_set.preset == 10 ? 1 : g_set.preset == 11 ? 2 : 0;
    if (ImGui::Combo("Model preset", &idx, presets, 3)) {
        g_set.preset = idx == 1 ? 10 : idx == 2 ? 11 : 0;
        changed = true;
    }
    ImGui::TextDisabled("Testing: if the image shakes or looks soft with DLSS on, try these:");
    changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
    if (changed) SaveSettings();
    std::string err;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        err = g_lastError;
    }
    ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
    if (g_set.enabled && !err.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "TAA kept: %s", err.c_str());
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
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    Log("DLSS %s at start (F9 toggles)", g_set.enabled ? "on" : "off");
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
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
}

}  // namespace rd
