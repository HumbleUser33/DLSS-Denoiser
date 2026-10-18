# Side-by-side PNGs of source and DLSS Denoiser, for the eye: a crop at 100 %, PQ shown on
# an SDR scale (diffuse white at 203 nits, highlights clipped).
#   vspipe preview.vpy -a clip=<file.mkv> -a at=<frame> [-a x=..] [-a y=..] [-a w=..] [-a h=..]
#         [-a preset=J] [-a name=tag] .
import os
import sys
import vapoursynth as vs

core = vs.core
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "python"))
import dlssdenoise

staxrip = r"E:\StaxRip-v2.52.6-x64"
core.std.LoadPlugin(os.path.join(staxrip, r"Apps\Plugins\Dual\BestSource\BestSource.dll"))
core.std.LoadPlugin(os.path.join(staxrip, r"Apps\Plugins\VS\LibImwri\libimwri.dll"))
core.std.LoadPlugin(os.path.join(root, r"_build\vsdlss.dll"))

args = globals()
src = core.bs.VideoSource(args["clip"])
at = int(args.get("at", 40))
w, h = int(args.get("w", 960)), int(args.get("h", 540))
x = int(args.get("x", (src.width - w) // 2)) & ~1
y = int(args.get("y", (src.height - h) // 2)) & ~1
name = args.get("name", "preview")
extra = {}
for k, v in list(args.items()):
    if isinstance(k, str) and k.startswith("dlss_"):
        v = v.decode() if isinstance(v, bytes) else v
        try:
            v = float(v) if "." in v else int(v)
        except ValueError:
            pass
        extra[k[5:]] = v
preset = args.get("preset", "J")

out = dlssdenoise.DLSSDenoise(src, preset=preset, **extra)


def sdr(c):
    c = core.std.CropAbs(c, w, h, x, y)
    return core.resize.Bicubic(c, format=vs.RGB24, matrix_in_s="2020ncl", transfer_in_s="st2084", transfer_s="709",
                               primaries_in_s="2020", primaries_s="709", nominal_luminance=203, dither_type="error_diffusion")


pair = core.std.StackHorizontal([sdr(src), sdr(out)])
os.makedirs(os.path.join(root, "out", "preview"), exist_ok=True)
pair = core.imwri.Write(pair, "PNG", os.path.join(root, "out", "preview", name + "_%05d.png"), firstnum=0)
pair[at].set_output()
