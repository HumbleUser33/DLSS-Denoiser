# Changelog

## v0.1 beta — 2026-10-18

First public release, commit `1bfeb7f`. It gathers the development steps below.

### Engine and plugin (2026-10-17)

- `vsdlss.dll` (`core.dlss.Denoise`) runs NVIDIA DLSS Super Resolution in DLAA mode on PQ
  R'G'B' planes: same size in and out, no resize, no HDR metadata.
- The picture goes to DLSS as linear light with IsHDR and preset J, since LDR mode quantizes
  its input to 8 bits.
- A 32-pixel mirrored guard band keeps DLSS's edge effect off the picture.
- Motion vectors come from NVIDIA Optical Flow at full resolution, a vector per pixel, at the
  slowest search. They are cleaned as MPCVR-Plus cleans them, with the windows kept at their
  measured reach in source pixels.
- A cut, detected on the GPU or read from `_SceneChangePrev`, becomes a DLSS reset on that very
  frame. Frames go through in order, with a preroll after a seek.
- `dlssdenoise.py` converts YUV to R'G'B' and back with zimg, and adds `strength`, a soft
  `limit`, `chroma` and a `prefilter` for the motion search.
- `dlssdn_bench` measures ramp precision, domains, presets, convergence and vector accuracy
  against exact motion.

### Two passes (2026-10-17)

- `passes=2`, the default, runs the film through DLSS forward and backward. Each frame is
  weighted by the history each pass holds, so grain no longer comes back after cuts.
- On a pan cut in two, grain left falls from 3.26 to 1.96. On a 4K extract, high-frequency
  energy is 18 % lower than with one pass.

### Manual, StaxRip validation, fixes (2026-10-17)

- `docs/MANUAL.md` documents every parameter with its range, effect and measured value.
- A full StaxRip AV1 encode of a 15 s extract gives a valid HDR10 and Dolby Vision file. It is
  29 % smaller than without denoising and 23 % smaller than FFT3DGPU.
- Fixed: `prefilter="dfttest"` did nothing, because DFTTest2 returns integer clips untouched;
  it now gets float input.
- Fixed: in StaxRip, the prefilters' plugins were never loaded. They are now loaded from
  StaxRip's plugin folder.
- Fixed: a second plugin instance failed after another one shut NGX down; NGX sessions are now
  reference-counted.

### Offline motion (2026-10-18)

- New `tiles` (default 1): untrusted areas take the motion of their 64-pixel tile instead of the
  whole picture's. An out-of-focus background behind a subject the camera follows stays in
  place: it was 2.7 px or more off, now 0.2 to 0.7 px.
- Each tile is refined by Gauss-Newton steps on blurred frames. A tile with no texture above
  the grain falls back to the global motion, and a 5×5 median removes tiles that the grain
  pushed off.
- Tile refinement reads precomputed values and gradients: rendering one frame went from 95 s to
  36 s. In 4K, two passes run at 0.95 frames/s, against 1.38 without tiles.
- New `vectors` option (clean, raw, global) for tests. New `debug=1` returns the motion vectors
  instead of the picture.
- New scripts in `vs/` compare raw 4K frames against FFT3DGPU: shifts, detail bands, closeness
  to a clean reference, dark folds, crop boards.

### Repository

- Everything is in English. The phase report becomes `docs/REPORT.md`, and the project is
  renamed DLSS Denoiser.
- An abandoned multi-scale "pyramid" motion approach was removed before release.
- `nvngx_dlss.dll` is not included: download it separately from NVIDIA.
