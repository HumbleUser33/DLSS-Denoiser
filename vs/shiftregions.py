# Where a variant's picture is displaced on the face shot: the offset that best aligns it with
# the source, measured on the out-of-focus background alone and on the face alone.
import os
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
d = r"E:\DLSS Denoise\out\raw4k"
src = core.std.Crop(core.ffms2.Source(r"E:\DLSS Denoise\out\clips\uprising_1200.mkv", cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)
regions = {"background": (80, 200, 1000, 1500), "trees": (2900, 1000, 800, 600), "face": (1500, 500, 900, 900)}


def luma(c):
    return core.std.ShufflePlanes(core.resize.Point(c, format=c.format.replace(sample_type=vs.FLOAT, bits_per_sample=32).id), 0, vs.GRAY)


def shift(a, b, box):
    x, y, w, h = box
    la = core.std.BoxBlur(luma(a), hradius=2, vradius=2, hpasses=2, vpasses=2)
    lb = core.std.BoxBlur(luma(b), hradius=2, vradius=2, hpasses=2, vpasses=2)
    best = (0.0, 0.0)
    for step in (1.0, 0.25, 0.0625, 0.015625):
        cands = [(best[0] + i * step, best[1] + j * step) for i in range(-2, 3) for j in range(-2, 3)]
        scores = []
        for dx, dy in cands:
            moved = core.resize.Bicubic(lb, src_left=-dx, src_top=-dy)
            diff = core.std.CropAbs(core.std.Expr([la, moved], "x y - abs"), w, h, x, y)
            scores.append(core.std.PlaneStats(diff).get_frame(0).props["PlaneStatsAverage"])
        best = cands[scores.index(min(scores))]
    return best


import sys
# The background holds still on this shot: the mean of 48 source frames is a clean picture of
# it, against which a shift is measurable even where it is out of focus. The face moves, so
# its region is measured against frame 56 itself.
half1 = core.std.AverageFrames([src[k:k + 1] for k in range(32, 56)], weights=[1] * 24)
half2 = core.std.AverageFrames([src[k:k + 1] for k in range(56, 80)], weights=[1] * 24)
mean = core.std.Merge(half1, half2, 0.5)
s = src[56:57]
regions_ref = {"background": mean, "trees": s, "face": s}
for n in (sys.argv[1:] or ["fft3dgpu", "dlss_default", "dlss_1pass", "dlss_nomotion", "dlss_raw", "dlss_global", "dlss_bm3d_raw", "dlss_ldr_pq"]):
    f = os.path.join(d, "fft3d_face.y4m" if n == "fft3dgpu" else f"face_{n}.y4m")
    c = core.bs.VideoSource(f)
    c = core.std.SetFrameProps(c[12:13] if n == "fft3dgpu" else c[0:1], _Matrix=9, _Transfer=16, _Primaries=9, _Range=vs.RANGE_LIMITED, _ChromaLocation=2)
    res = {r: shift(regions_ref[r], c, box) for r, box in regions.items()}
    out = "  ".join(f"{r}: {v[0]:+.3f} {v[1]:+.3f}" for r, v in res.items())
    print(f"{n:18} {out}")
