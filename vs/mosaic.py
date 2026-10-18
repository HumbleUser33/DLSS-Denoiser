# Mosaics of 100 % crops from out\raw4k\<shot>_<variant>_crop.png, labelled, three to a row.
#   python mosaic.py <shot> <x> <y> <w> <h> <variant> [<variant> ...]
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\VS\LibImwri\libimwri.dll")
d = r"E:\DLSS Denoise\out\raw4k"
shot, x, y, w, h = sys.argv[1], *map(int, sys.argv[2:6])
names = sys.argv[6:]
tiles = []
for n in names:
    c = core.imwri.Read(os.path.join(d, f"{shot}_{n}_crop.png"))
    c = core.std.CropAbs(c, w, h, x, y)
    tiles.append(core.text.Text(c, n, alignment=7, scale=2))
while len(tiles) % 3:
    tiles.append(core.std.BlankClip(tiles[0]))
rows = [core.std.StackHorizontal(tiles[i:i + 3]) for i in range(0, len(tiles), 3)]
board = core.std.StackVertical(rows)
out = os.path.join(d, f"mosaic_{shot}_{len(names)}_{names[-1]}.png")
core.imwri.Write(board, "PNG", out.replace(".png", "_%d.png"), firstnum=0, overwrite=True).get_frame(0)
print(out.replace(".png", "_0.png"))
