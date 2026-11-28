# PushMPC for mpv (vf=vapoursynth): frame generation through pushmpc.Enhance. mpv sets video_in, container_fps,
# display_fps and user_data; mpv reloads this script on every seek (the plugin keeps its core warm across that).
#
# Backends (pushmpc.SelectBackend picks one; user_data backend=... or the Backend setting overrides Auto):
#   nvidia    RTX Video Super Resolution, NVIDIA frame generation (VFG), RTX Video HDR, the scene-cut guard: pushcore on
#             Windows; on Linux NVIDIA's Video Effects runtime through CUDA (installed by PushMPC's installer).
#   apple     Apple's VideoToolbox ML frame interpolation (+ super resolution up to 1080p sources) on Apple silicon,
#             macOS 26+ (frame-rate conversion on 15.4+); user_data apple_sr=off|2|4, apple_fi=lowlatency|frc.
#   rife      vs-rife-ncnn-vulkan (core.rife), when you have it installed (any Vulkan GPU). Never downloaded by PushMPC.
#   portable  PushFPS's motion-compensated frame generation on the CPU (any GPU, Windows, Linux, macOS).
# Auto: nvidia on an RTX GPU with pushcore, else apple when the Mac offers it, else rife when installed, else portable.
#
# user_data (optional, from the vf option user-data=...): "key=value,key=value" with the plugin's arguments (pushmpc-display.lua
# adds display_hdr, display_peak, display_w, display_h, display_fs from mpv's own view of the display), e.g.
#   smoother=4,sharper=2,chroma=420        backend=portable,portable_threads=8        backend=rife,rife_model=46
# Settings otherwise come from PushMPC's own settings: HKCU\Software\PushMPC on Windows (the same as in MPC-HC),
# ~/.config/pushmpc/settings.ini on Linux and macOS.
import os
import sys
import vapoursynth as vs

core = vs.core

_here = os.path.dirname(os.path.abspath(globals().get("__file__", ""))) if globals().get("__file__") else ""
_home = os.path.expanduser("~")
if sys.platform == "win32":
    _lib = "pushmpc_vs.dll"
    _dirs = [_here, os.path.join(os.environ.get("LOCALAPPDATA", ""), "PushMPC", "vsplugin")]
elif sys.platform == "darwin":
    _lib = "pushmpc_vs.dylib"
    _dirs = [_here, os.path.join(_home, "Library", "Application Support", "PushMPC", "vsplugin")]
else:
    _lib = "pushmpc_vs.so"
    _dirs = [_here, os.path.join(os.environ.get("XDG_DATA_HOME", "") or os.path.join(_home, ".local", "share"), "pushmpc", "vsplugin")]
_candidates = [os.path.join(_d, _lib) for _d in _dirs if _d]
if not hasattr(core, "pushmpc"):
    for _p in _candidates:
        if os.path.isfile(_p):
            core.std.LoadPlugin(_p)
            break

clip = globals().get("video_in")
if clip is None:
    raise vs.Error("pushmpc.vpy: no video_in (run it from mpv's vf=vapoursynth)")
_raw = clip   # mpv's own input node (pushmpc.MpvColour reads frame 0's colour from it)

# mpv's input has no rate of its own: the container's, with the exact NTSC fractions
_fps = globals().get("container_fps", 0) or 0
if _fps <= 0.1:
    _fps = 24000 / 1001
for _num, _den in ((24000, 1001), (30000, 1001), (60000, 1001), (120000, 1001)):
    if abs(_fps - _num / _den) < 0.002:
        _fpsnum, _fpsden = _num, _den
        break
else:
    _fpsnum, _fpsden = int(round(_fps * 1000)), 1000
clip = core.std.AssumeFPS(clip, fpsnum=_fpsnum, fpsden=_fpsden)

# the plugin takes YUV 4:2:0 at 8 or 10 bits (mpv hands out its decoder's format; others are converted)
_f = clip.format
if _f.color_family != vs.YUV or _f.subsampling_w != 1 or _f.subsampling_h != 1 or _f.sample_type != vs.INTEGER:
    clip = core.resize.Bicubic(clip, format=vs.YUV420P10 if _f.bits_per_sample > 8 else vs.YUV420P8)

_args = {}
_ud = globals().get("user_data", "") or ""
for _kv in str(_ud).split(","):
    if "=" in _kv:
        _k, _v = _kv.split("=", 1)
        _k, _v = _k.strip(), _v.strip()
        if _k in ("display_hdr", "matrix", "transfer", "primaries", "range", "wait_ms", "display_w", "display_h", "display_fs"):
            _args[_k] = int(_v)
        elif _k in ("display_fps", "sharpness", "display_peak"):
            _args[_k] = float(_v)
        elif _k:
            _args[_k] = _v

# frame 0's colour while this script loads, when the user-data has none (pushmpc.lua adds the filter before decoding,
# when mpv knows no colour yet): mpv's dummy frame carries frame 0's params (pushmpc.MpvColour, vsplugin mp_params.h).
# mpv logs "Frame requested during init" for that one read, by design. The user-data's colour wins when it has one.
_mpvver = str(_args.pop("mpvver", "") or "")
if "transfer" not in _args and hasattr(core.pushmpc, "MpvColour"):
    _mc = core.pushmpc.MpvColour(_raw, mpvver=_mpvver)
    if int(_mc["ok"]):
        for _k in ("matrix", "transfer", "primaries", "range"):
            if _k not in _args and int(_mc[_k]) >= 0:
                _args[_k] = int(_mc[_k])
