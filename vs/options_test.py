# Every option of DLSSDenoise on a few frames: that it runs, and what it changes.
#   python options_test.py <clip.mkv> [frames]
# Mean |output - source| in 10-bit code values, luma and chroma, over the frames.
import os
import sys
import time
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

src = core.ffms2.Source(sys.argv[1], cachefile=os.path.join(root, "out", "options.ffindex"))
src = core.std.Crop(src, 0, 0, 42, 42)
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 8
src = src[:frames + 24]

variants = [
    ("defaults, passes=1", dict(passes=1)),
    ("strength=0", dict(strength=0.0)),
    ("strength=0.5", dict(passes=1, strength=0.5)),
    ("limit=3", dict(passes=1, limit=3)),
    ("chroma=source", dict(passes=1, chroma="source")),
    ("half=True", dict(passes=1, half=True)),
    ("preset=K", dict(passes=1, preset="K")),
    ("linear=0", dict(passes=1, linear=0)),
    ("exposure=2.0", dict(passes=1, exposure=2.0)),
    ("auto_exposure=1", dict(passes=1, auto_exposure=1)),
    ("motion=0", dict(passes=1, motion=0)),
    ("grid=4 perf=1", dict(passes=1, grid=4, perf=1)),
    ("flow_factor=2", dict(passes=1, flow_factor=2, grid=2)),
    ("flow_blur=2", dict(passes=1, flow_blur=2)),
    ("pad=0", dict(passes=1, pad=0)),
    ("scenecut=0", dict(passes=1, scenecut=0)),
    ("prefilter=bm3d", dict(passes=1, prefilter="bm3d")),
    ("prefilter=dfttest", dict(passes=1, prefilter="dfttest")),
    ("passes=2 chunk=8 lookahead=8", dict(passes=2, chunk=8, lookahead=8)),
]


def change(a, b, plane):
    d = core.std.Expr([core.std.ShufflePlanes(a, plane, vs.GRAY), core.std.ShufflePlanes(b, plane, vs.GRAY)], "x y - abs")
    s = core.std.PlaneStats(d)
    return sum(s.get_frame(n).props["PlaneStatsAverage"] for n in range(frames)) / frames * 1023


print(f"{'variant':32} {'Y':>8} {'U':>8} {'V':>8} {'s':>6}")
for name, kw in variants:
    start = time.time()
    try:
        out = dlssdenoise.DLSSDenoise(src, **kw)
        y, u, v = (change(src, out, p) for p in range(3))
        print(f"{name:32} {y:8.3f} {u:8.3f} {v:8.3f} {time.time() - start:6.1f}")
    except vs.Error as e:
        print(f"{name:32} ERROR {e}")
