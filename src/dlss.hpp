// NVIDIA DLSS Super Resolution through NGX (DLSS SDK 310.9.16).
#pragma once

#include <d3d12.h>

#include <string>

namespace dlss {

// Inputs::preset value for "Automatic": the model NVIDIA recommends for the render scale (K
// for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance).
constexpr int kPresetAuto = -1;

struct Inputs {
    ID3D12Resource* color = nullptr;   // render resolution, linear HDR, jittered
    ID3D12Resource* depth = nullptr;   // render resolution, reversed Z
    ID3D12Resource* motion = nullptr;  // render resolution
    ID3D12Resource* output = nullptr;  // output resolution, UAV
    unsigned renderWidth = 0, renderHeight = 0;
    unsigned outputWidth = 0, outputHeight = 0;
    float jitterX = 0, jitterY = 0;  // in render pixels
    float motionScaleX = 1, motionScaleY = 1;
    bool reset = false;
    int preset = 0;  // NVSDK_NGX_DLSS_Hint_Render_Preset_*
};

// Loads NGX for the device (once). False with a reason if DLSS can't run.
bool Init(ID3D12Device* device, std::string& error);

// Records DLSS on the command list. Each output has its own feature (history), created when
// first seen and recreated when the sizes or preset change. The
// inputs must be in a shader resource state and the output in the unordered access state.
// Changes the command list's compute state and descriptor heaps.
bool Evaluate(ID3D12GraphicsCommandList* list, const Inputs& in, std::string& error);

// Once per presented frame: releases replaced features the GPU is done with.
void EndFrame();

// Releases every feature (after the GPU is done with them): when FSR takes over.
void ReleaseFeatures();

void Shutdown();

// A line for the overlay ("DLSS 310.x, Quality mode ...").
std::string Status();

}  // namespace dlss
