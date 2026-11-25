"""Play the test clips in a real Kodi (Linux: Flathub Kodi under Xvfb with Mesa's software GL; Windows: the official
build, portable mode, on WARP) with inputstream.pushmpc installed, and judge each case with analyze.py.

usage: run_cases.py --kodi "<command>" --home <kodi home> --addon <built add-on dir> --clips <dir> --major 21|22
                    --out <dir> [--cases a,b,...]
<kodi home> is the folder holding addons/ and userdata/ (Flathub: ~/.var/app/tv.kodi.Kodi/data; portable Windows:
<kodi>/portable_data)."""
import argparse, glob, os, shlex, shutil, sqlite3, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--kodi", required=True)
ap.add_argument("--home", required=True)
ap.add_argument("--addon", required=True)
ap.add_argument("--clips", required=True)
ap.add_argument("--major", type=int, required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--cases", default="")
ap.add_argument("--kill", default="", help="command that stops any leftover Kodi")
ap.add_argument("--gpu", action="store_true", help="the GPU engine is expected (Vulkan present)")
ap.add_argument("--play", type=float, default=14.0)
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(a.out, exist_ok=True)
summary = os.path.join(a.out, "summary.md")
kodi22 = a.major >= 22

# name: clip, settings, expected k (0 = Auto), decode expected, output size, drive case
CASES = [
    ("h264_x2", "h264.mkv", {"smoother": 2}, 2, True, "640x360", "subs"),
    ("subs_kodi", "h264.mkv", {"enabled": "false"}, 1, False, "640x360", "subs"),   # the subtitle check through Kodi alone
    ("h264_x4", "h264.mkv", {"smoother": 4}, 4, True, "640x360", "play"),
    ("h264_x5", "h264.mp4", {"smoother": 5}, 5, True, "640x360", "play"),
    ("h264_x10", "h264.mp4", {"smoother": 10}, 10, True, "640x360", "play"),
    ("auto", "h264.mp4", {"smoother": 0, "upscale": 0, "engine": 0}, 0, True, "*", "play"),
    ("nis", "h264.mp4", {"smoother": 1, "upscale": 3, "engine": 0}, 1, True, "up" if a.gpu else "640x360", "play"),
    ("gpu_x2", "h264.mp4", {"smoother": 2, "upscale": 1, "engine": 0}, 2, True, "640x360", "play"),
    ("cpu_engine", "h264.mp4", {"smoother": 2, "engine": 1, "upscale": 1}, 2, True, "640x360", "play"),
    ("off", "h264.mp4", {"smoother": 1}, 1, True, "640x360", "play"),
    ("upscale2", "h264.mp4", {"smoother": 2, "upscale": 2}, 2, True, "1280x720", "play"),
    ("hevc_x2", "hevc.mkv", {"smoother": 2}, 2, True, "640x360", "play"),
    ("mpeg2_x2", "mpeg2.ts", {"smoother": 2}, 2, True, "640x360", "play"),
    ("hdr10_x2", "hdr10.mkv", {"smoother": 2}, 2, kodi22 and os.name != "nt", "640x360", "play"),
    ("hdr10_kodi", "hdr10.mkv", {"enabled": "false"}, 1, False, "640x360", "play"),   # the same file through Kodi alone
    ("seek", "h264.mkv", {"smoother": 2}, 2, True, "640x360", "seek"),
    ("chapter", "h264.mkv", {"smoother": 2}, 2, True, "640x360", "chapter"),
    ("disabled", "h264.mp4", {"enabled": "false"}, 1, False, "640x360", "play"),
]
if a.cases:
    keep = set(a.cases.split(","))
    CASES = [c for c in CASES if c[0] in keep]

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)

ud = os.path.join(a.home, "userdata")

def kodi_settings():
    # written before every start: Kodi rewrites guisettings.xml when it quits
    write(os.path.join(ud, "advancedsettings.xml"),
          "<advancedsettings version=\"1.0\">\n  <loglevel hide=\"true\">1</loglevel>\n</advancedsettings>\n")
    write(os.path.join(ud, "guisettings.xml"), """<settings version="2">
        <setting id="debug.showloginfo">true</setting>
        <setting id="debug.extralogging">true</setting>
        <setting id="debug.setextraloglevel">32768,262144</setting>
        <setting id="videoplayer.adjustrefreshrate">0</setting>
        <setting id="services.esenabled">true</setting>
        <setting id="filelists.showparentdiritems">true</setting>
    </settings>
    """)

