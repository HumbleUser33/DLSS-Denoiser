# How close each variant comes to the clean picture, box by box: the reference is the mean of
# 13 source frames of the shot around the frame, each aligned to it by its own shift search
# over the box, started from its neighbour's (so the grain falls to about a quarter and the
# picture stays, where the box moves as one). Reported per band of
# box blurs, as the RMS of (variant - reference) relative to the source's own distance to it:
# 1.0 is no better than the untouched source, 0 is the clean picture.
#   python fidelity.py [variant ...]
import json
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
d = r"E:\DLSS Denoise\out\raw4k"
src = core.std.Crop(core.ffms2.Source(r"E:\DLSS Denoise\out\clips\uprising_1200.mkv", cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)
MARGIN = 96
shots = {
    "face": (56, 50, 62, {"trees_a": (2900, 1000, 400, 300), "trees_b": (3300, 1000, 400, 300),
                  "trees_c": (2900, 1300, 400, 300), "trees_d": (3300, 1300, 400, 300),
                  "field": (100, 1600, 1200, 350), "cheek": (1500, 900, 400, 400), "beard": (1600, 1300, 500, 300),
                  "cloth": (2000, 1700, 600, 250)}),
    "dark": (112, 111, 123, {"coat": (1350, 1120, 620, 440), "centre_a": (1200, 700, 700, 450), "centre_b": (1900, 700, 700, 450),
                   "centre_c": (1200, 1150, 700, 450), "centre_d": (1900, 1150, 700, 450)}),
}
bands = [(0, 1), (1, 3), (3, 8), (8, 24)]


def luma(c):
    return core.std.ShufflePlanes(core.resize.Point(c, format=vs.YUV420PS), 0, vs.GRAY)


def padded(c, box):
    x, y, w, h = box
    return core.std.CropAbs(c, w + 2 * MARGIN, h + 2 * MARGIN, x - MARGIN, y - MARGIN)


def inner(c, box):
    return core.std.CropAbs(c, box[2], box[3], MARGIN, MARGIN)


def shift(a, b, box, guess):
    """(dx, dy) that moves b onto a over the box: b shifted by src_left=dx, src_top=dy."""
    # Band-passed: the grain blurred away, and the light, which may change along the shot.
    def bandpass(c):
        p = padded(luma(c), box)
        return core.std.Expr([core.std.BoxBlur(p, hradius=3, vradius=3, hpasses=2, vpasses=2),
                              core.std.BoxBlur(p, hradius=16, vradius=16, hpasses=2, vpasses=2)], "x y -")
    la, lb = bandpass(a), bandpass(b)
    best = guess
    for step in (8.0, 2.0, 0.5, 0.125, 0.03125):
        cands = [(best[0] + i * step, best[1] + j * step) for i in range(-2, 3) for j in range(-2, 3)]
        cands = [c for c in cands if abs(c[0]) < MARGIN - 4 and abs(c[1]) < MARGIN - 4]
        scores = []
        for dx, dy in cands:
            moved = core.resize.Bicubic(lb, src_left=dx, src_top=dy)
            diff = inner(core.std.Expr([moved, la], "x y - abs"), box)
            scores.append(core.std.PlaneStats(diff).get_frame(0).props["PlaneStatsAverage"])
        if scores:
            best = cands[scores.index(min(scores))]
    return best


cache_path = os.path.join(d, "fidelity_shifts3.json")
cache = json.load(open(cache_path)) if os.path.exists(cache_path) else {}
refs = {}
for shot, (n0, first, last, boxes) in shots.items():
    for name, box in boxes.items():
        found = {n0: (0.0, 0.0)}
        # Outwards from the frame, each search started where the one before ended, plus the
        # step between them again.
        order = [k for k in range(n0 + 1, last + 1)] + [k for k in range(n0 - 1, first - 1, -1)]
        for k in order:
            step = 1 if k > n0 else -1
            key = f"{shot}:{name}:{k}"
            if key in cache:
                found[k] = tuple(cache[key])
                continue
            near = found[k - step]
            before = found.get(k - 2 * step, near)
            guess = (2 * near[0] - before[0], 2 * near[1] - before[1])
            found[k] = shift(src[n0:n0 + 1], src[k:k + 1], box, guess)
            cache[key] = found[k]
            json.dump(cache, open(cache_path, "w"), indent=1)
        planes = [core.resize.Bicubic(padded(luma(src[k:k + 1]), box), src_left=found[k][0], src_top=found[k][1])
                  for k in range(first, last + 1)]
        refs[(shot, name)] = core.std.AverageFrames(planes, weights=[1] * len(planes))


def load(shot, n):
    n0 = shots[shot][0]
    if n == "source":
        return src[n0:n0 + 1]
    f = os.path.join(d, f"fft3d_{shot}.y4m" if n == "fft3dgpu" else f"{shot}_{n}.y4m")
    if not os.path.exists(f):
        return None
    c = core.bs.VideoSource(f)
    c = c[12:13] if n == "fft3dgpu" else c[0:1]
    return core.std.SetFrameProps(c, _Matrix=9, _Transfer=16, _Primaries=9, _Range=vs.RANGE_LIMITED, _ChromaLocation=2)


def blur(c, r):
    return c if r == 0 else core.std.BoxBlur(c, hradius=r, vradius=r, hpasses=2, vpasses=2)


def band_errors(c, ref, box):
    e = core.std.Expr([padded(luma(c), box), ref], "x y -")
    out = []
    for a, b in bands:
        diff = core.std.Expr([blur(e, a), blur(e, b)], "x y - dup *")
        out.append(core.std.PlaneStats(inner(diff, box)).get_frame(0).props["PlaneStatsAverage"] ** 0.5)
    return out


names = ["fft3dgpu"] + (sys.argv[1:] or ["dlss_default", "dlss_tiles_ldr"])
for shot, (n0, first, last, boxes) in shots.items():
    print(f"== {shot}, frame {n0}: error to the mean of frames {first}-{last} aligned, relative to the source's, "
          f"bands r0-1 / r1-3 / r3-8 / r8-24, then the mean of the four")
    base = {name: band_errors(load(shot, "source"), refs[(shot, name)], box) for name, box in boxes.items()}
    for n in names:
        c = load(shot, n)
        if c is None:
            continue
        line, total = [], []
        for name, box in boxes.items():
            e = band_errors(c, refs[(shot, name)], box)
            rel = [v / max(s, 1e-9) for v, s in zip(e, base[name])]
            total.append(rel)
            line.append(f"{name} " + " ".join(f"{v:4.2f}" for v in rel))
        mean = [sum(t[i] for t in total) / len(total) for i in range(len(bands))]
        print(f"  {n:18} mean " + " ".join(f"{v:4.2f}" for v in mean) + "   | " + "  ".join(line))
