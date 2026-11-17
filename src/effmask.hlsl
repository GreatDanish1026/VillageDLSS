// Effect mask for games whose TAA has none (Village, RE7): the add-on copies the scene colour just
// before the game draws its particles, fire, smoke, refraction and other effects into it, and this
// compares that copy with the colour the TAA gets. Where the effects changed a pixel, DLSS (bias
// current colour) and FSR (reactive mask) trust the current frame more: the effects have no motion
// vectors of their own, so their history would smear.

cbuffer Params : register(b0) {
    float Threshold;  // relative change below this counts as none (noise, subtle haze)
    float Gain;       // relative change above the threshold -> mask value
    uint2 Size;
    uint4 Pad;
};

Texture2D<float4> Before : register(t0);  // scene colour before the effects
Texture2D<float4> After : register(t1);   // scene colour the TAA reads
RWTexture2D<float> Mask : register(u0);

float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= Size)) return;
    const float3 a = max(After[id].rgb, 0), b = max(Before[id].rgb, 0);
    // Relative change, so it means the same in dark and bright places (the colour isn't exposed yet).
    const float rel = Max3(abs(a - b)) / max(max(Max3(a), Max3(b)), 1e-4);
    Mask[id] = isfinite(rel) ? saturate((rel - Threshold) * Gain) : 0;
}
