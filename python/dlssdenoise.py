"""DLSS Denoiser: NVIDIA DLSS Super Resolution in DLAA mode as a degrainer, for VapourSynth.

    import dlssdenoise
    clip = dlssdenoise.DLSSDenoise(clip)

The clip is a YUV film in PQ (HDR10, the base layer of Dolby Vision profile 8). It is
taken to R'G'B' -- PQ code values, nothing else changed -- through core.dlss.Denoise
(vsdlss.dll), which runs DLSS on linear light, and back to the clip's own format. Its
frame properties go through, so whatever carries the HDR metadata downstream finds them.

strength  1.0 is DLSS's picture, 0.0 the source untouched (bit for bit); in between, a mix
limit     in 10-bit code values: a change larger than this is let through less and less, and
          one larger than `elast` times it not at all -- what DLSS changes beyond the grain's
          amplitude is detail, not grain. None: no limit
chroma    "dlss" or "source": the chroma planes of DLSS's picture or of the source
prefilter None, "bm3d", "dfttest" or a clip of the same format: a copy with the grain taken
          out, on which the motion is measured instead of the grained picture
**dlss    passed to core.dlss.Denoise: passes (2: forward and backward, the default; 1:
          forward only, three times faster), chunk, lookahead, preset ("J", "K", "L", "M"),
          exposure, pad, motion, flow_factor, grid, perf, flow_blur, scenecut, preroll, dll,
          device, linear, auto_exposure
"""

import vapoursynth as vs

core = vs.core

__all__ = ["DLSSDenoise"]

_PQ = 16
_MATRIX = {9: "2020ncl", 10: "2020cl", 1: "709", 6: "170m"}
_CHROMALOC = {0: "left", 1: "center", 2: "top_left", 3: "top", 4: "bottom_left", 5: "bottom"}


def _props(clip):
    return clip.get_frame(0).props


def _to_rgb(clip, matrix, crange, chromaloc, half):
    return core.resize.Spline36(clip, format=vs.RGBH if half else vs.RGBS, matrix_in_s=matrix,
                                range_in=crange, chromaloc_in_s=chromaloc)


def _limit(flt, src, limit, elast):
    """Changes up to `limit` kept, beyond limit * elast reverted, a ramp between."""
    scale = (1 << (src.format.bits_per_sample - 10)) if src.format.sample_type == vs.INTEGER else 1.0 / 1023.0
    t = limit * scale
    t2 = t * elast
    expr = (f"y x - abs {t} <= y y x - abs {t2} >= x "
            f"x y x - {t2} y x - abs - * {t2} {t} - / + ? ?")
    return core.std.Expr([src, flt], expr)


def _staxrip_plugins():
    r"""StaxRip's VapourSynth plugin folder, from where its portable VapourSynth lives:
    StaxRip\Apps\FrameServer\VapourSynth\Lib\site-packages\vapoursynth -> StaxRip\Apps\Plugins\VS."""
    import os
    here = os.path.dirname(os.path.abspath(vs.__file__))
    for _ in range(6):
        candidate = os.path.join(here, "Plugins", "VS")
        if os.path.basename(here).lower() == "apps" and os.path.isdir(candidate):
            return candidate
        here = os.path.dirname(here)
    return None


def _ensure_plugin(namespace, relative):
    """A plugin the prefilter needs, loaded from StaxRip's folder when the script did not:
    StaxRip loads a plugin only when the script itself names one of its functions."""
    if hasattr(core, namespace):
        return
    import os
    folder = _staxrip_plugins()
    path = os.path.join(folder, relative) if folder else None
    if not path or not os.path.isfile(path):
        raise vs.Error(f"DLSSDenoise: the prefilter needs core.{namespace}; load {relative} with core.std.LoadPlugin")
    core.std.LoadPlugin(path, altsearchpath=True)


def _prefilter(clip, kind):
    if kind == "bm3d":
        _ensure_plugin("bm3dcuda", r"BM3DCUDA\bm3dcuda.dll")
        f = core.resize.Point(clip, format=clip.format.replace(sample_type=vs.FLOAT, bits_per_sample=32).id)
        f = core.bm3dcuda.BM3Dv2(f, sigma=[3.0, 2.0, 2.0], radius=2)
        return core.resize.Point(f, format=clip.format.id)
    if kind == "dfttest":
        import os
        import sys
        _ensure_plugin("dfttest2_nvrtc", r"DFTTest2\dfttest2_nvrtc.dll")
        _ensure_plugin("dfttest2_cuda", r"DFTTest2\dfttest2_cuda.dll")
        folder = _staxrip_plugins()
        if folder and os.path.join(folder, "DFTTest2") not in sys.path:
            sys.path.append(os.path.join(folder, "DFTTest2"))
        import dfttest2
        # On integer input DFTTest2 hands the clip back untouched: float it is.
        f = core.resize.Point(clip, format=clip.format.replace(sample_type=vs.FLOAT, bits_per_sample=32).id)
        f = dfttest2.DFTTest(f, sigma=8.0, tbsize=3)
        return core.resize.Point(f, format=clip.format.id)
    raise vs.Error(f"DLSSDenoise: unknown prefilter {kind!r} (None, 'bm3d', 'dfttest' or a clip)")


def DLSSDenoise(clip: vs.VideoNode, strength: float = 1.0, limit: float | None = None, elast: float = 2.0,
                chroma: str = "dlss", prefilter=None, half: bool = False, **dlss) -> vs.VideoNode:
    if not isinstance(clip, vs.VideoNode) or clip.format is None or clip.format.color_family != vs.YUV:
        raise vs.Error("DLSSDenoise: a YUV clip of constant format is expected")
    if chroma not in ("dlss", "source"):
        raise vs.Error("DLSSDenoise: chroma is 'dlss' or 'source'")
    if strength <= 0.0:
        return clip

    props = _props(clip)
    transfer = props.get("_Transfer", _PQ)
    if transfer != _PQ:
        raise vs.Error("DLSSDenoise: the clip is not PQ (_Transfer 16): only HDR10 sources are handled for now")
    matrix = _MATRIX.get(props.get("_Matrix", 9), "2020ncl")
    crange = props.get("_ColorRange", 1)            # 1 limited, 0 full
    chromaloc = _CHROMALOC.get(props.get("_ChromaLocation", 2), "top_left")

    rgb = _to_rgb(clip, matrix, crange, chromaloc, half)
    ref = None
    if prefilter is not None:
        pre = prefilter if isinstance(prefilter, vs.VideoNode) else _prefilter(clip, prefilter)
        ref = _to_rgb(pre, matrix, crange, chromaloc, half)

    out = core.dlss.Denoise(rgb, ref=ref, **dlss) if ref is not None else core.dlss.Denoise(rgb, **dlss)
    yuv = core.resize.Spline36(out, format=clip.format.id, matrix_s=matrix, range=crange,
                               chromaloc_s=chromaloc, dither_type="none")

    if chroma == "source":
        yuv = core.std.ShufflePlanes([yuv, clip, clip], planes=[0, 1, 2], colorfamily=vs.YUV)
    if limit is not None:
        yuv = _limit(yuv, clip, float(limit), float(elast))
    if strength < 1.0:
        yuv = core.std.Merge(clip, yuv, weight=float(strength))
    return yuv