dst = os.path.join(a.home, "addons", "inputstream.pushmpc")
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(a.addon, dst)
print("installed", dst, os.listdir(dst), flush=True)

def find_log():
    c = glob.glob(os.path.join(a.home, "**", "kodi.log"), recursive=True)
    return max(c, key=os.path.getmtime) if c else None

def launch(args, seconds, drive_case=None, drive_out=None):
    cmd = shlex.split(a.kodi, posix=(os.name != "nt")) + args
    print("+", cmd, flush=True)
    log = find_log()
    if log:
        os.remove(log)
    kodi_settings()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    d = None
    if drive_case:
        d = subprocess.Popen([sys.executable, os.path.join(here, "drive.py"), drive_case, str(seconds)],
                             stdout=open(drive_out, "w"), stderr=subprocess.STDOUT)
    try:
        p.wait(timeout=seconds + 100)
    except subprocess.TimeoutExpired:
        print("kodi did not quit: killing", flush=True)
        p.kill()
        p.wait()
    if d:
        try:
            d.wait(timeout=10)
        except subprocess.TimeoutExpired:
            d.kill()
    if a.kill:
        subprocess.run(a.kill, shell=True, check=False)
    time.sleep(2)

# first start: Kodi registers the add-on (disabled, as for any add-on copied in by hand); enable it in the database
launch([], 25, "enable", os.path.join(a.out, "first-start.txt"))
print(open(os.path.join(a.out, "first-start.txt"), encoding="utf-8", errors="replace").read(), flush=True)
dbs = sorted(glob.glob(os.path.join(ud, "Database", "Addons*.db")))
print("databases:", dbs, flush=True)
if dbs:
    con = sqlite3.connect(dbs[-1])
    cur = con.cursor()
    cur.execute("SELECT addonID, enabled FROM installed WHERE addonID='inputstream.pushmpc'")
    row = cur.fetchone()
    print("installed row:", row, flush=True)
    if row:
        cur.execute("UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID='inputstream.pushmpc'")
    else:
        cur.execute("INSERT INTO installed (addonID, enabled, installDate, origin, disabledReason) VALUES "
                    "('inputstream.pushmpc', 1, datetime('now'), '', 0)")
    con.commit()
    con.close()
log = find_log()
if log:
    shutil.copy(log, os.path.join(a.out, "first-start.log"))

failed = []
for name, clip, settings, k, decode, size, drive_case in CASES:
    s = {"enabled": "true", "smoother": 0, "quality": 1, "hdr": "true", "upscale": 0, "engine": 0, "displayhz": 0, "threads": 0}
    if a.gpu:
        s.update({"engine": 1, "upscale": 1})
    s.update(settings)
    write(os.path.join(ud, "addon_data", "inputstream.pushmpc", "settings.xml"),
          "<settings version=\"2\">\n" + "".join(f"    <setting id=\"{k_}\">{v}</setting>\n" for k_, v in s.items()) + "</settings>\n")
    drive_out = os.path.join(a.out, f"{name}.drive.txt")
    launch([os.path.join(a.clips, clip)], a.play, drive_case, drive_out)
    log = find_log()
    lp = os.path.join(a.out, f"{name}.log")
    if log:
        shutil.copy(log, lp)
    else:
        write(lp, "")
    r = subprocess.run([sys.executable, os.path.join(here, "analyze.py"), lp, drive_out, name, str(k), "1" if decode else "0",
                        str(24000 / 1001), size, summary])
    if r.returncode:
        failed.append(name)
# the HDR file through Kodi alone is the reference: when Kodi itself cannot show it on this runner (no 10-bit / HDR path
# in software GL), the add-on's HDR case is not judged
if "hdr10_kodi" in failed:
    failed = [f for f in failed if f not in ("hdr10_kodi", "hdr10_x2")]
    with open(summary, "a", encoding="utf-8") as f:
        f.write("Note: Kodi itself cannot play the HDR10 clip on this runner (hdr10_kodi failed), so hdr10_x2 is not judged here.\n\n")
print("failed cases:", failed or "none")
sys.exit(1 if failed else 0)
