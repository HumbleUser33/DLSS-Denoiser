# Detail left by each variant, band by band, as a fraction of the source's: the RMS of the
# luma's difference of box blurs, per region. The finest band is mostly grain; the middle and
# coarse bands are mostly detail, where a denoiser should keep what the source has.
#   python bands.py [variant ...]
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
d = r"E:\DLSS Denoise\out\raw4k"
src = core.std.Crop(core.ffms2.Source(r"E:\DLSS Denoise\out\clips\uprising_1200.mkv", cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)
shots = {"face": (56, {"face": (1500, 500, 900, 900), "trees": (2900, 1000, 800, 600)}),
         "dark": (112, {"coat": (1350, 1120, 620, 440), "centre": (1200, 700, 1400, 900)})}
bands = [(0, 1), (1, 3), (3, 8)]   # box radii: [fine, middle, coarse]


def luma(c):
    return core.std.ShufflePlanes(core.resize.Point(c, format=vs.YUV420PS), 0, vs.GRAY)


def blur(c, r):
    return c if r == 0 else core.std.BoxBlur(c, hradius=r, vradius=r, hpasses=2, vpasses=2)


def energies(c, box):
    x, y, w, h = box
    l = luma(c)
    out = []
    for a, b in bands:
        diff = core.std.Expr([blur(l, a), blur(l, b)], "x y - dup *")
        out.append(core.std.PlaneStats(core.std.CropAbs(diff, w, h, x, y)).get_frame(0).props["PlaneStatsAverage"] ** 0.5)
    return out


def load(shot, n):
    if n == "source":
        return src[shots[shot][0]:shots[shot][0] + 1]
    f = os.path.join(d, f"fft3d_{shot}.y4m" if n == "fft3dgpu" else f"{shot}_{n}.y4m")
    c = core.bs.VideoSource(f)
    c = c[12:13] if n == "fft3dgpu" else c[0:1]
    return core.std.SetFrameProps(c, _Matrix=9, _Transfer=16, _Primaries=9, _Range=vs.RANGE_LIMITED, _ChromaLocation=2)


names = ["fft3dgpu"] + (sys.argv[1:] or ["dlss_default", "dlss_tiles", "dlss_tiles_ldr"])
for shot, (_, regions) in shots.items():
    ref = {r: energies(load(shot, "source"), box) for r, box in regions.items()}
    print(f"== {shot}: fraction of the source's RMS per band (fine r0-1 / middle r1-3 / coarse r3-8)")
    for n in names:
        c = load(shot, n)
        parts = []
        for r, box in regions.items():
            e = energies(c, box)
            parts.append(f"{r}: " + " ".join(f"{v / s:5.2f}" for v, s in zip(e, ref[r])))
        print(f"  {n:20} " + "   ".join(parts))
