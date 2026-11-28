# Interlaced input through pushmpc.Enhance (CI, no GPU): a smooth random texture panning 4 pixels per frame, 320x240
# YUV420P8 at 30000/1001, made from the core's own filters (BlankClip, Stack, resize: every VapourSynth has them).
#   PUSHMPC_IL_KIND = woven            two fields from different moments per frame (a 60i capture woven), flagged TFF
#                   = woven-unflagged  the same, flagged progressive (_FieldBased 0): the picture must still decide
#                   = progressive      the same motion as whole frames, flagged TFF: the flag must not decide
#                   = telecine         24p film of the pan carried as 60i by 2:3 pulldown (30000/1001), flagged TFF
#                   = film             that 24p film itself (the master the inverse telecine must give back)
#   PUSHMPC_IL_FRAMES  frames (default 120)       PUSHMPC_IL_MODE = enhance (default) | clip (the source alone, for y4m)
#   PUSHMPC_VS_PLUGIN, PUSHMPC_STUB_DIR, PUSHMPC_TEST_K as stub_clip.vpy
import os
import random
import vapoursynth as vs

core = vs.core
kind = os.environ.get("PUSHMPC_IL_KIND", "woven")
frames = int(os.environ.get("PUSHMPC_IL_FRAMES", "120"))
mode = os.environ.get("PUSHMPC_IL_MODE", "enhance")

rng = random.Random(7)
cols = (4 * frames + 360) // 24 + 1   # room for the whole pan (2 pixels per field)
rows = [core.std.StackHorizontal([core.std.BlankClip(format=vs.GRAY8, width=1, height=1, length=1, color=[rng.randint(24, 232)]) for _ in range(cols)])
        for _ in range(16)]
tex = core.resize.Bicubic(core.std.StackVertical(rows), 24 * cols, 384)   # 24 px cells: smooth
# 2 x frames pictures at 60000/1001, the texture 2 pixels further right each (one per field of a 60i capture)
base = core.std.Splice([core.resize.Bicubic(tex, 320, 240, src_left=8 + 2 * n, src_top=72, src_width=320, src_height=240) for n in range(2 * frames)])
base = core.std.AssumeFPS(base, fpsnum=60000, fpsden=1001)
if kind == "progressive":
    luma = core.std.SelectEvery(base, 2, 0)            # whole frames
    fb = 2
elif kind in ("film", "telecine"):
    # film: 24p frames of the same pan (5 pixels apart); telecine: that film carried as 60i by 2:3 pulldown (output
    # frames AA BB BC CD DD of the film frames A..D: the top field of B with the bottom field of C, and so on)
    film = core.std.Splice([core.resize.Bicubic(tex, 320, 240, src_left=8 + 5 * n, src_top=72, src_width=320, src_height=240) for n in range((4 * frames) // 5)])
    film = core.std.AssumeFPS(film, fpsnum=24000, fpsden=1001)
    if kind == "film":
        luma = film
    else:
        f = core.std.SeparateFields(film, tff=True)       # A top, A bottom, B top, B bottom, ...
        luma = core.std.DoubleWeave(core.std.SelectEvery(f, 8, [0, 1, 2, 3, 2, 5, 4, 7, 6, 7]), tff=True)[::2]
    fb = 0 if kind == "film" else 2
else:
    fields = core.std.SeparateFields(base, tff=True)    # t0 top, t0 bottom, t1 top, t1 bottom, ...
    pick = core.std.SelectEvery(fields, 4, [0, 3])      # the top field of t0, the bottom field of t1, ...
    luma = core.std.DoubleWeave(pick, tff=True)[::2]    # each frame: two moments
    fb = 0 if kind == "woven-unflagged" else 2
luma = core.std.AssumeFPS(luma, fpsnum=24000 if kind == "film" else 30000, fpsden=1001)
chroma = core.std.BlankClip(luma, format=vs.GRAY8, width=160, height=120, color=[128])
clip = core.std.ShufflePlanes([luma, chroma, chroma], planes=[0, 0, 0], colorfamily=vs.YUV)
clip = core.std.RemoveFrameProps(clip, ["_Field", "_Combed"])
clip = core.std.SetFrameProps(clip, _FieldBased=fb, _Matrix=1, _Transfer=1, _Primaries=1, _Range=1)
if mode != "clip":
    core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
    clip = core.pushmpc.Enhance(clip, smoother=os.environ.get("PUSHMPC_TEST_K", "4"), sharper="off", adapter="warp",
                                core_dir=os.environ["PUSHMPC_STUB_DIR"])
clip.set_output()
