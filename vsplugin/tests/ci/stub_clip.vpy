# vspipe test clip for pushmpc.Enhance against the stub core (CI, no GPU): 48 frames of 320x240 YUV420P8 at
# 24000/1001 whose brightness steps every frame, through Enhance with the multiplier from PUSHMPC_TEST_K.
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs.dll     PUSHMPC_STUB_DIR  folder of the stub pushcore.dll
import os
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
k = os.environ.get("PUSHMPC_TEST_K", "4")
frames = int(os.environ.get("PUSHMPC_TEST_FRAMES", "48"))
bits = int(os.environ.get("PUSHMPC_TEST_BITS", "8"))
fmt = vs.YUV420P10 if bits > 8 else vs.YUV420P8
scale = 4 if bits > 8 else 1
clips = [core.std.BlankClip(format=fmt, width=320, height=240, length=1, fpsnum=24000, fpsden=1001,
                            color=[(16 + 4 * (i % 50)) * scale, 128 * scale, 128 * scale]) for i in range(frames)]
clip = core.std.Splice(clips)
clip = core.std.SetFrameProps(clip, _Matrix=1, _Transfer=1, _Primaries=1, _ColorRange=1)
clip = core.pushmpc.Enhance(clip, smoother=k, sharper="off", adapter="warp", core_dir=os.environ["PUSHMPC_STUB_DIR"],
                            chroma=os.environ.get("PUSHMPC_TEST_CHROMA", "auto"))
clip.set_output()
