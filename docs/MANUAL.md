# DLSS Denoiser — manual

DLSS Denoiser takes the grain out of a film before it is encoded. It runs NVIDIA DLSS
Super Resolution in DLAA mode, where the output is the size of the input, on a 4K HDR
source in VapourSynth.

It never resizes and never touches HDR metadata. Place it **after the crop and before any
resize**:

```
source -> crop -> DLSS Denoiser -> resize (Spline36, Lanczos...) -> encoder
```

Every figure below was measured on an RTX 4060 (driver 616.56) with `nvngx_dlss.dll`
310.9.16. How each was measured is in [REPORT.md](REPORT.md).

---

## 1. Requirements

| | |
|---|---|
| GPU | NVIDIA RTX (Turing or later; Ampere or later for Optical Flow grids of 1 and 2) |
| Driver | one with the NGX runtime and `nvofapi64.dll` (any recent Game Ready or Studio driver) |
| DLL | `nvngx_dlss.dll` from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS), `lib/Windows_x86_64/rel`. Not included |
| VapourSynth | R79 or later, x64 |
| Source | YUV, PQ transfer (HDR10, or the base layer of Dolby Vision profile 8), any bit depth. SDR is refused for now |
| Memory | measured in 4K, two passes, chunk and look-ahead of 24: 3.6 GB of VRAM; vspipe peaked at 11.9 GB of RAM, **7.0 GB with `half=True`** |

## 2. Installation

### In StaxRip

1. Copy `vsdlss.dll` and `dlssdenoise.py` into `StaxRip\Settings\Plugins\VapourSynth\`.
2. Copy `nvngx_dlss.dll` into a **subfolder**: `StaxRip\Settings\Plugins\VapourSynth\dlss\`.
   StaxRip loads every DLL in the top folder as a VapourSynth plugin, and NVIDIA's is not
   one.
3. Choose VapourSynth as the frame server: the filter has no AviSynth version.
4. Open the filters menu, right-click, choose *Edit Profiles...*, and paste the contents of
   `staxrip\VapourSynth-profile.txt`. The path it names is the StaxRip folder; change it if
   yours is elsewhere.
5. Add *DLSS Denoiser* to the job, between the crop and the resize.

### In any VapourSynth script

```python
import sys
sys.path.append(r"<folder of dlssdenoise.py>")
import dlssdenoise
core.std.LoadPlugin(r"<folder>\vsdlss.dll")    # unless VapourSynth autoloads it

