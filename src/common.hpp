// What main.cpp (logging, shader ids, resource tracking) shares with the upscaler, DLSS, FSR and
// frame generation code.
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rd {

void Log(const char* fmt, ...);
std::wstring ExeDir();  // with a trailing backslash

// Shader ids (FNV-1a of the bytecode) of a pipeline: pixel shader for graphics, compute shader.
uint32_t PipelinePs(reshade::api::pipeline pipe);

// The resource a view was created for, from the add-on's own map (never asks ReShade about a
// view whose resource may have been freed).
reshade::api::resource ViewResource(reshade::api::resource_view view);

// Frame generation (framegen.cpp, shared with RE4DLSS and the Unreal add-ons): inputs go to their
// frame when the game submits them, not when they're recorded.
constexpr bool kTagAtRecord = false;
constexpr bool kTsrMapped = true;  // the TAA pass is known (framegen.cpp's name from the TSR add-ons)
// Unreal's HDR output copies the picture before the HUD out of the back buffer; here the upscaler
// draws that picture itself (FrameGenHudless* below).
constexpr bool kHudFromBackBufferCopy = false;

// Overlay helpers (upscaler.cpp).
float ControlWidth();
void Help(const char* text);  // a "(?)" after the last item, with <text> as its tooltip
extern const ImVec4 kWarn;
// Puts the game's pipeline, root signatures, descriptor heaps and bindings back on <cl> after
// the add-on recorded work of its own on it.
void RestoreListState(reshade::api::command_list* cl);

// framegen.cpp: DLSS Frame Generation through Streamline, or AMD FSR frame generation.
void RegisterFrameGen();
void UnregisterFrameGen();
void DrawFrameGenSettings();
void DrawFrameGenStatus();
void DrawFrameGenAdvanced();  // DLSS-G object separation, FSR HUD protection
std::string FrameGenHeadline(bool* warn);
std::string FrameGenDiag();  // inputs tagged / frames without them since the last call, and why
bool FrameGenEnabled();  // chosen in the settings (it may still wait for a restart)
// True while frame generation wants each frame's camera.
bool FrameGenWantsCamera();
// RE Engine's SceneInfo constant buffer (kSceneInfoBytes) of the frame's main view.
constexpr uint32_t kSceneInfoBytes = 528;
void FrameGenSetSceneInfo(const float* scene);
// The picture before the HUD, drawn by the upscaler: whether frame generation wants it, the
// render target to draw it into (back buffer size and format; left in the shader-resource state),
// and that it was drawn on <cl> (handed to frame generation when <cl> is submitted).
bool FrameGenWantsHudless();
bool FrameGenHudlessTarget(reshade::api::device* dev, reshade::api::resource* tex, reshade::api::resource_view* rtv);
void FrameGenHudlessDrawn(reshade::api::command_list* cl);
// This frame's depth (reversed Z) and motion vectors (previous minus current; times mvScale =
// UV offsets), each in its D3D12 resource state, tagged on <list>. Jitter in render pixels.
void FrameGenTag(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, unsigned depthState, ID3D12Resource* motion,
                 unsigned motionState, unsigned renderWidth, unsigned renderHeight, float jitterX, float jitterY,
                 float mvScaleX, float mvScaleY, bool reset);

// upscaler.cpp
void UpscalerInit();
void UpscalerShutdown();
bool UpscalerOnDraw(reshade::api::command_list* cl, uint32_t vertices, uint32_t instances);  // true: draw replaced
bool UpscalerOnDispatch(reshade::api::command_list* cl);  // true: dispatch replaced
void UpscalerBeforeDraw(reshade::api::command_list* cl);  // other draws: ends a redirect
void UpscalerPresent(reshade::api::swapchain* sc);
void UpscalerOverlay();

// main.cpp: the frame capture tool (Advanced tab).
void DrawCaptureTool();

}  // namespace rd
