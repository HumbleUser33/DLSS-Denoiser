# The high-frequency energy of the luma, frame by frame: grain, mostly, on a film that holds
# still. Mean |Y - box3(Y)| in 10-bit code values, for the source and DLSS Denoiser variants.
#   python grainlevel.py <clip.mkv> <frames> name:opt=v,opt=v [name:...]
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

src = core.bs.VideoSource(sys.argv[1])
frames = int(sys.argv[2])
variants = [("source", None)]
for spec in sys.argv[3:]:
    name, _, opts = spec.partition(":")
    kw = {}
    for a in filter(None, opts.split(",")):
        k, v = a.split("=", 1)
        kw[k] = float(v) if "." in v else int(v) if v.lstrip("-").isdigit() else v
    variants.append((name, kw))


def hf(clip):
    y = core.std.ShufflePlanes(clip, 0, vs.GRAY)
    blur = core.std.BoxBlur(y, hradius=1, vradius=1)
    return core.std.PlaneStats(core.std.Expr([y, blur], "x y - abs"))


rows = []
for name, kw in variants:
    clip = src if kw is None else dlssdenoise.DLSSDenoise(src, **kw)
    s = hf(clip)
    rows.append((name, [s.get_frame(n).props["PlaneStatsAverage"] * 1023 for n in range(frames)]))

print("frame " + " ".join(f"{name:>10}" for name, _ in rows))
for n in range(frames):
    print(f"{n:5d} " + " ".join(f"{vals[n]:10.3f}" for _, vals in rows))
print("mean  " + " ".join(f"{sum(vals) / frames:10.3f}" for _, vals in rows))
