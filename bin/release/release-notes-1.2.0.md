**Experimental DLSS Ray Reconstruction** for Resident Evil Village's ray tracing (as RE2DLSS, RE3DLSS and RE7DLSS 1.2.0).

- New **Ray Reconstruction** option (Settings tab, DLSS, off by default): with the game's ray tracing on, NVIDIA's DLSS Ray Reconstruction replaces DLSS Super Resolution. It upscales and cleans up the remaining noise in the ray-traced lighting and reflections, on top of the game's own ray tracing denoiser, using the game's surface data (G-buffer) and camera. Without ray tracing the add-on uses Super Resolution as before.
- **Ray Reconstruction model**: NVIDIA's default, D, E or F. An override in the NVIDIA App takes precedence (set it to "Use app default" there to choose here).
- Advanced tab, for testing: **Game's ray tracing denoiser** can skip the game's temporal filtering (or all of its denoiser) and hand Ray Reconstruction the rays directly. The game traces at half resolution, so that comes out blotchier; keeping the game's denoiser is the default. **Show guide** displays the surface pictures Ray Reconstruction gets.

**Install**: replace the previous version with `VillageDLSS-1.2.0.zip` in Fluffy Mod Manager (or copy its files over the old ones). New file: `nvngx_dlssd.dll` (NVIDIA's Ray Reconstruction runtime 310.9.16, signed), next to `re8.exe` and in `streamline\`.

Tested at 3840x2160 HDR (RenoDX), DLAA and Quality, with ray tracing on, on an RTX 5080.