# this script runs only inside mpv (video_in): playback, so Auto's anime upscaler is checked against real time (an
# encode script never passes it)
_args["playback"] = 1
_dfps = globals().get("display_fps", 0) or 0
if _dfps > 1 and "display_fps" not in _args:
    _args["display_fps"] = float(_dfps)
# mpv 0.36+ also passes the display size (display_res: [w, h]): the GPU engine upscales to it (Upscaler, UpscaleTarget)
_dres = globals().get("display_res")
try:
    if _dres and len(_dres) >= 2 and int(_dres[0]) > 0 and "display_w" not in _args:
        _args["display_w"], _args["display_h"] = int(_dres[0]), int(_dres[1])
except (TypeError, ValueError):
    pass

def _text(v):
    return v.decode() if isinstance(v, (bytes, bytearray)) else str(v)


def _rife(c, args):
    # the multiplier: smoother=N, else the display rate over the source rate (x2 when the display is unknown)
    _sm = str(args.get("smoother", "auto")).lower().lstrip("x")
    if _sm in ("off", "0", "1"):
        return c
    if _sm.isdigit():
        k = min(int(_sm), 16)   # PushMPC's ceiling (PUSHCORE_MAX_MULTIPLIER)
    else:
        _src = c.fps_num / c.fps_den if c.fps_den else 23.976
        k = max(1, min(16, int(args.get("display_fps", 0) / _src + 0.02))) if args.get("display_fps", 0) > 1 else 2
    if k <= 1:
        return c
    bits = c.format.bits_per_sample
    # mpv's frames carry their matrix, but none is known while the script loads: the same guess as the plugin's
    mat = "2020ncl" if bits > 8 and c.width >= 3200 else ("170m" if c.width < 1024 and c.height < 600 else "709")
    rgb = core.resize.Bicubic(c, format=vs.RGBS, matrix_in_s=mat)
    sc = hasattr(core, "misc")
    if sc:
        rgb = core.misc.SCDetect(rgb, threshold=0.12)   # RIFE's sc=True reads _SceneChangeNext: no blend across a cut
    opts = {"factor_num": k, "factor_den": 1, "sc": sc, "uhd": c.width > 1920}
    if "rife_model" in args:
        opts["model"] = int(args["rife_model"])
    if "rife_gpu" in args:
        opts["gpu_id"] = int(args["rife_gpu"])
    out = core.rife.RIFE(rgb, **opts)
    return core.resize.Bicubic(out, format=vs.YUV420P10 if bits > 8 else vs.YUV420P8, matrix_s=mat)


_selargs = {_k: _args[_k] for _k in ("core_dir", "runtime_dir", "display_fps") if _k in _args}
_sel = core.pushmpc.SelectBackend(backend=_args.pop("backend", "") or "", width=clip.width, height=clip.height, fpsnum=clip.fps.numerator, fpsden=clip.fps.denominator, **_selargs)
_backend = _text(_sel["backend"])
# interp: no VFG on this GPU (Linux: RTX 20, or RTX 30 before its self-test): Auto compared the measured interpolators;
# ml (RIFE through pmml), rife or portable interpolates first, at the source size, then nvidia upscales every frame;
# empty: the nvidia backend generates the frames itself (VFG, or its NVIDIA optical-flow tier)
try:
    _interp = _text(_sel["interp"])
except (KeyError, IndexError):
    _interp = ""
# interlaced input (vsplugin vs_interlace.h): field combing measured in the picture once for the whole chain (the
# frames' _FieldBased is only a prior). Interlaced: PushMPC steps aside and the source frames pass through at the source
# rate. mpv's source has no length yet, so the first frames are measured as they play; a verdict that differs from how
# this script started makes pushmpc.lua add the filter again (interlace_reload in the status file).
_il = None
if hasattr(core.pushmpc, "Interlace"):
    _il = core.pushmpc.Interlace(clip, end_frames=0 if (_backend == "rife" or _interp == "rife") else 1)
    clip = _il["clip"]
    _args["interlace_check"] = 0
if _il is not None and int(_il["interlaced"]):
    pass
elif _backend == "rife":
    clip = _rife(clip, _args)
else:
    if _backend == "nvidia" and _interp == "rife":
        clip = _rife(clip, _args)
        _args["smoother"] = "off"
    for _k in ("rife_model", "rife_gpu"):
        _args.pop(_k, None)
    if _backend == "nvidia" and _interp == "ml":
        clip = core.pushmpc.Enhance(clip, backend="ml", **_args)
        _args["smoother"] = "off"
    if _backend == "nvidia" and _interp == "portable":
        clip = core.pushmpc.Enhance(clip, backend="portable", **_args)
        _args["smoother"] = "off"
    clip = core.pushmpc.Enhance(clip, backend=_backend, **_args)
# mpv's vf=vapoursynth takes planar YUV only ("Unsupported output format" for RGB): the plugin's full-chroma RGB output
# (chroma auto/444) goes to YUV 4:4:4, so no chroma resolution is lost. mpv keeps the input's colour parameters for the
# output, and none are known while the script loads: the same matrix guess as the plugin's and _rife's.
# Keep this rule in step with vsplugin/src/mp_params.h VpyRgbMatrix: the plugin writes it into the output's _MP_IMAGE.
if clip.format.color_family == vs.RGB:
    _b = clip.format.bits_per_sample
    _mat = "2020ncl" if _b > 8 and clip.width >= 3200 else ("170m" if clip.width < 1024 and clip.height < 600 else "709")
    clip = core.resize.Point(clip, format=vs.YUV444P10 if _b > 8 else vs.YUV444P8, matrix_s=_mat)
clip.set_output()
