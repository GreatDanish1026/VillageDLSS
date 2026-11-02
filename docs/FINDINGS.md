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

Velocity encoding: from the shader bytecode (0.0.5 saves DumpShaders' bytecode as
VillageDLSS-<id>.cso, disassembled with the Windows SDK's dxc -dumpbin).
