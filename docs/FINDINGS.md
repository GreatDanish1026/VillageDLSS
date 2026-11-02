# Resident Evil Village frame analysis

Game: Steam build, re8.exe 1.2.3.9, D3D12, 3840x2160 output. ReShade 6.8.0, REFramework and
RenoDX (HDR) installed. Captures in `captures/` (VillageDLSS 0.0.3, two frames each; the
second frame is the complete one).

| Capture | Settings | Back buffer |
|---|---|---|
| frame-1 | Image Quality 1.0, FSR off, TAA | rgb10a2, HDR10 (colour space 3) |
| frame-2 | FSR 1 Performance | rgba8, SDR (colour space 1) |
| frame-3 | Image Quality 0.5, FSR off | rgba8, SDR (colour space 1) |

After a settings change the swap chain came back SDR (rgba8); HDR needs checking after a restart.

## Frame layout

Three graphics command lists per frame on one graphics queue (L2 shadows/G-buffer, L3, L4
lighting and post) plus empty compute-queue lists. About 300 passes per frame.

RE Engine binds one root signature to everything: compute param 2 = SRV table t0..t127
space0, param 3 = UAV table, params 9..16 = bindless buffer tables (spaces 1..8), root CBVs at
params 5..8, root constants at param 0. Captures before 0.0.4 list unused slots too; 0.0.4
filters by each shader's declared registers (PSV0 / RDEF).

## G-buffer and velocity

G-buffer render targets (at internal resolution): rgba16f (lit colour), rgba8srgb,
rgb10a2unorm, **rgba16snorm (velocity)**, depth r32g8x24 (d32s8). A full-screen pass then
writes camera motion into the rgba16snorm target using depth (frame-3 line 47186), so the
velocity buffer covers both static and moving geometry. Encoding (scale, sign) still to check.

## Resolution chain (the main risk)

At Image Quality 0.5 everything up to and including bloom and the final colour passes runs at
1920x1080. One full-screen pass (frame-3 line 56662) upscales to the 3840x2160 rgba16f target,
then the HUD is drawn at 4K (6-index quad draws) and a last pass writes the back buffer. FSR 1
Performance has the same structure. So the whole post chain runs at internal resolution:
DLSS replacing TAA would have to output at internal size, or the post passes after TAA would
have to run at output size.

## Post chain (confirmed with 0.0.4, captures/r2: frame-1 = Image Quality 1.0, frame-2 = 0.5, both HDR10)

In list L4, all at internal resolution unless noted:

| Pass | Shader | Reads | Writes |
|---|---|---|---|
| camera velocity | psc303a118 | t0 depth | rgba16snorm velocity (G-buffer RT 3) |
| lighting/fog ... | cs4206d838, psa2c098fe | G-buffer | rgba16f scene colour |
| depth history | copy | depth | prev depth |
| bloom-ish mips | cs58b193d0, csf1d0eec6, csa1d7d4cf | scene colour | mip chain |
| **TAA** | **pscf113c28** | t0 depth (r32g8x24), t1 scene colour (rgba16f), t2 history (rgba16f), t3 velocity (rgba16snorm); root CBVs b0, b1 | rgba16f TAA output |
| history save | copy | TAA output | history (read as t2 next frame) |
| bloom down/up | ps6388e1dd, psc0ec2e42 x6, ps554e83fe .. ps5e69525e | TAA output | 960x540 .. 15x8 chain |
| bloom composite | ps77ae4c63 | bloom | TAA output texture |
| tonemap/grade | ps3cc6333b (b4..b11 CBVs, LUTs) | depth, TAA output | rgba16f |
| sharpen (CAS?) | ps8195a264 | tonemapped | rgba16f |
| upscale (only below 1.0) | ps48155d5e | depth, two colour targets | 3840x2160 rgba16f |
| HUD | ps10e0b9fc, psa63129f6 (6-index quads) | UI atlases | 3840x2160 |
| output | psaf8e9bff (HDR encode; RenoDX) | 3840x2160 rgba16f | back buffer rgb10a2 |

So DLSS replaces pscf113c28: colour = t1, depth = t0, motion vectors = t3, output = its render
target (and the history copy can stay or be skipped). At Image Quality 1.0 this is DLAA with
no other changes. For upscaling, bloom, tonemap and sharpen run at internal size after TAA;
options are re-running ps3cc6333b (and ps8195a264) at output size on the DLSS output with the
bloom upsampled, or replacing ps48155d5e's input with a DLSS image tonemapped at output size.

## Camera constants and jitter (captures/r3: frame-1 = 1.0, frame-2 = 0.5)

TAA's root CBV b0 (graphics root param 3, also param 9) is the camera constant buffer, a 4 KB
block in a 32 MB upload buffer, double-buffered (offset alternates, e.g. +49664 / +51200):

| Byte | Contents |
|---|---|
| 0..159 | view-projection / view matrices |
| 160, 164 | projection x scale 1.42815, y scale 2.53893 (16:9) |
| **168, 172** | **jitter in clip space (x, y)** |
| 176..188 | 2.5e-6, -1, 0.01 (near), 0: reverse-Z |
| 192, 196 | tan(fov/2) x 0.700208, y 0.393867; 204 = 99.9997 |
| 208, 212 | jitter in view space (clip jitter x tan) |

Jitter in pixels = clip x (width/2, height/2):

| Capture | Frame | x px | y px |
|---|---|---|---|
| 3840x2160 | 1 | -0.15625 | -0.1759 |
| 3840x2160 | 2 | +0.09375 | -0.0093 |
| 1920x1080 | 1 | +0.125 | -0.1944 |
| 1920x1080 | 2 | -0.1875 | -0.0278 |

