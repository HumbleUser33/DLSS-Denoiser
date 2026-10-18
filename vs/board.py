# A board of 100 % crops from the full-frame PNGs out\raw4k\<shot>_<variant>.png, labelled,
# two to a row, for looking at one region across variants.
#   python board.py <shot> <x> <y> <w> <h> <out-name> <variant> [<variant> ...]
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\VS\LibImwri\libimwri.dll")
d = r"E:\DLSS Denoise\out\raw4k"
shot, x, y, w, h, name = sys.argv[1], *map(int, sys.argv[2:6]), sys.argv[6]
tiles = []
for n in sys.argv[7:]:
    c = core.std.CropAbs(core.imwri.Read(os.path.join(d, f"{shot}_{n}.png")), w, h, x, y)
    tiles.append(core.text.Text(c, n, alignment=7, scale=2))
if len(tiles) % 2:
    tiles.append(core.std.BlankClip(tiles[0]))
board = core.std.StackVertical([core.std.StackHorizontal(tiles[i:i + 2]) for i in range(0, len(tiles), 2)])
out = os.path.join(d, f"board_{name}_%d.png")
core.imwri.Write(board, "PNG", out, firstnum=0, overwrite=True).get_frame(0)
print(out % 0)
