"""Synthetic test clips for the Kodi cases (testsrc2 moves every frame, so missing or repeated pictures show).
usage: gen_clips.py <ffmpeg> <outdir>"""
import os, subprocess, sys

ff, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

def run(args):
    print("+", " ".join(args), flush=True)
    subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y"] + args, check=True)

srt = os.path.join(out, "subs.srt")
with open(srt, "w", encoding="utf-8") as f:
    for i in range(10):
        a, b = i * 3, i * 3 + 2
        f.write(f"{i+1}\n00:00:{a:02d},000 --> 00:00:{b:02d},500\nSubtitle line {i+1}\n\n")
meta = os.path.join(out, "chapters.txt")
with open(meta, "w", encoding="utf-8") as f:
    f.write(";FFMETADATA1\n")
    for i, (s, e) in enumerate([(0, 10000), (10000, 20000), (20000, 30000)]):
        f.write(f"[CHAPTER]\nTIMEBASE=1/1000\nSTART={s}\nEND={e}\ntitle=Chapter {i+1}\n")

src = ["-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24000/1001:duration=30",
       "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=30"]
# H.264 + AAC + SRT subtitles + chapters, mkv (the main clip)
run(src + ["-i", srt, "-i", meta, "-map", "0:v", "-map", "1:a", "-map", "2:s", "-map_metadata", "3", "-map_chapters", "3",
           "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-g", "48", "-c:a", "aac", "-b:a", "96k",
           "-c:s", "srt", "-metadata:s:s:0", "language=eng", "-metadata:s:a:0", "language=eng",
           os.path.join(out, "h264.mkv")])
# H.264 in mp4
run(src[:4] + ["-t", "15", "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", os.path.join(out, "h264.mp4")])
# HEVC 8-bit (Kodi 21's add-on API has no HEVC: announced under another name)
run(src[:4] + ["-t", "15", "-c:v", "libx265", "-preset", "fast", "-pix_fmt", "yuv420p", "-x265-params", "log-level=error",
               os.path.join(out, "hevc.mkv")])
# HEVC 10-bit HDR10 (PQ, BT.2020)
run(src[:4] + ["-t", "15", "-c:v", "libx265", "-preset", "fast", "-pix_fmt", "yuv420p10le",
               "-x265-params", "log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc",
               "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
               os.path.join(out, "hdr10.mkv")])
# MPEG-2 in a transport stream (not in Kodi's add-on codec list at all)
run(src[:4] + ["-t", "15", "-c:v", "mpeg2video", "-q:v", "4", "-pix_fmt", "yuv420p", os.path.join(out, "mpeg2.ts")])
print("clips:", sorted(os.listdir(out)))