clip = dlssdenoise.DLSSDenoise(clip)
```

The plugin looks for `nvngx_dlss.dll` in these places, in order:
1. `dlss\` beside `vsdlss.dll`;
2. beside `vsdlss.dll`;
3. `..\dlss\`;
4. `..`.

`dll=` overrides the search.

## 3. Quick start

```python
clip = dlssdenoise.DLSSDenoise(clip)                       # maximum quality, the defaults
clip = dlssdenoise.DLSSDenoise(clip, passes=1)              # 3 times faster, more grain after cuts
clip = dlssdenoise.DLSSDenoise(clip, strength=0.7)          # keep 30 % of the grain
clip = dlssdenoise.DLSSDenoise(clip, prefilter="bm3d")      # motion measured on a BM3D copy
```

---

## 4. Parameters of `dlssdenoise.DLSSDenoise`

`DLSSDenoise(clip, strength=1.0, limit=None, elast=2.0, chroma="dlss", prefilter=None, half=False, **dlss)`

All the arguments in §4.2 to §4.6 go through `**dlss` to `core.dlss.Denoise`.

### 4.1 How much of DLSS's picture is kept

These act on the result, in the clip's own format and in PQ, after DLSS.

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `strength` | 1.0 | 0.0 to 1.0 | The mix of source and DLSS. **0.0 returns the source bit for bit** and skips DLSS altogether. 0.5 halves every change (measured: 3.83 → 1.92 codes of mean luma change). Use it to keep some of the film's grain. |
| `limit` | None | code values (10-bit scale) | A soft limiter. A change up to `limit` is kept whole; between `limit` and `limit × elast` it fades out; beyond, the source is kept. It protects edges and detail from changes larger than grain, **but grain itself is several codes in 4K**. On The Uprising, `limit=3` cut the mean change from 3.83 to 0.86 codes, keeping most of the grain. Start around 8 to 12 and judge by eye. |
| `elast` | 2.0 | > 1 | The width of `limit`'s fade, as a multiple of `limit`. |
| `chroma` | "dlss" | "dlss", "source" | "source" puts the source's chroma planes back. Chroma grain then stays, and DLSS cannot shift colours (measured shift with "dlss": under 0.02 code on average). |

### 4.2 Passes

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `passes` | 2 | 1, 2 | **2**: the film runs through DLSS forward and backward. Each frame is the mean of the two, weighted by the history each pass held. **1**: forward only, three times faster. |
| `chunk` | 24 | frames | The backward pass runs over this many frames at a time. Larger means less backward work per frame but more memory: a chunk of output frames is held. |
| `lookahead` | 24 | frames | The backward pass starts this many frames past the chunk, so that it has a history at the chunk's last frame. Backward work per frame is `(chunk + lookahead) / chunk`. |
| `preroll` | 24 | frames | After a seek (a preview, a request out of order), the forward pass starts again this many frames earlier. Encodes are sequential and never use it. |

**Why two passes.** DLSS's history keeps taking grain out for more than 30 frames after a
reset. Heavy grain left, in thousandths of PQ:

| Frames after a reset | 0 | 8 | 16 | 24 | 36 |
|---|---|---|---|---|---|
| Grain left | 9.1 | 3.1 | 2.3 | 1.95 | 1.67 |

So one pass leaves the first second of every shot grainy. The second pass has the whole
shot behind those frames.

| | One pass | Two passes |
|---|---|---|
| Pan cut in two, synthetic (grain left) | 3.26, and 9.1 on the first frame after the cut | **1.96**, between 1.91 and 2.08 everywhere |
| The Uprising, 48 frames (high-frequency luma energy; source 2.59) | 1.16 | **0.95** |

### 4.3 DLSS

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `preset` | "J" | "J", "K", "L", "M" (and "E", "F", deprecated) | The DLSS model. **J and K** (transformer) are within 0.3 dB of each other; J leaves the least flicker. **L and M** (DLSS 4.5) leave far more grain: 5 to 8 dB lower on average. Not for this use. |
| `linear` | 1 | 0, 1 | **1**: DLSS gets linear light (1.0 = 100 nits) with the IsHDR flag, as games feed it. **0**: PQ code values in LDR mode, as MPCVR-Plus does; NVIDIA states DLSS then quantizes its input to 8 bits. Measured on 0: no visible banding, but a third more flicker (0.37 against 0.28). |
| `exposure` | 1.0 | > 0 | With `linear=1`, the exposure value DLSS is given (read by J and K). 1.0 suits film, where diffuse white is about 100 to 200 nits. 2.0 changed little on The Uprising (mean change 3.84 against 3.83 codes). |
| `auto_exposure` | 0 | 0, 1 | DLSS measures the exposure itself. It kept less detail than the fixed exposure (0.92 against 0.94). Preset L always uses it. |
| `pad` | 32 | 0 to 256 pixels | A guard band mirrored around the picture, so DLSS's own edge effect lands outside it. |

### 4.4 Motion vectors

DLSS takes the grain out along the motion vectors. On a pan, vectors that are a fraction of
a pixel off cost several dB.

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `motion` | 1 | 0, 1 | 0: no vectors (zero motion). Still pictures are unaffected, but **a pan loses about 7 dB** and smears. Only for tests. |
| `flow_factor` | 1 | 1 to 4 | The divider of the picture for NVIDIA Optical Flow. **1 = the picture's own size, no divider.** |
| `grid` | 1 | 1, 2, 4 | Optical Flow pixels per vector: 1 = a vector per pixel (Ada and later), 4 = a vector per 4×4 block. |
| `perf` | 0 | 0 slow, 1 medium, 2 fast | Optical Flow's search effort. |
| `flow_blur` | 1 | 0 to 4 | Pixels the box filter reaches beyond each flow pixel before the search: it takes grain out of what Optical Flow matches. |
| `prefilter` | None | None, "bm3d", "dfttest", a clip | The motion is measured on this copy with the grain taken out, instead of on the grained picture. DLSS still filters the original. "bm3d" is BM3DCUDA `BM3Dv2(sigma=[3,2,2], radius=2)`; "dfttest" is DFTTest2 `sigma=8, tbsize=3`. Both are loaded from StaxRip's plugin folder if the script has not loaded them. A clip must have the source's format, size and length. |
| `tiles` | 1 | 0, 1, 16 to 512 | Where Optical Flow cannot be trusted, a block takes the motion of its tile of the picture (64×64 pixels for 1, or that size), measured on the frames themselves, instead of the motion of the whole picture. **It keeps an out-of-focus background in place when the camera follows a subject** (parallax). 0: the whole picture's motion, as MPCVR-Plus does. See below. |
| `vectors` | "clean" | "clean", "raw", "global" | "clean": Optical Flow's vectors, cleaned as MPCVR-Plus cleans them (each block drawn to its tile's motion unless it moves on its own and its neighbours agree). "raw": Optical Flow's own vectors, uncleaned. "global": the tiles' motion everywhere (or the whole picture's with `tiles=0`). For tests. |

**What the vector settings are worth.** Deficit against the exact motion, in dB, on
synthetic film frames with light and heavy grain:

| Vectors | Still | Pan 2 px/frame | Pan 0.7 px/frame | A subject moving on its own |
|---|---|---|---|---|
| **f1, grid 1, slow (default)** | 0.0 to −0.3 | 0.0 to −0.2 | 0.0 to −0.1 | −3.0 to −4.0 |
| f1, grid 2 | 0.0 to −0.3 | 0.0 to −0.2 | −0.1 | −2.6 to −4.5 |
| f1, grid 4 | −0.2 to −0.5 | −0.3 to −0.4 | −0.3 | −2.4 to −4.5 |
| f2, grid 2 | 0.0 to −0.2 | −0.2 to −0.8 | −1.2 to −2.1 | −2.3 to −4.5 |
| MPCVR-Plus real time (f4, grid 4, medium) | −0.4 | −4.1 to −9.4 | −4.1 to −6.0 | −2.4 to −3.8 |

Still pictures, pans and a film's weave are within a few tenths of a dB of perfect motion
at the defaults. A subject moving on its own under heavy grain is the weak point of every
setting, because Optical Flow cannot tell its motion from the grain's. Given the clean
picture to measure on, the same pipeline tracks it within 0.3 pixel. That is what
`prefilter` aims at; its gain on real films is not measured yet.

**Tiles: a background behind a subject.** On The Uprising's close-up (frame 56), the camera
follows the face, 5 to 11 pixels a frame, and the out-of-focus trees behind move 1 to 2
pixels a frame differently. Optical Flow cannot see the trees under the grain, and drawn to
the face's motion they came out of DLSS **2.7 pixels and more** from where they are
(`tiles=0`). With tiles they come out 0.2 to 0.7 pixel off (FFT3DGPU, which does not follow
motion, 0.1). Each tile starts from the picture's motion and takes 64 Gauss-Newton steps on
the two frames, heavily then lightly blurred against the grain; a tile with no texture above
the grain is drawn back to the picture's motion, and each tile is the median of the 5×5
around it, since grain moves tiles each their own way and a parallax moves them alike. On
the synthetic frames of the table above, tiles change still pictures and pans by −0.15 to
+0.1 dB, but under heavy grain they leave a third more flicker (0.80 against 0.61); a subject
moving on its own gains 0.7 dB under heavy grain and loses 1 dB under light grain, where its
tiles spill onto the background around it. They cost about 0.1 s a frame per pass in 4K
(0.95 frames/s against 1.38 with two passes).

### 4.5 Cuts

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `scenecut` | 3 | 0 to 3 | How a cut is found, so that each pass starts its history over on the right frame. **1**: detection on the GPU, on frames of about 540 lines, with MPCVR-Plus's thresholds (tuned on 48 minutes of six films: every cut ffmpeg scores above 0.3 was called). **2**: the `_SceneChangePrev` frame property, for example from `core.misc.SCDetect`. **3**: either. **0**: none. |

A cut that is called but is not one only costs that frame its history. A missed cut lets
DLSS blend the previous shot into the first frames of the next.

### 4.6 System

| Parameter | Default | Values | What it does |
|---|---|---|---|
| `half` | False | True, False | The R'G'B' planes the plugin exchanges are 16-bit floats instead of 32-bit. Same result to half precision (3.828 against 3.829 codes of mean change) and the same speed, but vspipe's memory in 4K, two passes, fell from 11.9 to 7.0 GB. **Recommended when RAM is short**: VapourSynth holds every R'G'B' frame a chunk and its look-ahead need. |
| `dll` | "" | path | `nvngx_dlss.dll`, or its folder. |
| `device` | 0 | index | Which NVIDIA GPU, counting NVIDIA's only. |

---

## 5. The plugin itself: `core.dlss.Denoise`

```
core.dlss.Denoise(clip clip[, clip ref, string preset, int linear, float exposure, int auto_exposure,
                  int pad, int motion, int flow_factor, int grid, int perf, int flow_blur, int scenecut,
                  int preroll, int passes, int chunk, int lookahead, string vectors, int tiles,
                  int debug, string dll, int device])
