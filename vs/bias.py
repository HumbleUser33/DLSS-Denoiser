# The mean change DLSS Denoiser makes to a clip, plane by plane, in 10-bit code values:
# what it takes out of the grain should leave the picture's level where it was.
#   python bias.py <clip.mkv> [frames] [option=value ...]   (options go to DLSSDenoise)
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

staxrip = r"E:\StaxRip-v2.52.6-x64"
core.std.LoadPlugin(os.path.join(staxrip, r"Apps\Plugins\Dual\BestSource\BestSource.dll"))
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

path = sys.argv[1]
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 48
opts = {}
for a in sys.argv[3:]:
    k, v = a.split("=", 1)
    try:
        v = float(v) if "." in v else int(v)
    except ValueError:
        pass
    opts[k] = v

src = core.bs.VideoSource(path)[:frames]
out = dlssdenoise.DLSSDenoise(src, **opts)
stats = [(core.std.PlaneStats(src, plane=p), core.std.PlaneStats(out, plane=p)) for p in range(3)]
d = [[], [], []]
for n in range(frames):
    for p in range(3):
        s, o = stats[p]
        d[p].append((o.get_frame(n).props["PlaneStatsAverage"] - s.get_frame(n).props["PlaneStatsAverage"]) * 1023.0)
for p in range(3):
    tail = d[p][frames // 2:]
    print(f"plane {'YUV'[p]}: mean change {sum(tail) / len(tail):+.3f} codes over the second half "
          f"(first frame {d[p][0]:+.3f}, last {d[p][-1]:+.3f})")
