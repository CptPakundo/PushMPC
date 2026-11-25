"""Judge one Kodi test case from kodi.log (debug log with the LOGVIDEO and LOGAVTIMING components) and drive.py output.

usage: analyze.py <kodi.log> <drive.txt> <case> <k> <decode 0|1> <src_fps> <out_w>x<out_h> <summary.md>
Checks (each printed; exit 1 if a required one fails):
  addon     Kodi created the add-on decoder (or, for decode=0, kept its own) for this file
  pictures  the add-on returned k pictures per source frame, timestamps strictly increasing on the 1/k grid
  kodi      Kodi's CAddonVideoCodec accepted them (VC_PICTURE lines with our timestamps and the expected size)
  render    RenderManager presented them: distinct presented pts per second vs min(k x fps, display rate)
  drive     JSON-RPC checks for the case (seek, chapter, subtitles, audio)
"""
import json, re, statistics, sys

log_path, drive_path, case, k, decode, fps, size, summary = sys.argv[1:9]
k, decode, fps = int(k), decode == "1", float(fps)
_text0 = open(log_path, encoding="utf-8", errors="replace").read()
if k == 0:  # Auto: take the multiplier the add-on chose
    _m = re.search(r"PushMPC: decoder [^,]*, x(\d+),", _text0)
    k = int(_m.group(1)) if _m else 1
size_any = size in ("*", "up")
size_up = size == "up"
ow, oh = (0, 0) if size_any else (int(v) for v in size.split("x"))
text = open(log_path, encoding="utf-8", errors="replace").read()
drive = open(drive_path, encoding="utf-8", errors="replace").read() if drive_path != "-" else ""
lines = text.splitlines()
res, notes = {}, []

def ts(line):
    m = re.match(r"(\d\d\d\d-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d+)", line)
    if not m:
        return None
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + float("0." + m.group(5))

# --- addon
ours = [l for l in lines if "PushMPC:" in l]
for l in ours:
    if any(x in l for x in ("video:", "video stays", "decoder ", "engine:", "upscaling", "multiplier:")):
        notes.append(l.split("PushMPC:", 1)[1].strip())
has_decoder = any("PushMPC: decoder" in l for l in lines)
kodi_codec = [l for l in lines if "Creating video codec" in l or "CAddonVideoCodec" in l and "Failed" in l]
res["addon"] = has_decoder if decode else (not has_decoder and any("video stays" in l for l in lines))

# --- pictures (the add-on's own log of each returned picture)
out = [(int(m.group(1)), m.group(2) == "1") for m in re.finditer(r"PushMPC: out pts=(-?\d+) real=(\d)", text)]
if decode:
    pts = [p for p, _ in out]
    real = sum(1 for _, r in out if r)
    d = [b - a for a, b in zip(pts, pts[1:])]
    step = 1e6 / (fps * k)
    on_grid = [x for x in d if abs(x - step) <= 1000.0 / k + 1.5]
    back = sum(1 for x in d if x <= 0)
    ratio = len(out) / real if real else 0
    grid_share = len(on_grid) / len(d) if d else 0
    res["pictures"] = bool(out) and back <= 2 and abs(ratio - k) <= 0.15 * k + 0.05 and grid_share >= 0.9
    notes.append(f"add-on returned {len(out)} pictures, {real} real (x{ratio:.2f}), {grid_share*100:.1f}% of steps on the "
                 f"{step/1000:.3f} ms grid, {back} steps back, median step {statistics.median(d)/1000 if d else 0:.3f} ms")
else:
    res["pictures"] = not out

# --- kodi: CAddonVideoCodec accepted the pictures
vc = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in
      re.finditer(r"CAddonVideoCodec: GetPicture::VC_PICTURE with pts (-?\d+) (\d+)x(\d+)", text)]