```

- **`clip`**: RGBS or RGBH, **PQ code values** (R'G'B', BT.2020), constant size. The wrapper
  converts YUV to this and back with zimg; use the plugin directly only for a pipeline of
  your own.
- **`ref`**: the same format, size and length; the motion is measured on it.
- **`debug`**: 1 returns the motion vectors DLSS would be given instead of its picture (RGBS:
  R = x, G = y, in pixels, content at x was at x + v in the previous frame), one pass. RGBS
  input only. For looking at the vectors.
- **Defaults** are those of §4.

**Frame properties written**:

| Property | Meaning |
|---|---|
| `DLSSDenoiseCut` | 1 when the forward pass started its history over on this frame (a cut, a seek, the first frame) |
| `DLSSDenoiseHistory` | with `passes=2`: [forward, backward], the frames of history each pass held for this frame |

## 6. Speed

The Uprising, 3840x2160, RTX 4060, Ryzen 7 5700G:

| Setting | Frames/s | A 2-hour film |
|---|---|---|
| defaults (2 passes, f1, grid 1, slow, tiles) | 0.95 | about 50 h |
| `tiles=0` | 1.38 | about 35 h |
| `passes=1` | 3.1 | about 15 h |
| `passes=1, grid=4, tiles=0` | 8.5 | about 6 h |
| `passes=1, motion=0` (tests only) | 11 | |

- **What it costs per pass**: DLSS itself takes 5 to 6 ms a frame in 4K. Most of the time
  goes to Optical Flow at a vector per pixel and to cleaning the vectors. The two passes
  each measure their own motion.
- **In a StaxRip encode** the encoder runs at the same time. On the test extract (crop
  3840x2076, then 1920x1038, SVT-AV1 `slower`), DLSS Denoiser set the pace at 1.38 frames/s.

## 7. Recipes

- **Maximum quality**, the defaults: `DLSSDenoise(clip)`.
- **Keep some grain**: `strength=0.6` to `0.8`. It is linear and predictable. `limit` works
  differently: grain larger than the limit stays, so it keeps the coarse grain and removes
  the fine.
- **Faster**: `passes=1` first (it loses the start of each shot), then `grid=4`. Both are
  measured above.
- **AV1 film grain synthesis**: a StaxRip job with `--film-grain 8` lets SVT-AV1 estimate
  grain from its input. Fed DLSS Denoiser's output, it estimates its grain from a clean
  picture, so expect less synthesized grain (not measured). On the test extract at CRF 33, the file was 29 % smaller than without denoising
  (23 % smaller than FFT3DGPU sigma 4). Raise `--film-grain`, or lower `strength`, if you
  want grain on screen.

## 8. Messages and what they mean

| Message | What to do |
|---|---|
| `nvngx_dlss.dll not found` | Put it in `dlss\` beside `vsdlss.dll`, or give `dll=`. |
| `the DLL must be named nvngx_dlss.dll` | The NGX runtime loads it by that name; do not rename it. |
| `the driver's NGX runtime (_nvngx.dll) was not found` | The NVIDIA driver is missing or too old. |
| `DLSS Super Resolution needs driver x.y or newer` | Update the driver. |
| `no motion vectors (Optical Flow: ...)` in the log | No `nvofapi64.dll`, or a GPU older than RTX 20. The filter runs with zero motion; see `motion`. |
| `the clip is not PQ` | Only HDR10 sources for now. |
| `the clip must be RGBS or RGBH` | Calling `core.dlss.Denoise` directly: give it PQ R'G'B', or use the wrapper. |
| `the prefilter needs core.bm3dcuda` | Load BM3DCUDA (or DFTTest2) with `core.std.LoadPlugin`. |
| A StaxRip preview slow to show a frame | Each jump runs DLSS over `preroll` frames, and two passes over a chunk and its look-ahead. Normal. |

## 9. Limitations

- **HDR in PQ only.** SDR will come in LDR mode; for 10-bit SDR sources a 10 → 8 bit
  debanding prefilter may be wanted.
- **Moving subjects under heavy grain** are tracked less well than still pictures and pans
  (§4.4).
- **Speed** (§6).
