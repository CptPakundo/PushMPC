"""Stock-only reproduction for a Kodi bug: CDVDDemux::SeekTime(time, backwards) with startpts == nullptr reaches an
inputstream add-on (kodi-dev-kit ADDON_DemuxSeekTime dereferences *startpts), here inputstream.ffmpegdirect.

Stock Kodi (portable), stock inputstream.ffmpegdirect (bundled with the Windows build), a self-generated test clip.
  start 1: enable inputstream.ffmpegdirect, play the clip normally to its end (the file gets a row in MyVideos*.db)
  start 2: play the same file through an .m3u with #KODIPROP:inputstream=inputstream.ffmpegdirect; with
           --geometry, videoscreen.extractcontentgeometry + videoscreen.contentgeometryonscan are on (Kodi master),
           so Kodi measures the file before playback through the item's inputstream add-on
           (MeasureContentGeometryBeforePlayback -> OpenVideoDecodeSession -> SeekAndDecodePictureAt -> SeekTime(ms, true))

usage: startpts_repro.py --kodi <kodi.exe> --home <portable_data> --clip <file> --out <dir> [--geometry]"""
import argparse, glob, json, os, shutil, socket, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--kodi", required=True)
ap.add_argument("--home", required=True)
ap.add_argument("--clip", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--geometry", action="store_true")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
ud = os.path.join(a.home, "userdata")
ADDON = "inputstream.ffmpegdirect"
report = []


def say(*x):
    s = " ".join(str(i) for i in x)
    print(s, flush=True)
    report.append(s)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def rpc(method, params=None, timeout=5.0):
    msg = {"jsonrpc": "2.0", "id": 7, "method": method}
    if params is not None:
        msg["params"] = params
    dec = json.JSONDecoder()
    with socket.create_connection(("127.0.0.1", 9090), timeout=timeout) as s:
        s.sendall(json.dumps(msg).encode())
        s.settimeout(timeout)
        buf = ""
        while True:
            c = s.recv(65536)
            if not c:
                return None
            buf += c.decode("utf-8", "replace")
            while buf.strip():
                buf = buf.lstrip()
                try:
                    obj, end = dec.raw_decode(buf)
                except ValueError:
                    break
                buf = buf[end:]
                if isinstance(obj, dict) and obj.get("id") == 7:
                    return obj


def try_rpc(method, params=None, timeout=5.0):
    try:
        return rpc(method, params, timeout)
    except OSError as e:
        return {"oserror": str(e)}


def settings(geometry):
    write(os.path.join(ud, "advancedsettings.xml"),
          "<advancedsettings version=\"1.0\">\n  <loglevel hide=\"false\">1</loglevel>\n</advancedsettings>\n")
    g = "true" if geometry else "false"
    write(os.path.join(ud, "guisettings.xml"), f"""<settings version="2">
    <setting id="debug.showloginfo">true</setting>
    <setting id="videoplayer.adjustrefreshrate">0</setting>
    <setting id="myvideos.extractthumb">true</setting>
    <setting id="myvideos.extractflags">true</setting>
    <setting id="videoscreen.extractcontentgeometry">{g}</setting>
    <setting id="videoscreen.contentgeometryonscan">{g}</setting>
</settings>
""")


def wait_rpc(p, secs=150):
    t = time.time() + secs
    while time.time() < t and p.poll() is None:
        r = try_rpc("JSONRPC.Ping")
        if r and r.get("result") == "pong":
            return True
        time.sleep(2)
    return False


def collect(tag):
    for pat in ("kodi.log", "kodi.old.log", "*.dmp", "*crashlog*", "*stacktrace*"):
        for f in glob.glob(os.path.join(a.home, "**", pat), recursive=True):
            dst = os.path.join(a.out, f"{tag}-{os.path.basename(f)}")
            shutil.copy(f, dst)
            say("collected", dst)


def launch(tag, geometry, body):
    for f in glob.glob(os.path.join(a.home, "**", "kodi*.log"), recursive=True):
        os.remove(f)
    settings(geometry)
    p = subprocess.Popen([a.kodi, "-p"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    say(f"== {tag}: kodi pid {p.pid}, geometry={geometry}")
    if not wait_rpc(p):
        say(f"{tag}: JSON-RPC never answered, exit code {p.poll()}")
    else:
        body(p)
    if p.poll() is None:
        try_rpc("Application.Quit")
        try:
            p.wait(timeout=40)
        except subprocess.TimeoutExpired:
            say(f"{tag}: Kodi did not quit, killing")
            p.kill()
            p.wait()
    rc = p.returncode
    say(f"{tag}: kodi exit code {rc} (0x{rc & 0xffffffff:08X})")
    time.sleep(3)
    collect(tag)
    return rc


def wait_idle(p, secs):
    """wait until no player is active (clip ended) or the process died"""
    t = time.time() + secs
    seen = False
    while time.time() < t:
        if p.poll() is not None:
            return "died"
        r = try_rpc("Player.GetActivePlayers")
        act = (r or {}).get("result")
        if act:
            seen = True
        elif seen:
            return "ended"
        time.sleep(1)
    return "timeout" if seen else "never-played"


def start1(p):
    say("version", json.dumps(try_rpc("Application.GetProperties", {"properties": ["version", "name"]})))
    say("enable", json.dumps(try_rpc("Addons.SetAddonEnabled", {"addonid": ADDON, "enabled": True})))
    time.sleep(2)
    say("addon", json.dumps(try_rpc("Addons.GetAddonDetails", {"addonid": ADDON,
                                                               "properties": ["enabled", "version", "path"]})))
    say("setting extractcontentgeometry", json.dumps(try_rpc("Settings.GetSettingValue",
                                                             {"setting": "videoscreen.extractcontentgeometry"})))
    say("play plain", json.dumps(try_rpc("Player.Open", {"item": {"file": a.clip}})))
    say("plain playback:", wait_idle(p, 60))
    time.sleep(3)
    # the single-entry playlist is played as one item whose path is the .m3u (dynpath = the clip): play it to its end
    # once too, so the .m3u has a row in the files table (the measurement before playback needs one)
    say("play m3u (no measurement yet)", json.dumps(try_rpc("Player.Open", {"item": {"file": m3u}})))
    say("m3u playback:", wait_idle(p, 60))
    time.sleep(3)


def start2(p):
    say("setting extractcontentgeometry", json.dumps(try_rpc("Settings.GetSettingValue",
                                                             {"setting": "videoscreen.extractcontentgeometry"})))
    say("setting contentgeometryonscan", json.dumps(try_rpc("Settings.GetSettingValue",
                                                            {"setting": "videoscreen.contentgeometryonscan"})))
    if a.geometry:
        # the start-up sweep measures the file through Kodi's own demuxer (the plain path); wait for it, then change
        # the file's modification time so the stored measurement no longer matches and the measurement before
        # playback runs on the playlist item (which carries #KODIPROP:inputstream)
        log = os.path.join(a.home, "kodi.log")
        t = time.time() + 30
        while time.time() < t:
            try:
                if "CVideoLibraryContentGeometryJob::Work" in open(log, encoding="utf-8", errors="replace").read():
                    say("start-up geometry sweep finished")
                    break
            except OSError:
                pass
            time.sleep(1)
        time.sleep(2)
        now = time.time() + 120
        os.utime(a.clip, (now, now))
        say("touched", a.clip)
    say("play m3u", json.dumps(try_rpc("Player.Open", {"item": {"file": m3u}}, timeout=60)))
    t = time.time() + 45
    seeked = False
    while time.time() < t and p.poll() is None:
        r = try_rpc("Player.GetActivePlayers")
        if (r or {}).get("result") and not seeked:
            time.sleep(2)
            say("player", json.dumps(try_rpc("Player.GetItem", {"playerid": 1, "properties": ["file"]})))
            say("seek (player path, startpts non-null)", json.dumps(try_rpc("Player.Seek", {"playerid": 1, "value": {"percentage": 50}})))
            seeked = True
        time.sleep(1)
    say("start 2 ended with kodi", "DEAD" if p.poll() is not None else "alive")


m3u = os.path.join(os.path.dirname(a.clip), "ffmpegdirect.m3u")
write(m3u, f"#EXTM3U\n#KODIPROP:inputstream={ADDON}\n#EXTINF:-1,ffmpegdirect startpts test\n{a.clip}\n")
say("m3u:", open(m3u, encoding="utf-8").read())

launch("start1", False, start1)
import sqlite3
adbs = glob.glob(os.path.join(ud, "Database", "Addons*.db"))
if adbs:
    # an add-on unpacked by hand is registered disabled; Addons.SetAddonEnabled above should have enabled it already
    con = sqlite3.connect(max(adbs))
    say("installed row", con.execute("SELECT addonID, enabled, disabledReason FROM installed WHERE addonID=?",
                                     (ADDON,)).fetchall())
    con.execute("UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID=?", (ADDON,))
    con.commit()
    con.close()
dbs = glob.glob(os.path.join(ud, "Database", "MyVideos*.db"))
if dbs:
    con = sqlite3.connect(max(dbs))
    say("files rows", con.execute("SELECT idFile, strFilename, playCount FROM files").fetchall())
    con.close()
rc = launch("start2", a.geometry, start2)
write(os.path.join(a.out, "report.txt"), "\n".join(report) + "\n")
