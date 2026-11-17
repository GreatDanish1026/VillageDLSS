// The game's velocity (NDC offset to the previous position, y up) includes this frame's jitter:
// its velocity pass rebuilds each pixel's position with the jittered viewProjInv and projects it
// with the unjittered prevViewProj. DLSS and FSR want motion without the jitter, in render pixels:
//   motion = velocity * (0.5 w, -0.5 h) + jitter (render pixels, the same sign the upscaler gets).

cbuffer Params : register(b0) {
    float2 Scale;   // (0.5 w, -0.5 h)
    float2 Offset;  // the jitter in render pixels
    uint2 Size;
    uint2 Pad;
};

Texture2D<float4> Velocity : register(t0);
RWTexture2D<float2> Motion : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= Size)) return;
    Motion[id] = Velocity[id].xy * Scale + Offset;
}
