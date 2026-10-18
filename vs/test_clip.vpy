# A test of DLSS Denoiser on an extract, with the plugin and the wrapper from the build.
#   vspipe test_clip.vpy -a clip=<file.mkv> [-a strength=1.0] [-a preset=J] [-a frames=N] - -c y4m | ...
#   vspipe test_clip.vpy -a clip=<file.mkv> -p .            (speed, no output)
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

args = globals()
src = core.bs.VideoSource(args["clip"])
frames = int(args.get("frames", 0))
if frames:
    src = src[:frames]
strength = float(args.get("strength", 1.0))
preset = args.get("preset", "J")
mode = args.get("mode", "out")

out = dlssdenoise.DLSSDenoise(src, strength=strength, preset=preset)
if mode == "side":
    out = core.std.StackHorizontal([core.resize.Point(src, width=src.width // 2, height=src.height // 2),
                                    core.resize.Point(out, width=src.width // 2, height=src.height // 2)])
out.set_output()
