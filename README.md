# VillageDLSS 1.1.0

ReShade add-on for **Resident Evil Village** (DirectX 12) that replaces the game's TAA
with **NVIDIA DLSS** or **AMD FSR 3.1 / FSR 4**, and adds **DLSS or FSR frame generation**. The game
only has FSR 1 (a spatial upscaler); this gives it native anti-aliasing (DLAA) and upscaling from
Quality to Performance, with the game's bloom, tonemapping (and RenoDX HDR) and sharpening on top of the upscaled
image.

DLSS: NVIDIA RTX graphics card (frame generation: RTX 40/50). FSR 3.1: any DirectX 12 graphics
card; FSR 4: Radeon cards that support it (AMD's runtime picks). ReShade 6.8 or later with add-on
support, and REFramework for the render resolution dropdown.

## What it does

- **DLSS or FSR 3/4 in place of TAA** (F9 to switch on and off): DLSS Super Resolution 310.9.16,
  or AMD FSR (SDK 2.4.0: FSR 4 where the GPU supports it, FSR 3.1 elsewhere), takes the game's own
  colour, depth, motion vectors and camera jitter.
- **Upscaler**: Auto (default: DLSS on RTX cards, FSR otherwise), DLSS, or FSR 3.1 / FSR 4.
- **Render resolution**: Native (100%: DLAA, or FSR's native anti-aliasing), Quality (67%),
  Balanced (58%), Performance (50%) or Ultra Performance (33%) set the game's render scale (its Image Quality setting) while
  upscaling is on, and keep it there if the in-game slider moves. "Game's Image Quality slider"
  leaves it to the menu. This needs REFramework: the bundled script
  `reframework\autorun\VillageDLSS.lua` sets the render scale.
- **Upscaling**: below 100%, DLSS/FSR render the full output resolution, and the game's
  bloom, tonemapping/colour grading (and RenoDX's HDR, if installed) and sharpening run on it at full resolution. The HUD is drawn as usual. The game's sharpening (its Sharpness setting) runs on the upscaled image; the Advanced tab
  can switch that off.
- **Frame generation**: Off (default), DLSS 2x / 3x / 4x / Dynamic (NVIDIA Streamline 2.14.16,
  RTX 40/50; 3x and 4x need an RTX 50; with NVIDIA Reflex), or FSR 2x (any DirectX 12 card). It
  uses the game's depth, motion vectors and camera, works with either upscaler (and with the
  game's own TAA), and gets the picture without the HUD so the HUD doesn't warp. Exclusive full
  screen works. Switching it on, or between DLSS and FSR, takes a game
  restart.
- **Sharper textures at lower render sizes** (on by default): a mip bias of log2(render scale) on
  the game's material textures, as NVIDIA recommends. Set when the game starts, so after changing
  the render resolution, restart the game for the textures to follow.
- **DLSS model**: Automatic (default: K, M at Performance, L at Ultra Performance, as NVIDIA recommends), or K, J, L or M (DLSS 4.5) at every resolution, or NVIDIA's default.

Settings are in ReShade's Add-ons tab (Home key) under VillageDLSS: what's running at the top, then
Settings, Status and Advanced tabs. They're saved in `ReShade.ini` under `[VillageDLSS]`.

## Install

**With Fluffy Mod Manager**: put `VillageDLSS-1.1.0.zip` in Fluffy Mod Manager's `Games\RE8\Mods`
folder (or drag it onto the manager) and enable it. It installs into the game folder:

- `VillageDLSS.addon64` (the add-on) and `nvngx_dlss.dll` (NVIDIA's DLSS runtime, 310.9.16)
- `streamline\` (NVIDIA's frame generation runtime, Streamline 2.14.16, with a copy of
  `nvngx_dlss.dll`)
- `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and
  `amd_fidelityfx_framegeneration_dx12.dll` (AMD's FSR runtime, SDK 2.4.0)
- `reframework\autorun\VillageDLSS.lua` (sets the render scale for the render resolution dropdown)
- `VillageDLSS-README.md`, `VillageDLSS-THIRD_PARTY_NOTICES.txt`, `VillageDLSS-licenses\`

**By hand**: copy the contents of the zip's `VillageDLSS` folder into the game folder (where
`re8.exe` is), leaving out `modinfo.ini`.

Then:

1. ReShade 6.8 or later with full add-on support must be installed for `re8.exe` (DirectX 12).
2. REFramework must be installed for the render resolution dropdown (without it, use the game's
   Image Quality slider: 0.67 = Quality, 0.58 = Balanced, 0.5 = Performance).
3. In the game, keep Anti-Aliasing on TAA (DLSS/FSR replace that pass) and FidelityFX Super
   Resolution off.
4. Open ReShade (Home), Add-ons tab, VillageDLSS: upscaling is on by default (DLSS on RTX cards, FSR
   otherwise); pick a render resolution and, if you like, frame generation.

If the NVIDIA App has a DLSS model override for the game, it takes precedence over the model here.

## Notes

- **1.1.0 is untested in Village** (the game isn't installed here): the FSR and frame generation
  code is RE7DLSS 1.1.1's, tested in RE7 and RE2 (RE2DLSS 1.1.0). New in 1.1.0, Village's
  bloom, tonemapping and sharpening are redirected to full resolution the way RE2DLSS, RE3DLSS and
  RE7DLSS do it, instead of being drawn a second time. Please send `VillageDLSS.log` if something
  doesn't work (1.0.1 is the previous, tested version).
- Works with REFramework: REFramework hooks NVIDIA's frame generation DLL in a way that would hang
  the game at launch; the add-on steps around that hook while the game's swapchain is created.
- FSR 3.1/4 and FSR frame generation through this add-on are untested on Radeon cards so far.
- A few frames during loading may show the game's own upscaling; that's expected.
- The render scale the dropdown sets is the game's Image Quality setting, so the game saves it in
  `config.ini`. With upscaling off (F9) or the dropdown on "Game's Image Quality slider", the
  script puts the game's previous value back.
- `VillageDLSS.log` next to `re8.exe` records what the add-on did and why it fell back, if it did;
  `streamline\sl.log` is NVIDIA's frame generation log.

## Building

`build.ps1` builds `bin\VillageDLSS.addon64` with Visual Studio 2022's C++ tools (MSVC): `-Install`
copies it, the NVIDIA and AMD runtimes and the REFramework script into the game folder;
`-Package` makes the Fluffy Mod Manager zip in `bin\release`. The runtimes aren't in the
repository: put NVIDIA's signed `nvngx_dlss.dll` 310.9.16 in `third_party\bin`, the Streamline
2.14.16 release's `bin\x64` in `third_party\streamline\bin\x64`, and AMD's signed FSR SDK 2.4.0 DLLs
(`Kits\FidelityFX\signedbin`) in `third_party\fidelityfx\bin`. `docs\FINDINGS.md` describes how the
game renders and what the add-on changes (RE2DLSS, RE3DLSS and RE7DLSS are forks of it); `docs\FRAMEGEN.md` covers FSR and frame
generation.

## Licence

MIT (see `LICENSE`). Third-party code and NVIDIA's and AMD's runtimes: `THIRD_PARTY_NOTICES.txt`.
