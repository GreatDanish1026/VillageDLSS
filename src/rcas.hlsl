// Sharpening after DLSS: AMD FidelityFX RCAS (robust contrast-adaptive sharpening), on the HDR
// image. Each pixel is sharpened against its four neighbours with a strength limited by their
// local contrast, so edges get crisper without ringing. HDR values are tonemapped (c / (1 + max))
// for the analysis and mapped back afterwards, so bright highlights don't overshoot.

cbuffer Params : register(b0) {
    float Sharpness;  // 0 = off, 1 = AMD's strongest
    float3 Pad;
    uint2 Size;
    uint2 Pad2;
};

Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Dest : register(u0);

float3 Tonemap(float3 c) { return c / (1 + max(max(c.r, c.g), c.b)); }
float3 Untonemap(float3 c) { return c / max(1e-4, 1 - max(max(c.r, c.g), c.b)); }

float3 Load(int2 p) { return Tonemap(max(0, Source[clamp(p, int2(0, 0), int2(Size) - 1)].rgb)); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= Size)) return;
    const int2 p = int2(id);
    const float4 src = Source[p];
    const float3 e = Tonemap(max(0, src.rgb));
    const float3 b = Load(p + int2(0, -1));
    const float3 d = Load(p + int2(-1, 0));
    const float3 f = Load(p + int2(1, 0));
    const float3 h = Load(p + int2(0, 1));
    const float3 mn = min(min(b, d), min(f, h));
    const float3 mx = max(max(b, d), max(f, h));
    // How far the ring can push this pixel before it clips, per channel (RCAS).
    const float3 hitMin = mn / (4 * max(mx, 1e-5));
    const float3 hitMax = (1 - mx) / (4 * mn - 4 - 1e-5);
    const float3 lobeRgb = max(-hitMin, hitMax);
    const float lobe = max(-0.1875, min(max(lobeRgb.r, max(lobeRgb.g, lobeRgb.b)), 0)) * Sharpness;
    const float3 sharp = (lobe * (b + d + f + h) + e) / (4 * lobe + 1);
    Dest[p] = float4(Untonemap(saturate(sharp)), src.a);
}
