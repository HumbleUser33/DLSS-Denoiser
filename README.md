# DLSS Denoiser

> [!WARNING]
> **Abandoned project.** The process is far too slow, and its quality falls short of a classic
> denoising filter.

A VapourSynth filter that takes the grain out of a film before it is encoded, using NVIDIA
DLSS Super Resolution in **DLAA mode** (the output is the size of the input). It is made for
4K HDR (PQ) sources and slow offline encodes, in StaxRip or with vspipe: every default is
the best quality measured, whatever it costs in speed.

It does one thing: no resizing (put your resize filter after it), and no HDR metadata
handling (the encoder's front end carries it).

```
source -> crop -> DLSS Denoiser -> resize (Spline36, Lanczos...) -> encoder
```

> **NVIDIA's DLSS DLL is not included. Download it separately.**
> Get `nvngx_dlss.dll` from NVIDIA's public [DLSS repository](https://github.com/NVIDIA/DLSS),
> folder `lib/Windows_x86_64/rel` (developed with version 310.9.16). It is NVIDIA's property,
> under the NVIDIA RTX SDKs licence, and this project neither ships nor redistributes it.

## Requirements

| | |
|---|---|
| GPU | NVIDIA RTX. Ampere or later for a motion vector per pixel. Developed on an RTX 4060 |
| Driver | a recent one: it brings the NGX runtime and NVIDIA Optical Flow (`nvofapi64.dll`) |
| DLL | `nvngx_dlss.dll`, **downloaded separately** (see above) |
| VapourSynth | R79 (API 4.2) or later, x64 |
| Source | YUV, PQ transfer (HDR10, or the base layer of Dolby Vision profile 8). SDR is refused for now |

## Installation

**In StaxRip**
1. Copy `vsdlss.dll` and `python/dlssdenoise.py` into `StaxRip\Settings\Plugins\VapourSynth\`.
2. Put `nvngx_dlss.dll` in a **subfolder**: `StaxRip\Settings\Plugins\VapourSynth\dlss\`.
   StaxRip loads every DLL of the plugin folder as a VapourSynth plugin, and NVIDIA's is not
   one.
3. Filters menu, right-click, *Edit Profiles...*: paste `staxrip/VapourSynth-profile.txt`.
   It names StaxRip's folder: change the path if yours is elsewhere.
4. Add *DLSS Denoiser* to the job, after the crop and before any resize.

**In any VapourSynth script**: load `vsdlss.dll`, put `dlssdenoise.py` on Python's path, and
point `dll=` at NVIDIA's DLL (or put it in a `dlss\` folder next to `vsdlss.dll`).

## Quick start

```python
import dlssdenoise
clip = dlssdenoise.DLSSDenoise(clip)                    # all defaults: the best measured quality
clip = dlssdenoise.DLSSDenoise(clip, linear=0)          # keeps more nuance in dark scenes
clip = dlssdenoise.DLSSDenoise(clip, strength=0.7)      # keeps 30 % of the grain
```

## Parameters

`DLSSDenoise(clip, **parameters)`. The full manual, with what each setting was measured to
be worth, is [docs/MANUAL.md](docs/MANUAL.md).

**Grain kept**

| Parameter | Default | Values | Effect |
|---|---|---|---|
| `strength` | 1.0 | 0.0 to 1.0 | Mix of source and DLSS. 1.0 = all of DLSS, 0.0 = the source bit for bit, 0.5 = half the grain back. |
| `limit` | None | 10-bit code values | Soft limiter: a change up to `limit` is kept whole, larger ones fade out up to `limit × elast`. Protects edges; grain is several codes in 4K, so start around 8 to 12. |
| `elast` | 2.0 | > 1 | Width of `limit`'s fade. |
| `chroma` | "dlss" | "dlss", "source" | "source" keeps the source's chroma planes untouched. |

**DLSS**

| Parameter | Default | Values | Effect |
|---|---|---|---|
| `passes` | 2 | 1, 2 | 2: the film runs through DLSS forward and backward, weighted by the history each holds. No grain coming back after cuts. 1: forward only, about three times faster. |
| `preset` | "J" | "J", "K", "L", "M" | DLSS model. J and K are even (J flickers least). L and M leave far more grain. |
| `linear` | 1 | 0, 1 | 1: linear light with IsHDR, as games feed DLSS. 0: PQ values in LDR mode. Keeps the folds and nuance of dark scenes better, with a little more grain and flicker. |
| `exposure` | 1.0 | > 0 | Exposure given to DLSS with `linear=1` (1.0 = 100 nits). |
| `auto_exposure` | 0 | 0, 1 | Let DLSS measure exposure itself. Kept less detail. |
| `pad` | 32 | 0 to 256 | Mirrored guard band around the picture, pixels. |

**Motion vectors**

| Parameter | Default | Values | Effect |
|---|---|---|---|
| `tiles` | 1 | 0, 1, 16 to 512 | Untrusted areas take the motion of their tile (64 px, or the given size) instead of the whole picture's. Keeps an out-of-focus background in place behind a subject the camera follows. 0: the whole picture's motion. |
| `flow_factor` | 1 | 1 to 4 | Picture divider for NVIDIA Optical Flow. 1 = full resolution. |
| `grid` | 1 | 1, 2, 4 | Optical Flow pixels per vector. 1 = a vector per pixel. |
| `perf` | 0 | 0, 1, 2 | Optical Flow effort: slow, medium, fast. |
| `flow_blur` | 1 | 0 to 4 | Pre-blur of the frames Optical Flow searches, against grain. |
| `prefilter` | None | "bm3d", "dfttest", a clip | Motion measured on a pre-denoised copy. DLSS still filters the original. |
| `vectors` | "clean" | "clean", "raw", "global" | How Optical Flow's vectors are cleaned. For tests. |
| `motion` | 1 | 0, 1 | 0: no vectors. For tests only: a pan loses about 7 dB. |

**Cuts, scheduling, system**

| Parameter | Default | Values | Effect |
|---|---|---|---|
| `scenecut` | 3 | 0 to 3 | 1: GPU cut detection, 2: the `_SceneChangePrev` property, 3: both, 0: none. Each pass starts its history over on a cut. |
| `chunk`, `lookahead` | 24, 24 | frames | Backward pass block size and how far past it it starts. |
| `preroll` | 24 | frames | Frames run again before a frame asked for out of order (preview, seek). |
| `half` | False | True, False | 16-bit float planes between wrapper and plugin: same result, far less RAM. |
| `dll`, `device` | | path, index | Where `nvngx_dlss.dll` is; which NVIDIA GPU. |

## What to expect

Perceived effect, from the measurements in [docs/REPORT.md](docs/REPORT.md): 4K extracts of a
heavily grained film (The Uprising), and synthetic film frames with exact motion. More dots is
better. Speed: 3840x2160, RTX 4060.

| Setting | Grain removed | Detail / sharpness | Background stability | Dark scenes | Speed |
|---|---|---|---|---|---|
| **Defaults** (J, 2 passes, `linear=1`, tiles) | ●●●●● | ●●●○○ | ●●●●○ | ●●○○○ | 0.95 fps |
| `linear=0` | ●●●●○ | ●●●○○ | ●●●●○ | ●●●●○ | same |
| `passes=1` | ●●●○○ grain back after each cut | ●●●○○ | ●●●○○ | ●●○○○ | 3.1 fps |
| `tiles=0` | ●●●●● | ●●○○○ background blurred | ●○○○○ 2 to 3 px off behind a followed subject | ●●○○○ | 1.4 fps |
| `strength=0.7` | ●●●○○ | ●●●●○ | ●●●●○ | ●●●○○ | same |
| `limit=8` | ●●○○○ | ●●●●○ edges protected | ●●●●○ | ●●●○○ | same |
| `preset="K"` | ●●●●● | ●●●○○ | ●●●●○ | ●●○○○ | same |
| `preset="L"` or `"M"` | ●○○○○ | ●●●●○ | ●●●●○ | ●●●○○ | same |
| `grid=4` | ●●●●○ | ●●●○○ | ●●●○○ | ●●○○○ | faster |
| *FFT3DGPU sigma 4, sharpen 0.4 (for reference)* | ●●●○○ coarse grain left | ●●●●○ sharpened | ●●●●● | ●●●●○ | fast |

In short: DLSS takes out more grain than FFT3DGPU, and comes closer to a clean picture in every
frequency band measured, but looks softer, since FFT3DGPU leaves coarse grain and sharpens. A
moving subject under heavy grain is the hardest case for the motion vectors.

## How it works

- **DLAA only.** A video brings DLSS no jitter, so no new detail: what is left is a temporal
  filter along the motion vectors, which takes the grain out.
- **Linear light with IsHDR**, as games feed DLSS before tone mapping. In LDR mode DLSS
  quantizes its input to 8 bits (programming guide 310.6.0, §3.1.2).
- **Motion vectors** from NVIDIA Optical Flow at full resolution, a vector per pixel, the
  slowest search, cleaned the way [MPCVR-Plus](https://github.com/HumbleUser33/MPCVR-Plus)
  cleans them, then refined per tile against parallax.
- **Cuts** become DLSS resets on the exact frame. **Two passes** (forward and backward) remove
  the grain that comes back at the start of every shot.

## Building

```
build.cmd           shaders, bench and plugin
build.cmd plugin    shaders and plugin
```

MSVC 14.44 (the v143 tools of Visual Studio 2022 or 2026). `VSINC` may name VapourSynth's API 4
headers; StaxRip's are the default. `bench/` is the measurement harness; `vs/` holds the
analysis scripts used for the report (they name local paths).

## Licence and credits

GPL v3 or later (`LICENSE.txt`). The DLSS, Optical Flow and motion code derives from
[MPCVR-Plus](https://github.com/HumbleUser33/MPCVR-Plus), GPL v3 or later. The Optical Flow
SDK headers in `src/NvOF` carry NVIDIA's own notice. `nvngx_dlss.dll` is NVIDIA's property,
used under the NVIDIA RTX SDKs licence and never distributed with this project.

---

Coded with Claude 55 Opus Extra, tested by human
