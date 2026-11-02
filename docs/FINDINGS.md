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

## TAA candidate

Frame-3 line 54040: full-screen pass writing #2a6a3 (1920x1080 rgba16f), which reads the velocity
texture, and is immediately copied to #2a60a (history for next frame). At native it's frame-1 line
55979 (writes #3310b 3840x2160). Preceded by motion-blur-like tile reductions (csf1d0eec6,
csa1d7d4cf). To confirm with 0.0.4 (pixel shader id, declared inputs) and then dump its
constants for jitter.
