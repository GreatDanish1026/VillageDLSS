// Ray Reconstruction guides from RE Engine's G-buffer, at render size. The decoding follows the
// game's rayTracingUpscale shader (docs/FINDINGS.md "Ray tracing chain"):
//   NormalXNormalYRoughnessMisc (rgb10a2): octahedral world normal in xy, roughness z, misc w
//   BaseColorMetallic (rgba8 sRGB): base colour, metallic (counts only when misc > 0.667)
// Diffuse albedo = base * (1 - metallic); specular colour F0 = max(0.04, base * metallic);
// specular albedo = EnvBRDFApprox(F0, roughness, N.V), the view direction from the pixel's world
// position (SceneInfo.viewProjInvMat) minus the camera position.

cbuffer Params : register(b0) {
    float4 ViewProjInv[4];  // SceneInfo.viewProjInvMat rows (row vectors: p * M)
    float4 CameraPos;       // xyz
    float2 Size;            // render size
    float2 InvSize;
};

Texture2D<float> Depth : register(t0);         // reversed Z
Texture2D<float4> BaseColor : register(t1);
Texture2D<float4> NormalRough : register(t2);
RWTexture2D<float4> DiffuseOut : register(u0);
RWTexture2D<float4> SpecularOut : register(u1);
RWTexture2D<float4> NormalOut : register(u2);

float3 DecodeNormal(float2 e) {
    const float2 f = e * 2 - 1;
    float3 n = float3(f, 1 - abs(f.x) - abs(f.y));
    if (n.z < 0) n.xy = float2(n.x >= 0 ? 1 - abs(f.y) : abs(f.y) - 1, n.y >= 0 ? 1 - abs(f.x) : abs(f.x) - 1);
    return normalize(n);
}

float3 EnvBRDFApprox(float3 specularColor, float roughness, float NoV) {
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    AB.y *= saturate(50.0 * specularColor.g);
    return specularColor * AB.x + AB.y;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(float2(id) >= Size)) return;
    const float d = Depth[id];
    const float2 ndc = float2((id.x + 0.5) * InvSize.x * 2 - 1, 1 - (id.y + 0.5) * InvSize.y * 2);
    // Near plane when there's no surface (sky): reversed Z, 1 = near.
    const float4 p = ndc.x * ViewProjInv[0] + ndc.y * ViewProjInv[1] + (d > 0 ? d : 1) * ViewProjInv[2] + ViewProjInv[3];
    const float3 V = normalize(p.xyz / p.w - CameraPos.xyz);  // camera to pixel

    if (d <= 0) {  // sky: nothing to demodulate
        DiffuseOut[id] = float4(1, 1, 1, 1);
        SpecularOut[id] = float4(0, 0, 0, 1);
        NormalOut[id] = float4(-V, 1);
        return;
    }
    const float4 nr = NormalRough[id];
    const float4 bc = BaseColor[id];
    const float3 N = DecodeNormal(nr.xy);
    const float roughness = max(0.01, nr.z);
    const float metallic = nr.w > 0.667 ? bc.a : 0;
    const float3 diffuse = bc.rgb * (1 - metallic);
    const float3 f0 = max(0.04, bc.rgb * metallic);
    const float NoV = saturate(dot(N, -V));

    DiffuseOut[id] = float4(diffuse, 1);
    SpecularOut[id] = float4(saturate(EnvBRDFApprox(f0, roughness, max(NoV, 1e-3))), 1);
    NormalOut[id] = float4(N, roughness);
}
