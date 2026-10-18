# The filter's speed on an extract: vspipe speed.vpy -a clip=<file> [-a frames=N] [-a dlss_<arg>=<v> ...] -p .
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

args = globals()
src = core.bs.VideoSource(args["clip"])
frames = int(args.get("frames", 0))
if frames:
    src = src[:frames]
extra = {}
for k, v in list(args.items()):
    if isinstance(k, str) and k.startswith("dlss_"):
        v = v.decode() if isinstance(v, bytes) else v
        try:
            v = float(v) if "." in v else int(v)
        except ValueError:
            pass
        extra[k[5:]] = v
mode = args.get("mode", "dlss")
if isinstance(mode, bytes):
    mode = mode.decode()
if mode == "source":
    out = src
elif mode == "convert":
    rgb = core.resize.Spline36(src, format=vs.RGBS, matrix_in_s="2020ncl")
    out = core.resize.Spline36(rgb, format=src.format.id, matrix_s="2020ncl", dither_type="none")
else:
    out = dlssdenoise.DLSSDenoise(src, **extra)
out.set_output()
