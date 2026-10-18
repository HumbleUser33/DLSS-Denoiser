# What each variant does to the shadows of the dark shot, in 10-bit code values:
#   mean:  mean luma of a region of the coat
#   folds: deviation of the luma blurred over ~8 pixels in that region: the folds' contrast,
#          without the grain
#   shadow shift: mean change of luma against the source where the source is below code 200
#          (about 1.2 nits), the whole picture
import os
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\ffms2\ffms2.dll")
core.std.LoadPlugin(r"E:\StaxRip-v2.52.6-x64\Apps\Plugins\Dual\BestSource\BestSource.dll")
d = r"E:\DLSS Denoise\out\raw4k"
src = core.std.Crop(core.ffms2.Source(r"E:\DLSS Denoise\out\clips\uprising_1200.mkv", cachefile=os.path.join(d, "src.ffindex")), 0, 0, 42, 42)


def y(clip):
    return core.std.ShufflePlanes(clip, 0, vs.GRAY)


def stats(clip, ref):
    l = core.std.CropAbs(y(clip), 620, 440, 1350, 1120)
    blur = core.std.BoxBlur(l, hradius=4, vradius=4, hpasses=2, vpasses=2)
    p = core.std.PlaneStats(blur).get_frame(0).props
    mean = p["PlaneStatsAverage"] * 1023
    sq = core.std.PlaneStats(core.std.Expr([blur], f"x {p['PlaneStatsAverage'] * 1023} - dup *")).get_frame(0).props
    folds = (sq["PlaneStatsAverage"] * 1023) ** 0.5   # Expr on 10-bit clips works in codes; average is normalized
    # Where the source is dark: mean of (clip - source), masked.
    ys, yc = y(ref), y(clip)
    diff = core.std.Expr([ys, yc], "x 200 < y x - 512 + 512 ?")
    cnt = core.std.Expr([ys], "x 200 < 1023 0 ?")
    sh = (core.std.PlaneStats(diff).get_frame(0).props["PlaneStatsAverage"] * 1023 - 512)
    frac = core.std.PlaneStats(cnt).get_frame(0).props["PlaneStatsAverage"]
    return mean, folds, sh / max(frac, 1e-6)


s = src[112:113]
import sys
names = ["source", "fft3dgpu"] + (sys.argv[1:] or ["dlss_default", "dlss_1pass", "dlss_nomotion", "dlss_realtime_mv", "dlss_ldr_pq",
         "dlss_exposure4", "dlss_auto_exposure", "dlss_K", "dlss_raw", "dlss_global", "dlss_bm3d", "dlss_bm3d_raw"])
print(f"{'variant':22} {'mean':>7} {'folds':>7} {'shadow shift':>13}")
for n in names:
    if n == "source":
        c = s
    else:
        f = os.path.join(d, "fft3d_dark.y4m" if n == "fft3dgpu" else f"dark_{n}.y4m")
        c = core.bs.VideoSource(f)
        c = c[12:13] if n == "fft3dgpu" else c[0:1]
    m, fo, sh = stats(c, s)
    print(f"{n:22} {m:7.1f} {fo:7.2f} {sh:+13.2f}")
