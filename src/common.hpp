// What main.cpp (logging, shader ids, resource tracking) shares with the upscaler and DLSS code.
#pragma once

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

namespace rd {

void Log(const char* fmt, ...);
std::wstring ExeDir();  // with a trailing backslash

// Shader ids (FNV-1a of the bytecode) of a pipeline: pixel shader for graphics, compute shader.
uint32_t PipelinePs(reshade::api::pipeline pipe);

// The resource a view was created for, from the add-on's own map (never asks ReShade about a
// view whose resource may have been freed).
reshade::api::resource ViewResource(reshade::api::resource_view view);

// upscaler.cpp
void UpscalerInit();
void UpscalerShutdown();
bool UpscalerOnDraw(reshade::api::command_list* cl, uint32_t vertices, uint32_t instances);  // true: draw replaced
void UpscalerPresent();
void UpscalerOverlay();

}  // namespace rd
