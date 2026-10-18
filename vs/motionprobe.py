# The motion each vector mode gives, region by region, on the face shot, against the motion
# measured between consecutive source frames (bicubic shift search on blurred luma).
#   python motionprobe.py [mode ...]     modes: clean raw global clean_notiles global_tiles, <prefilter>+<mode>
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))
d = os.path.join(root, "out", "raw4k")
src = core.std.Crop(core.ffms2.Source(os.path.join(root, r"out\clips\uprising_1200.mkv"), cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)
regions = {"trees": (2900, 1000, 800, 600), "left bg": (80, 200, 1000, 1500), "face": (1500, 500, 900, 900)}
frames = range(52, 59)
# What the measurement below gives on those frames (bicubic search to 1/16 pixel).
measured = {"trees": [(-7.44, 4.25), (-9.06, 5.94), (-5.25, 10.38), (-4.69, 7.19), (-0.88, 3.31), (1.31, 3.44), (7.38, 1.00)],
            "face": [(-8.06, 5.94), (-10.62, 6.81), (-5.75, 10.19), (-5.88, 9.38), (-2.31, 4.31), (0.50, 3.06), (8.38, 1.00)]}


def luma(c):
    return core.std.ShufflePlanes(core.resize.Point(c, format=vs.YUV420PS), 0, vs.GRAY)


def shift(a, b, box):
    """d with b(x) ~ a(x + d): b is a moved by -d. With a the current frame and b the previous,
    the content at x now was at x + d: the vector DLSS wants."""
    x, y, w, h = box
    la = core.std.BoxBlur(luma(a), hradius=3, vradius=3, hpasses=2, vpasses=2)
    lb = core.std.BoxBlur(luma(b), hradius=3, vradius=3, hpasses=2, vpasses=2)
    best = (0.0, 0.0)
    for step in (4.0, 1.0, 0.25, 0.0625):
        cands = [(best[0] + i * step, best[1] + j * step) for i in range(-2, 3) for j in range(-2, 3)]
        scores = []
        for dx, dy in cands:
            moved = core.resize.Bicubic(la, src_left=dx, src_top=dy)
            diff = core.std.CropAbs(core.std.Expr([moved, lb], "x y - abs"), w, h, x, y)
            scores.append(core.std.PlaneStats(diff).get_frame(0).props["PlaneStatsAverage"])
        best = cands[scores.index(min(scores))]
    return best


args = [a for a in sys.argv[1:] if a != "--nomeasure"]
if "--nomeasure" not in sys.argv:
    print("measured between source frames (n vs n-1), as DLSS vectors: content at x was at x + v")
    for r, box in regions.items():
        vs_ = [shift(src[n:n + 1], src[n - 1:n], box) for n in frames]
        print(f"  {r:8} " + "  ".join(f"{-v[0]:+.2f},{-v[1]:+.2f}" for v in vs_))

rgb = core.resize.Spline36(src, format=vs.RGBS, matrix_in_s="2020ncl")
modes = args or ["clean", "raw", "global"]
def modeargs(m):
    if m == "clean_notiles":
        return dict(vectors="clean", tiles=0)
    if m == "global_tiles":
        return dict(vectors="global", tiles=1)
    return dict(vectors=m)
for mode in modes:
    if "+" in mode:   # <prefilter>+<mode>: the motion searched on a pre-denoised copy
        pre, m = mode.split("+", 1)
        sys.path.insert(0, os.path.join(root, "python"))
        import dlssdenoise
        ref = core.resize.Spline36(dlssdenoise._prefilter(src, pre), format=vs.RGBS, matrix_in_s="2020ncl")
        mv = core.dlss.Denoise(rgb, ref=ref, debug=1, **modeargs(m))
    else:
        mv = core.dlss.Denoise(rgb, debug=1, **modeargs(mode))
    print(f"vectors={mode}")
    for r, (x, y, w, h) in regions.items():
        vals = []
        for n in frames:
            f = core.std.CropAbs(mv[n:n + 1], w, h, x, y)
            mx = core.std.PlaneStats(core.std.ShufflePlanes(f, 0, vs.GRAY)).get_frame(0).props["PlaneStatsAverage"]
            my = core.std.PlaneStats(core.std.ShufflePlanes(f, 1, vs.GRAY)).get_frame(0).props["PlaneStatsAverage"]
            vals.append((mx, my))
        print(f"  {r:8} " + "  ".join(f"{v[0]:+.2f},{v[1]:+.2f}" for v in vals))
        if r in measured:
            ex = [v[0] - m[0] for v, m in zip(vals, measured[r])]
            ey = [v[1] - m[1] for v, m in zip(vals, measured[r])]
            rms = (sum(a * a + b * b for a, b in zip(ex, ey)) / len(ex)) ** 0.5
            print(f"  {'':8} error: mean {sum(ex) / len(ex):+.2f},{sum(ey) / len(ey):+.2f}  rms {rms:.2f}")
    del mv
