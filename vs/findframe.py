# Which frame of an extract a screenshot shows: the normalized correlation of their luma at a
# small size, robust to the screenshot's tone mapping.
#   python findframe.py <clip.mkv> <shot.png> [<shot.png> ...]
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\VS\LibImwri\libimwri.dll")
W, H = 192, 104


def small(clip):
    g = core.resize.Bicubic(clip, W, H, format=vs.GRAYS, matrix_in_s="2020ncl" if clip.format.color_family == vs.YUV else None)
    return g


def values(frame):
    m = memoryview(frame[0])
    return [m[y, x] for y in range(H) for x in range(W)]


def normalize(v):
    n = len(v)
    mean = sum(v) / n
    d = [x - mean for x in v]
    s = (sum(x * x for x in d) / n) ** 0.5 or 1.0
    return [x / s for x in d]


src = core.ffms2.Source(sys.argv[1], cachefile=os.path.join(os.path.dirname(sys.argv[1]), "find.ffindex"))
src = core.std.Crop(src, 0, 0, 42, 42)
s = small(src)
frames = [normalize(values(s.get_frame(n))) for n in range(src.num_frames)]
for shot in sys.argv[2:]:
    img = core.imwri.Read(shot)
    img = core.resize.Bicubic(img, W, H, format=vs.GRAYS, matrix_s="709")
    v = normalize(values(img.get_frame(0)))
    scores = [sum(a * b for a, b in zip(v, f)) / len(v) for f in frames]
    best = sorted(range(len(scores)), key=lambda n: -scores[n])[:5]
    print(os.path.basename(shot), "best frames:", ", ".join(f"{n} ({scores[n]:.4f})" for n in best))
