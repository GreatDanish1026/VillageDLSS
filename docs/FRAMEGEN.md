# FSR and frame generation (1.1.0, 2026-11-06)

Ported from RE7DLSS 1.1.1 (itself from RE4DLSS 1.2.0: `src/fsr.cpp`, `src/framegen.cpp`,
`src/fsrfg.cpp`; RE7DLSS's and RE4DLSS's docs have the background). The upscaler code is RE7DLSS's
with this game's shader ids (top of `src/upscaler.cpp`).

## FSR 3.1 / FSR 4

Same hook as DLSS: the TAA draw (PreTonemap2_PS). Inputs depth, colour and velocity (rgba16snorm
NDC offsets, y up: scale (0.5 w, -0.5 h)) from the TAA's own slots, output X (the DLSS output
texture). Camera: near = SceneInfo cameraNearPlane (byte 392), FOV = 2 atan(1 / projElement[0].y),
far infinite (reversed Z), metres. No reactive mask.

## Frame generation

- Inputs at the TAA draw, whoever upscales (also the game's TAA): depth (state 0xE0, copied),
  velocity (0xC0), DLSS-G mvecScale (0.5, -0.5), FSR FG motion scale (0.5 w, -0.5 h); SceneInfo
  bound right there; reset on a camera cut.
- Picture before the HUD: the HUD is drawn into the output-size texture the output pass writes to
  the back buffer (its t0). That texture is copied once the scene is in it: after the game's CAS_CS
  or our blit into it, or else (new here, instead of RE7's native-size fill pass id) just before
  the first draw into it that isn't a full-screen triangle, i.e. the first HUD element. After the
  output pass, the copy goes back into it and the output pass is repeated into our
  back-buffer-format target.
- Exclusive full screen: the proxy's SetFullscreenState retries on
  DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (3 s), as in RE7DLSS.
- REFramework: its hook on sl.dlss_g.dll would hang the game at launch; the add-on steps around it
  while the game's swapchain is created (from RE4DLSS).
