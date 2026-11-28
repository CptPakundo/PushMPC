"""score.py <ffmpeg> <ffprobe> <output> <master> [--offset N]: frame count and rate of <output>, and PSNR (luma, frame by
frame from the first frame, as many frames as both have) against <master> for the deinterlace probe. Prints one line:
  frames N (master M), rate R; psnr Y avg A dB, min B dB, exact E of K (inf), >= 45 dB G of K"""
import re, subprocess, sys, tempfile, os

ff, fp, out, master = sys.argv[1:5]
off = int(sys.argv[sys.argv.index("--offset") + 1]) if "--offset" in sys.argv else 0
def probe(p):
    r = subprocess.run([fp, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames,r_frame_rate,avg_frame_rate",
                        "-of", "default=nw=1", p], capture_output=True, text=True).stdout
    d = dict(l.split("=", 1) for l in r.split() if "=" in l)
    return int(d.get("nb_read_frames", "0") or 0), d.get("avg_frame_rate", "?")
n, rate = probe(out)
m, mrate = probe(master)
stats = tempfile.NamedTemporaryFile(delete=False, suffix=".txt").name
g = (f"[0]trim=start_frame={off},settb=1/100,setpts=N,format=yuv420p,extractplanes=y[a];"
     f"[1]settb=1/100,setpts=N,format=yuv420p,extractplanes=y[b];[a][b]psnr=stats_file='{stats.replace(os.sep, '/').replace(':', chr(92) + ':')}':shortest=1")
subprocess.run([ff, "-v", "error", "-i", out, "-i", master, "-lavfi", g, "-f", "null", "-"], capture_output=True)
vals = []
for line in open(stats):
    mm = re.search(r"psnr_(?:avg|y):(\S+)", line)
    if mm: vals.append(float("inf") if mm.group(1) == "inf" else float(mm.group(1)))
fin = [v for v in vals if v != float("inf")]
k = len(vals)
avg = (sum(min(v, 100.0) for v in vals) / k) if k else float("nan")
print(f"frames {n} (master {m}), rate {rate} (master {mrate}); psnr Y avg {avg:.2f} dB (inf as 100), min {min(fin) if fin else float('inf'):.2f} dB, "
      f"exact {k - len(fin)} of {k}, >= 45 dB {sum(1 for v in vals if v >= 45)} of {k}")
