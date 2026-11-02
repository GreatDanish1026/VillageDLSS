// Copies the output-size image over the game's output target with a full-screen triangle: the
// render target write converts the format (the game's post chain is r11g11b10f at some render
// scales, its output target rgba16f), which a plain GPU copy can't.

Texture2D<float4> Source : register(t0);

void VsMain(uint id : SV_VertexID, out float4 pos : SV_Position) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PsMain(float4 pos : SV_Position) : SV_Target {
    return Source.Load(int3(pos.xy, 0));
}
