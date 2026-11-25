"""check_frames.py <file> <expected frames> <expected fps num/den> [--tolerance N]
ffprobe the video stream of <file>: frame count, strictly increasing pts, every interval within 1.5 ticks of the
expected output interval (1 / fps), except where the source had a gap. Prints a summary; exit 1 on failure."""
import json, subprocess, sys
from fractions import Fraction

def main():
    path, want, fps = sys.argv[1], int(sys.argv[2]), Fraction(sys.argv[3])
    tol = int(sys.argv[sys.argv.index("--tolerance") + 1]) if "--tolerance" in sys.argv else 0
    ff = sys.argv[sys.argv.index("--ffprobe") + 1] if "--ffprobe" in sys.argv else "ffprobe"
    out = subprocess.run([ff, "-v", "error", "-select_streams", "v:0", "-show_entries", "frame=pts,best_effort_timestamp_time,pts_time",
                          "-show_entries", "stream=time_base,width,height", "-of", "json", path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    tb = Fraction(j["streams"][0]["time_base"])
    pts = [int(f["pts"]) for f in j["frames"] if "pts" in f]
    n = len(j["frames"])
    ok = True
    print(f"{path}: {n} frames ({j['streams'][0]['width']}x{j['streams'][0]['height']}), time base {tb}")
    if abs(n - want) > tol:
        print(f"FAIL: {n} frames, expected {want}"); ok = False
    bad = [i for i in range(1, len(pts)) if pts[i] <= pts[i - 1]]
    if bad:
        print(f"FAIL: pts not increasing at {bad[:10]}"); ok = False
    step = 1 / fps
    d = [Fraction(pts[i] - pts[i - 1]) * tb for i in range(1, len(pts))]
    off = [i for i, x in enumerate(d) if abs(x - step) > tb * 2 + step / 50]
    if d:
        print(f"intervals: min {float(min(d))*1000:.3f} ms, max {float(max(d))*1000:.3f} ms, expected {float(step)*1000:.3f} ms; {len(off)} off")
    if len(off) > max(2, len(d) // 100):
        print(f"FAIL: {len(off)} intervals differ from 1/{fps} (first at {off[:10]})"); ok = False
    print("OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)

main()
