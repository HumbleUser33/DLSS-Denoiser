# Raw 4K comparisons, no encode: source, FFT3DGPU (StaxRip's AviSynth chain, rendered to y4m
# by avs2pipemod) and DLSS Denoiser variants, on chosen frames of an extract.
#   python raw4k.py
# Writes out\raw4k\<shot>_<variant>.png (full frame) and _crop.png (100 % crops), on an SDR
# scale (PQ, diffuse white at 203 nits; the dark shot also two stops brighter), and prints:
#   shift: the sub-pixel offset that best aligns the variant with the source (blurred luma)
#   hf:    mean |Y - box3(Y)| over the crop, 10-bit codes: fine detail and grain together
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\VS\LibImwri\libimwri.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")

out_dir = os.path.join(root, "out", "raw4k")
src = core.ffms2.Source(os.path.join(root, r"out\clips\uprising_1200.mkv"), cachefile=os.path.join(out_dir, "src.ffindex"))
src = core.std.Crop(src, 0, 0, 42, 42)

# frame, name, crop (x, y, w, h) in the 3840x2076 picture, brighter view
shots = [
    (56, "face", (1300, 450, 1280, 900), False),
    (112, "dark", (1100, 600, 1400, 1000), True),
]

variants = [
    ("dlss_default", {}),
    ("dlss_1pass", dict(passes=1)),
    ("dlss_nomotion", dict(motion=0)),
    ("dlss_realtime_mv", dict(flow_factor=4, grid=4, perf=1)),
    ("dlss_ldr_pq", dict(linear=0)),
    ("dlss_exposure4", dict(exposure=4.0)),
    ("dlss_auto_exposure", dict(auto_exposure=1)),
    ("dlss_K", dict(preset="K")),
]
only = sys.argv[1:]


def sdr(clip, bright):
    return core.resize.Bicubic(clip, format=vs.RGB24, matrix_in_s="2020ncl", transfer_in_s="st2084", transfer_s="709",
                               primaries_in_s="2020", primaries_s="709", nominal_luminance=50 if bright else 203,
                               dither_type="error_diffusion")


def write(clip, path):
    w = core.imwri.Write(clip, "PNG", path, firstnum=0, overwrite=True)
    w.get_frame(0)


def luma(clip):
    return core.std.ShufflePlanes(core.resize.Point(clip, format=clip.format.replace(sample_type=vs.FLOAT, bits_per_sample=32).id), 0, vs.GRAY)


def shift(a, b):
    """The offset d with b(x) ~ a(x + d), in source pixels: the bicubic shift of b that best
    matches a (mean |difference| of blurred luma, 64-pixel margin left out), coarse to fine."""
    la = core.std.BoxBlur(luma(a), hradius=2, vradius=2, hpasses=2, vpasses=2)
    lb = core.std.BoxBlur(luma(b), hradius=2, vradius=2, hpasses=2, vpasses=2)
    best = (0.0, 0.0)
    for step in (0.5, 0.125, 0.03125):
        cands = [(best[0] + i * step, best[1] + j * step) for i in range(-2, 3) for j in range(-2, 3)]
        scores = []
        for dx, dy in cands:
            moved = core.resize.Bicubic(lb, src_left=-dx, src_top=-dy)
            diff = core.std.CropRel(core.std.Expr([la, moved], "x y - abs"), 64, 64, 64, 64)
            scores.append(core.std.PlaneStats(diff).get_frame(0).props["PlaneStatsAverage"])
        best = cands[scores.index(min(scores))]
    return best

def matchframe(a_clip, f, b):
    """Which source frame around f the clip's frame matches best (mean |difference|)."""
    out = []
    for k in (f - 1, f, f + 1):
        d = core.std.Expr([luma(a_clip[k:k + 1]), luma(b)], "x y - abs")
        out.append((k, core.std.PlaneStats(d).get_frame(0).props["PlaneStatsAverage"] * 1023))
    return out

def hf(clip, box):
    x, y, w, h = box
    l = core.std.CropAbs(luma(clip), w, h, x, y)
    d = core.std.Expr([l, core.std.BoxBlur(l, hradius=1, vradius=1)], "x y - abs")
    return core.std.PlaneStats(d).get_frame(0).props["PlaneStatsAverage"] * 1023


