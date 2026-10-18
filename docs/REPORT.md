# DLSS Denoiserr — measurement report

All measurements were made on an RTX 4060 8 GB (driver 616.56) with `nvngx_dlss.dll`
310.9.16, by `_build\dlssdn_bench.exe` (source in `bench\bench.cpp`) and the scripts in `vs\`.
Their raw output went to `out\`, which is not part of the repository.

**Synthetic method.** 4K film frames are converted to PQ BT.2020, diffuse white at 203 nits.
Every frame gets fresh film-like grain: correlated over about one pixel, partly common to the
three dye layers. Two levels: light (3.6 10-bit codes rms) and heavy (10.2 codes). Measured
against the clean picture, in PQ values:
- RGB PSNR;
- grain left (rms of the luma error on flat areas, ×1000);
- detail kept (1.00 = the clean picture's);
- flicker (frame-to-frame change along the motion, ×1000).

## 1. LDR mode's 8 bits

NVIDIA's programming guide (310.6.0, §3.1.2) states that LDR mode "quantizes the data to 8
bits". Games do not go through it: DLSS runs before tone mapping, with `IsHDR`, on linear
light.

What comes out, on a smooth, still PQ ramp:

| Range | `ldr_pq` max error | `hdr_lin_auto` max error |
|---|---|---|
| dark (0.05–14 nits) | 0.37 code | 0.23 code |
| mid (14–240 nits) | 0.74 code | 0.22 code |
| bright (240–1500 nits) | 0.75 code | 0.25 code |

No 8-bit step shows in the output: one would be about 60 pixels wide and 2 codes of error.
Linear light with `IsHDR` is the most faithful over the whole range.

## 2. Domain and preset

Exact motion, 4 reference frames:

| | Mean PSNR (4 cases) | Mean flicker | Detail |
|---|---|---|---|
| **`hdr_lin` J** (default) | 56.06 dB | **0.28** | 0.947 |
| `hdr_lin` K | 55.97 dB | 0.32 | 0.943 |
| `ldr_pq` J (MPCVR-Plus) | 56.35 dB | 0.37 | 0.951 |
| `ldr_pq` K | 55.99 dB | 0.43 | 0.951 |
| L, M (DLSS 4.5) | 5 to 8 dB lower on average | 2 to 15 times more | |

- **L and M are out**: they leave far more grain.
- **J and K are even** in PSNR.
- **`hdr_lin` J flickers least**, which matters most to an encoder.
- **Exposure**: the fixed exposure texture at 1.0 (1.0 = 100 nits) keeps more detail than
  auto exposure (0.94 against 0.92).

**Mean level on a real extract** (The Uprising, 48 frames, `vs\bias.py`):

| Plane | `hdr_lin` J | `ldr_pq` J |
|---|---|---|
| Luma | −0.33 10-bit code | −0.74 10-bit code |
| Chroma | under 0.02 code | under 0.02 code |

Negligible either way.

## 3. Motion vectors

Heavy and light grain, 2 reference frames. PSNR deficit against the exact motion:

| Vectors | Still | Pan 2 px/frame | Pan 0.7 px/frame | Moving subject |
|---|---|---|---|---|
| MPCVR-Plus real time (flow ÷4, grid 4) | −0.4 | **−4.1 to −9.4** | **−4.1 to −6.0** | −2.4 to −3.8 |
| f2g2 slow | 0.0 to −0.2 | −0.2 to −0.8 | −1.2 to −2.1 | −2.3 to −4.5 |
| f1g4 slow | −0.2 to −0.5 | −0.3 to −0.4 | −0.3 | −2.4 to −4.5 |
| f1g2 slow | 0.0 to −0.3 | 0.0 to −0.2 | −0.1 | −2.6 to −4.5 |
| **f1g1 slow (default)** | **0.0 to −0.3** | **0.0 to −0.2** | **0.0 to −0.1** | −3.0 to −4.0 |

- **Full resolution** (no divider, a vector per pixel): still pictures and pans reach the
  exact motion. The global motion is right to 0.005 pixel.
- **A subject moving on its own** is the weak point of every setting under grain. Optical
  Flow's raw per-pixel flow is wrong there (52 px rms over the picture on average, because of
  the grainy flat areas). Given a clean clip to search on, the error on the subject falls to
  0.29 pixel, nearly the exact motion.
- **Bootstrapping** (a first DLSS pass as the search clip) helps the subject but harms still
  pictures. Not kept as it is.

## 4. Convergence and two passes

**Convergence**: after a reset, the grain keeps falling well past 24 frames. Under heavy
grain:

| Frame after the reset | Grain left |
|---|---|
| 0 | 9.1 |
| 8 | 3.1 |
| 16 | 2.3 |
| 24 | 1.95 |
| 36 | 1.67 |

So every cut brings the grain back for several dozen frames: a pumping effect.

**Two passes**: DLSS forward then backward in time, exact motion, the mean of the two. Over
the 8 cases measured (J and K, still and pan, light and heavy grain), on the middle third of
48 frames:
- **22 to 25 % less grain left**;
- **+1.5 to +2.3 dB**;
- 21 to 24 % less flicker;
- detail unchanged.

It also cures the pumping after cuts, since the backward pass has its whole history at the
start of a shot.

**As built** (`passes=2`, the default): the backward lane runs the film backward in chunks of
24 frames, starting 24 frames further on. Each frame is the mean of the two lanes weighted by
the history each held (`CEngine::Weight`: history + 1, capped at 30, from the convergence
curve).

A pan cut in two, heavy grain, 48 frames (`--twopass`, `--twopass-mv`):

| Motion | One pass | Two passes, plain mean | **Two passes, weighted** |
|---|---|---|---|
| exact | 3.27 | 2.58 | **1.98** |
| measured | 3.26 | 2.57 | **1.96** |

- With one pass, the first frame after the cut climbs back to 9.1.
- With two weighted passes, the grain stays between 1.91 and 2.08 over the whole sequence.
- The GPU cut detection sees the cut in both directions.

Real extract (`vs\grainlevel.py`, The Uprising, 48 frames, high-frequency luma energy in
10-bit codes):

| | Source | 1 pass | 2 passes |
|---|---|---|---|
| Mean | 2.59 | 1.16 | **0.95** |
| Frames 0 to 5 | 2.6 to 2.8 | 2.72 → 1.29 | **1.07 → 0.93** |

No step shows at chunk boundaries.

## 5. Cost

| | |
|---|---|
| DLSS DLAA alone, 3840x2160 | 5.6 to 6.2 ms (J or K) |
| Full filter, 3840x2160, f1g1 slow, one pass | 3.7 frames/s |
| Full filter, two passes | 1.27 frames/s, about 38 h for a 2-hour film |
| Full filter, f1g4, one pass | 8.5 frames/s |
| Full filter, no vectors | 11 frames/s |
| Decoding alone | 59 frames/s |
| YUV ↔ RGB conversions alone | 27.5 frames/s |

Most of the time goes to Optical Flow at grid 1 and to the per-pixel cleaning. The processing
is fully sequential and synchronous: no CPU/GPU overlap, and upload and readback are float32.
Ways to cut it without touching quality: reuse the backward flow Optical Flow already
computes, overlap CPU and GPU, upload RGBH, lighten the grid-1 cleaning pass.

## 6. In a StaxRip encode

**Install.** `vsdlss.dll` and `dlssdenoise.py` in `StaxRip\Settings\Plugins\VapourSynth\`, and
`nvngx_dlss.dll` in a `dlss\` subfolder so that it is not loaded as a plugin.

**Test.** A real StaxRip job on The Uprising, reproduced on a 15 s extract: 381 frames, 4
shots according to the Dolby Vision RPU. StaxRip's own executables and options:
- `vspipe … --container y4m | SvtAv1EncApp-Essential`;
- CRF 33, `slower`, `--film-grain 8`, HDR10, `--dolby-vision-rpu` (the extract's RPU, from
  dovi_tool);
- then mkvmerge.

| Chain (crop 42/42, then Spline36 1920x1038) | Size | Bitrate | Speed |
|---|---|---|---|
| no denoising | 2.14 MB | 1067 kb/s | 7.2 frames/s |
| FFT3DGPU sigma 4 (the AviSynth chain in use) | 1.98 MB (−7 %) | 991 kb/s | 7.8 frames/s |
| **DLSS Denoiserr, defaults (J, 2 passes, f1g1)** | **1.52 MB (−29 %)** | **759 kb/s** | 1.38 frames/s |

**Checks on the three files**: AV1 10-bit, BT.2020 PQ, 381 frames, 16.016 s, mastering
display, MaxCLL 212 / MaxFALL 60, Dolby Vision metadata.

**By eye**, on the decoded files, AV1 grain synthesis included: the face's detail is kept, and
no ghost or artefact shows, after the cuts included.

## 7. Raw 4K against FFT3DGPU, offline motion (2026-10-18)

No encode: two frames of The Uprising processed in 4K (crop 42/42), against FFT3DGPU as
StaxRip sets it (sigma 4, sharpen 0.4). Frame 56: a close-up, the camera follows the face (5 to
11 pixels a frame), out-of-focus trees behind. Frame 112: a dark shot, a figure seen from
behind; the second frame of its shot (a cut at 111), so the forward pass has almost no
history. Scripts: `vs\raw4k.py`, `vs\shiftregions.py`, `vs\fidelity.py`, `vs\bands.py`,
`vs\shadows.py`, boards `vs\board.py`.

**Why the background moved.** The cleaning taken from MPCVR-Plus draws every untrusted block
to the picture's global motion. Here that motion is the face's. The trees move 1 to 2 pixels
a frame differently (parallax), and DLSS accumulated their history along the face's motion.
**The fix (`tiles`, on by default)**: the global motion is refined per tile of 64 pixels on
the blurred frames. A tile with no texture above the grain is drawn back to the global motion,
then each tile takes the median of the 5×5 around it.

| | Trees: shift (px) | All areas, against a clean reference: mean shift (px) | Coat folds (source 13.96) |
|---|---|---|---|
| FFT3DGPU | 0.08 / 0.13 | 0.38 | 13.90 |
| DLSS before (`linear=1`) | ≥ 2.66 / ≥ 2.66 | 2.60 | 10.68 |
| **DLSS with tiles, `linear=1`** | **0.16 / 0.63** | **1.02** | 11.81 |
| DLSS with tiles, `linear=0` | 0.50 / 0.73 | 1.14 | **13.63** |

The clean reference is the mean of 13 frames of the shot, each aligned on frame 56 area by
area. The source itself is 0.24 pixel from it: the floor of the measure.

**Closeness to that reference** (error left as a fraction of the source's; 1 = no better than
the source, 0 = the clean picture), bands fine / mid / coarse / very coarse: FFT3DGPU 0.36 /
0.62 / 0.78 / 0.86; DLSS with tiles 0.27 / 0.32 / 0.40 / 0.62. DLSS is closer to the clean
picture in every band, the face included (cheek: 0.28 / 0.36 / 0.53 / 0.70 against 0.40 /
0.69 / 0.90 / 0.99). Limit: the neighbouring frames have more motion blur than frame 56, so the
reference is a little softer than it and favours a smooth result in the fine bands. On the
dark shot the alignment fails (changing light, fast motion): no figure there.

**Perceived sharpness.** FFT3DGPU keeps 75 % of the face's mid band, DLSS 55 %. The gap is the
coarse grain FFT3DGPU leaves (the reference shows it is not detail) and its sharpening, which
DLSS does not do.

**Dark shot.** With `linear=1` DLSS flattens the folds (11.81). With `linear=0` (PQ in LDR
mode) it keeps them (13.63) but leaves more fine grain (fine band 0.67 against FFT3DGPU's
0.28), mostly because the forward pass starts at the cut.

**Synthetic bench** (tiles against no tiles):
- still pictures and pans: −0.15 to +0.1 dB;
- under heavy grain: 30 % more flicker (0.80 against 0.61);
- a moving subject: +0.7 dB under heavy grain, −1 dB under light grain, its tiles spilling onto
  the background around it.

Cost: about 0.1 s a frame per pass in 4K: two passes run at 0.95 frames/s with tiles, 1.38
without (240 frames of The Uprising).