if decode:
    sizes = {(w, h) for _, w, h in vc}
    size_ok = (len(sizes) == 1 and (not size_up or all(w > 640 and h > 360 for w, h in sizes))) if size_any else sizes == {(ow, oh)}
    res["kodi"] = len(vc) >= 0.9 * len(out) and size_ok
    notes.append(f"Kodi took {len(vc)} add-on pictures, sizes {sorted(sizes)}")
else:
    res["kodi"] = not vc
cfg = [l.split(" - ", 1)[-1] for l in lines if "CRenderManager::Configure" in l or "Invalid framerate" in l
       or "framerate was:" in l or "Display resolution ADJUST" in l]
notes += [f"kodi: {c.strip()}" for c in cfg[:8]]

# --- render: LOGAVTIMING logs every PrepareNextRender call (one per rendered GUI frame) with the pts at the front of the
# render queue. Each picture reaches the front once, so distinct front pts per second = pictures the renderer took in
# (shown, or skipped because a newer one was already due); the loop rate is the GUI's achieved refresh (software GL on
# a runner is slower than the display mode). Pass: >= 85% of min(source x k, loop rate).
rend = []
for l in lines:
    m = re.search(r"PrepareNextRender: frameOnScreen: (-?[d.]+) renderPts: (-?[d.]+) nextFramePts: (-?[d.]+)", l)
    if m:
        t = ts(l)
        if t is not None:
            rend.append((t, float(m.group(3))))
hz = None
for l in lines:
    m = re.search(r"@ ?([d.]+) ?Hz", l)
    if m and "Display" in l:
        hz = float(m.group(1))
if len(rend) > 10:
    span = rend[-1][0] - rend[0][0]
    distinct = len({round(p) for _, p in rend})
    rate = distinct / span if span > 0 else 0
    loop = len(rend) / span if span > 0 else 0
    want = fps * (k if decode else 1)
    res["render"] = rate >= 0.85 * min(want, loop)
    notes.append(f"renderer took {distinct} distinct pictures in {span:.1f} s = {rate:.1f}/s; render loop {loop:.1f}/s; source x{k if decode else 1} = "
                 f"{want:.1f}/s; display mode {hz or 'unknown'} Hz")
else:
    res["render"] = False
    notes.append("no LOGAVTIMING render lines")
drops = sum(1 for l in lines if "dropped in output" in l)
notes.append(f"VideoPlayer 'dropped in output': {drops}")

# --- drive
for key in ("seek_ok", "chapter_ok", "subs_ok", "audio_ok"):
    m = re.search(r"DRIVE " + key + r" (True|False)", drive)
    if m:
        res[key] = m.group(1) == "True"
if "DRIVE: playing" not in drive and drive_path != "-":
    res["playing"] = False
audio = any("Creating audio thread" in l or "CVideoPlayerAudio::OpenStream" in l for l in lines)
notes.append(f"audio stream opened by Kodi: {audio}")
errors = [l for l in lines if re.search(r"ERROR <general>.*(VideoPlayer|CAddonVideoCodec|InputStream|RenderManager|PushMPC)", l)]
notes += ["ERROR: " + e.split("ERROR <general>:", 1)[-1].strip() for e in errors[:6]]

if case == "nis" and size == "up":
    res["gpu"] = "engine: GPU" in text
required = ["addon", "pictures", "kodi", "render"] + (["gpu"] if "gpu" in res else []) + [x for x in ("seek_ok", "chapter_ok", "subs_ok", "audio_ok", "playing") if x in res]
ok = all(res.get(x) for x in required)
with open(summary, "a", encoding="utf-8") as f:
    f.write(f"### {case}: {'PASS' if ok else 'FAIL'}\n")
    f.write("checks: " + ", ".join(f"{x}={'ok' if res.get(x) else 'FAIL'}" for x in required) + "\n\n")
    for n in notes:
        f.write(f"- {n}\n")
    f.write("\n")
print(json.dumps({"case": case, "ok": ok, "checks": res}, indent=1))
print("\n".join(notes))
sys.exit(0 if ok else 1)