So jitter is in pixel units of the render size, within +-0.5 px (x in 1/32 steps). Sign
convention for DLSS (clip y up vs pixel y down) to be settled when testing.

Root CBV b1 (+271616) held previous/current view matrices in one dump and zeros in others:
written after the command list was recorded, so dumps of it aren't reliable.

## TAA shader (pscf113c28, `PreTonemap2_PS`)

Bytecode saved by 0.0.5 (`shaders/VillageDLSS-pscf113c28.cso`), disassembled with the Windows
SDK's `dxc -dumpbin` (`.txt` next to it). DXIL with full reflection names:

- cb0 `SceneInfo` (528 bytes): viewProjMat 0, transposeViewMat 64, transposeViewInvMat 112,
  **projElement[2] 160** (projElement[0].zw = jitter, clip space), projInvElements[2] 192,
  viewProjInvMat 224, prevViewProjMat 288, ZToLinear 352, **screenSize 368, screenInverseSize
  376**, cameraNearPlane 392, cameraFarPlane 396, viewFrustum 400, clipplane 496.
- cb1 `Tonemap`: exposureAdjustment, tonemapRange, sharpness, preTonemapRange (12), useAutoExposure,
  echoBlend, AABlend (24), AASubPixel, ResponsiveAARate.
- t0 `ReadonlyDepth`, t1 `HDRImage`, t2 `PrevHDRImage`, t3 `Velocity`; s0 PointClamp, s1 BilinearClamp.
- Input `Exposure` comes from the vertex shader (auto exposure).

What it does:
1. Current colour = HDRImage at `(pos - projElement[0].zw * 0.5) * screenInverseSize`, times Exposure.
2. Closest depth in a 5-tap cross (reverse-Z: largest depth = nearest).
3. **Velocity at that pixel: prevUV = uv + (v.x * 0.5, -v.y * 0.5).** So Velocity.xy is the NDC
   (clip xy, y up) offset from the current to the previous position.
4. History at prevUV, neighbourhood clamp in a Reinhard space (preTonemapRange), blend.
5. Output = resolved / Exposure (max 0): **same un-exposed space as HDRImage**.

## DLAA result (0.1.0, 2026-11-02)

Works at Image Quality 1.0: "TAA replaced by DLSS: 3840x2160 -> 3840x2160", DLAA mode, preset K.
User: looks good, no flips needed, so the jitter sign is jitterX = -clip.x * w/2,
jitterY = +clip.y * h/2. NGX takes about 5 s to start the first time DLSS is switched on (one
stall). NGX's "failed to load NGXCore" and model cache lines in the log are its normal probing.

## Upscaling (0.2.0 / 0.2.1, 2026-11-02)

Confirmed by the user at Image Quality 0.5..0.7 (1920x1080, 2304x1296, 2688x1512 -> 3840x2160):
no issues. Below the output size: DLSS writes an output-size image while the game's TAA still
runs at render size; NewFinal (ps77ae4c63, bloom, texture coordinates) is drawn again into it;
LDRPostProcess_WithTonemap (ps3cc6333b, reads t1 by pixel position and SceneInfo.screenSize) is
drawn only at output size, with t1 rewritten in its descriptor table and a SceneInfo copy with
the output size; CopyImageWithDepthPS (ps48155d5e) runs (it writes the output-size depth) and
the output-size image is copied over its colour.

The tonemap's descriptor tables are ring-allocated per frame (hundreds of different tables in
a session), so rewritten slots are never put back (0.2.0 did, wrongly; fixed in 0.2.1).
Transition frames with other sizes (a 1864x1048 r11g11b10f target, a 3840x2097 one) fall back
to the game's stretch. Not done: the game's sharpening (Sharpness_PS) at output size, texture
mip bias for the lower render size.

## Sharpening, texture detail, DLSS modes (0.3.0 - 0.4.2, 2026-11-02)

- Sharpness_PS (ps8195a264, reads t0 by texture coordinates and SceneInfo.screenInverseSize) is
  redrawn at output size like the tonemap, so the game's sharpness setting applies.
- Texture detail: mip bias log2(render scale) on material samplers (wrap + anisotropic or
  trilinear) at creation; 5 such samplers in this game (25 others). Needs a restart.
- The render scale is via.render.RenderConfig (native singleton) ImageQualityRate, found with a
  REFramework Lua probe (docs/probe). reframework/VillageDLSS.lua sets it from the add-on's DLSS
  mode (file reframework/data/VillageDLSS_mode.txt, status back in VillageDLSS_status.txt).
  RenderConfig also has SamplerQuality, StreamingTextureLoadLevelBias, UpscaleType.
- At 0.667 and 0.58 the game's post chain (TAA target, tonemap target) is r11g11b10f while the
  output target stays rgba16f; at 0.5 and 0.333 both are rgba16f. So the output-size image goes
  over the output target with our own full-screen pass (src/blit.hlsl), which converts formats.
- Confirmed by the user: DLAA 3840x2160, Quality 2561x1440, Balanced 2227x1252, Performance
  1920x1080, Ultra Performance 1278x719 all upscale to 3840x2160.

## DLSS inputs (from the above)

| DLSS | Source |
|---|---|
| colour | TAA t1 HDRImage, rgba16f, render size; DLSS auto exposure |
| depth | TAA t0, r32g8x24 typeless (view as R32_FLOAT_X8X24), **inverted (reverse-Z)** |
| motion vectors | TAA t3 rgba16snorm .xy, **scale (0.5 x width, -0.5 x height)** to pixels, low-res, not jittered |
| jitter | SceneInfo projElement[0].zw x (width/2, height/2) px; sign to settle in testing |
| output | TAA render target (rgba16f); the history copy after it can stay |
| reset | camera cuts: not found yet (big jumps in prevViewProjMat vs viewProjMat could do) |
