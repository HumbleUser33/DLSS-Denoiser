# One DLSS Denoiser variant of an extract, cropped as the job crops it, for vspipe to render
# frames of: vspipe variant.vpy -a clip=<file> [-a dlss_<arg>=<v> ...] -s N -e N -c y4m out.y4m
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

args = globals()
clip = args["clip"].decode() if isinstance(args["clip"], bytes) else args["clip"]
src = core.ffms2.Source(clip, cachefile=os.path.join(root, "out", "raw4k", "src.ffindex"))
src = core.std.Crop(src, 0, 0, int(args.get("top", 42)), int(args.get("bottom", 42)))
extra = {}
for k, v in list(args.items()):
    if isinstance(k, str) and k.startswith("dlss_"):
        v = v.decode() if isinstance(v, bytes) else v
        try:
            v = float(v) if "." in v else int(v)
        except ValueError:
            pass
        extra[k[5:]] = v
dlssdenoise.DLSSDenoise(src, **extra).set_output()
