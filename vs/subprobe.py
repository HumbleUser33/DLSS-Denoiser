# The motion each vector mode gives on smaller boxes of the face shot, against the motion
# measured between consecutive source frames, cached in out\raw4k\measured.json.
#   python subprobe.py [mode ...]     modes as motionprobe.py, plus <prefilter>+<mode>
import json
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))
d = os.path.join(root, "out", "raw4k")
src = core.std.Crop(core.ffms2.Source(os.path.join(root, r"out\clips\uprising_1200.mkv"), cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)
# The trees in four, and the face in four.
boxes = {}
for name, (x0, y0, w, h) in {"trees": (2900, 1000, 800, 600), "face": (1500, 500, 900, 900)}.items():
    for j in range(2):
        for i in range(2):
            boxes[f"{name}{j}{i}"] = (x0 + i * w // 2, y0 + j * h // 2, w // 2, h // 2)
frames = list(range(50, 59))


def luma(c):
    return core.std.ShufflePlanes(core.resize.Point(c, format=vs.YUV420PS), 0, vs.GRAY)


def shift(a, b, box):
    """The vector DLSS wants: the content at x in a (current) was at x + v in b (previous)."""
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
    return (-best[0], -best[1])


cache = os.path.join(d, "measured.json")
measured = json.load(open(cache)) if os.path.exists(cache) else {}
changed = False
for name, box in boxes.items():
    for n in frames:
        key = f"{name}:{n}"
        if key not in measured:
            measured[key] = shift(src[n:n + 1], src[n - 1:n], box)
            changed = True
if changed:
    json.dump(measured, open(cache, "w"), indent=1)

if sys.argv[1:] == ["measure"]:
    sys.exit(0)
rgb = core.resize.Spline36(src, format=vs.RGBS, matrix_in_s="2020ncl")


def modeargs(m):
    if m == "clean_notiles":
        return dict(vectors="clean", tiles=0)
    if m == "global_tiles":
        return dict(vectors="global", tiles=1)
    return dict(vectors=m)


for mode in sys.argv[1:] or ["clean"]:
    if "+" in mode:
        pre, m = mode.split("+", 1)
        sys.path.insert(0, os.path.join(root, "python"))
        import dlssdenoise
        ref = core.resize.Spline36(dlssdenoise._prefilter(src, pre), format=vs.RGBS, matrix_in_s="2020ncl")
        mv = core.dlss.Denoise(rgb, ref=ref, debug=1, **modeargs(m))
    else:
        mv = core.dlss.Denoise(rgb, debug=1, **modeargs(mode))
    print(f"vectors={mode}: error against the measured motion, mean x,y and rms, per box (frames {frames[0]}-{frames[-1]})")
    total = []
    for name, (x, y, w, h) in boxes.items():
        ex, ey = [], []
        for n in frames:
            f = core.std.CropAbs(mv[n:n + 1], w, h, x, y)
            mx = core.std.PlaneStats(core.std.ShufflePlanes(f, 0, vs.GRAY)).get_frame(0).props["PlaneStatsAverage"]
            my = core.std.PlaneStats(core.std.ShufflePlanes(f, 1, vs.GRAY)).get_frame(0).props["PlaneStatsAverage"]
            m = measured[f"{name}:{n}"]
            ex.append(mx - m[0])
            ey.append(my - m[1])
        rms = (sum(a * a + b * b for a, b in zip(ex, ey)) / len(ex)) ** 0.5
        total.append(rms)
        print(f"  {name:8} mean {sum(ex) / len(ex):+.2f},{sum(ey) / len(ey):+.2f}  rms {rms:.2f}")
    t = [r for r, n in zip(total, boxes) if n.startswith("trees")]
    print(f"  trees rms mean {sum(t) / len(t):.2f}   face rms mean {sum(total[4:]) / 4:.2f}")
    del mv
