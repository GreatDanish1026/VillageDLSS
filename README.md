# VillageDLSS 1.0.1

ReShade add-on for **Resident Evil Village** (RE Engine, DirectX 12) that replaces the game's
TAA with **NVIDIA DLSS**. The game only has FSR 1 (a spatial upscaler); this gives it DLAA at
native resolution and DLSS upscaling from Quality down to Ultra Performance, with the game's own
bloom, tonemapping (and RenoDX HDR), sharpening and HUD on top of the DLSS image.

NVIDIA RTX graphics card. ReShade 6.8 or later with add-on support, and REFramework for the DLSS
mode dropdown.

## What it does

- **DLSS in place of TAA** (F9 to switch on and off): DLSS Super Resolution 310.9.16 takes the
  game's own colour, depth, motion vectors and camera jitter.
- **DLSS mode**: DLAA (100%), Quality (67%), Balanced (58%), Performance (50%) or Ultra
  Performance (33%) set the game's render scale (its Image Quality setting) while DLSS is on, and
  keep it there if the in-game slider moves. "Game's Image Quality slider" leaves it to the menu.
  This part needs REFramework: the bundled script `reframework\autorun\VillageDLSS.lua` sets the
  render scale.
- **Upscaling**: below 100%, DLSS renders the full output resolution, and the game's bloom,
  tonemapping/colour grading (and RenoDX's HDR, if installed) and sharpening run on it at full
  resolution. The HUD is drawn as usual.
- **Game sharpening at output size** (on by default): the game's own sharpening (its Sharpness
  setting) on the DLSS image.
- **Sharper textures at lower render sizes** (on by default): a mip bias of log2(render scale) on
  the game's material textures, as NVIDIA recommends for DLSS. Set when the game starts, so after
  changing the DLSS mode, restart the game for the textures to follow.
- **DLSS model**: Automatic (default: K, M at Performance, L at Ultra Performance, as NVIDIA recommends), or K, J, L or M (DLSS 4.5) at every resolution, or NVIDIA's default.

Settings are in ReShade's Add-ons tab (Home key) under VillageDLSS, saved in `ReShade.ini` under
`[VillageDLSS]`.

## Install

**With Fluffy Mod Manager**: put `VillageDLSS-1.0.1.zip` in Fluffy Mod Manager's
`Games\RE8\Mods` folder (or drag it onto the manager) and enable it. It installs into the game
folder:

- `VillageDLSS.addon64` (the add-on) and `nvngx_dlss.dll` (NVIDIA's DLSS runtime, 310.9.16)
- `reframework\autorun\VillageDLSS.lua` (sets the render scale for the DLSS mode)
- `VillageDLSS-README.md`, `VillageDLSS-THIRD_PARTY_NOTICES.txt`, `VillageDLSS-licenses\`

**By hand**: copy the contents of the zip's `VillageDLSS` folder into the game folder (where
`re8.exe` is), leaving out `modinfo.ini`.

Then:

1. ReShade 6.8 or later with full add-on support must be installed for `re8.exe` (DirectX 12).
2. REFramework must be installed for the DLSS mode dropdown (without it, use the game's Image
   Quality slider: 0.67 = Quality, 0.58 = Balanced, 0.5 = Performance).
3. In the game, keep Anti-Aliasing on TAA (DLSS replaces that pass), and FidelityFX Super
   Resolution off.
4. Open ReShade (Home), Add-ons tab, VillageDLSS: DLSS is on by default; pick a DLSS mode.

If the NVIDIA App has a DLSS model override for the game, it takes precedence over the model here.

## Notes

- Tested at 3840x2160 with HDR (RenoDX) on an NVIDIA RTX card, at every DLSS mode.
- A few frames during loading may show the game's own upscaling; that's expected.
- The render scale the DLSS mode sets is the game's Image Quality setting, so the game saves it in
  `config.ini`. With DLSS off (F9) or the mode on "Game's Image Quality slider", the script puts the
  game's previous value back.
- `VillageDLSS.log` next to `re8.exe` records what the add-on did and why it fell back, if it did.

## Building

`build.ps1` builds `bin\VillageDLSS.addon64` with Visual Studio 2022's C++ tools (MSVC):
`-Install` copies it, `nvngx_dlss.dll` and the REFramework script into the game folder;
`-Package` makes the Fluffy Mod Manager zip in `bin\release`. `nvngx_dlss.dll` isn't in the
repository: put NVIDIA's signed 310.9.16 file (from the Streamline SDK 2.14.16 release, `bin\x64`)
in `third_party\bin`. `docs\FINDINGS.md` describes how the game renders and what the add-on
changes.

## Licence

MIT (see `LICENSE`). Third-party code and NVIDIA's runtime: `THIRD_PARTY_NOTICES.txt`.
