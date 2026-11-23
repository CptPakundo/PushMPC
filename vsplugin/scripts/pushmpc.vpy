# PushMPC for mpv (vf=vapoursynth): frame generation through pushmpc.Enhance. mpv sets video_in, container_fps,
# display_fps and user_data; mpv reloads this script on every seek (the plugin keeps its core warm across that).
#
# Backends (pushmpc.SelectBackend picks one; user_data backend=... or the Backend setting overrides Auto):
#   nvidia    pushcore: RTX Video Super Resolution, NVIDIA frame generation, the scene-cut guard (Windows, RTX).
#   rife      vs-rife-ncnn-vulkan (core.rife), when you have it installed (any Vulkan GPU). Never downloaded by PushMPC.
#   portable  PushFPS's motion-compensated frame generation on the CPU (any GPU, Windows, Linux, macOS).
# Auto: nvidia on an RTX GPU with pushcore, else rife when installed, else portable.
#
# user_data (optional, from the vf option user-data=...): "key=value,key=value" with the plugin's arguments, e.g.
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
        if _k in ("display_hdr", "matrix", "transfer", "primaries", "range", "wait_ms"):
            _args[_k] = int(_v)
        elif _k == "display_fps":
            _args[_k] = float(_v)
        elif _k:
            _args[_k] = _v

_dfps = globals().get("display_fps", 0) or 0
if _dfps > 1 and "display_fps" not in _args:
    _args["display_fps"] = float(_dfps)

def _text(v):
    return v.decode() if isinstance(v, (bytes, bytearray)) else str(v)


def _rife(c, args):
    # the multiplier: smoother=N, else the display rate over the source rate (x2 when the display is unknown)
    _sm = str(args.get("smoother", "auto")).lower().lstrip("x")
    if _sm in ("off", "0", "1"):
        return c
    if _sm.isdigit():
        k = min(int(_sm), 8)
    else:
        _src = c.fps_num / c.fps_den if c.fps_den else 23.976
        k = max(1, min(8, int(args.get("display_fps", 0) / _src + 0.02))) if args.get("display_fps", 0) > 1 else 2
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


_sel = core.pushmpc.SelectBackend(backend=_args.pop("backend", "") or "", **{_k: _args[_k] for _k in ("core_dir", "runtime_dir") if _k in _args})
_backend = _text(_sel["backend"])
if _backend == "rife":
    clip = _rife(clip, _args)
else:
    for _k in ("rife_model", "rife_gpu"):
        _args.pop(_k, None)
    clip = core.pushmpc.Enhance(clip, backend=_backend, **_args)
# mpv's vf=vapoursynth takes planar YUV only ("Unsupported output format" for RGB): the plugin's full-chroma RGB output
# (chroma auto/444) goes to YUV 4:4:4, so no chroma resolution is lost. mpv keeps the input's colour parameters for the
# output, and none are known while the script loads: the same matrix guess as the plugin's and _rife's.
if clip.format.color_family == vs.RGB:
    _b = clip.format.bits_per_sample
    _mat = "2020ncl" if _b > 8 and clip.width >= 3200 else ("170m" if clip.width < 1024 and clip.height < 600 else "709")
    clip = core.resize.Point(clip, format=vs.YUV444P10 if _b > 8 else vs.YUV444P8, matrix_s=_mat)
clip.set_output()
