# A 10 s 320x240 YUV420P8 23.976 fps test clip (brightness steps each frame) for mpv in CI: vspipe -c y4m source_clip.vpy clip.y4m
import vapoursynth as vs

core = vs.core
clips = [core.std.BlankClip(format=vs.YUV420P8, width=320, height=240, length=1, fpsnum=24000, fpsden=1001,
                            color=[16 + 4 * (i % 50), 128, 128]) for i in range(240)]
core.std.Splice(clips).set_output()
