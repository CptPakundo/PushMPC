"""Drive portable Kodi with inputstream.startptsrepro installed: enable it, then ask Kodi for the thumbnail and stream
details of a local .mkv (Videos > Files with "extract thumbnails" on, plus the same request through JSON-RPC and the
web server's /image/ endpoint). Kodi's thumbnail extraction (CDVDFileInfo) calls SeekTime(ms, true) with
startpts == nullptr; the add-on's DemuxSeekTime writes its startpts reference.

usage: addon_repro.py --kodi <kodi.exe> --home <portable_data> --clip <dir/file.mkv> --out <dir>"""
import argparse, glob, json, os, shutil, socket, sqlite3, subprocess, time, urllib.parse, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--kodi", required=True)
ap.add_argument("--home", required=True)
ap.add_argument("--clip", required=True)
ap.add_argument("--out", required=True)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
ud = os.path.join(a.home, "userdata")
ADDON = "inputstream.startptsrepro"
report = []


def say(*x):
    s = " ".join(str(i) for i in x)
    print(s, flush=True)
    report.append(s)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def rpc(method, params=None, timeout=10.0):
    msg = {"jsonrpc": "2.0", "id": 7, "method": method, **({"params": params} if params is not None else {})}
    dec, buf = json.JSONDecoder(), ""
    try:
        with socket.create_connection(("127.0.0.1", 9090), timeout=timeout) as s:
            s.sendall(json.dumps(msg).encode())
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
    except OSError as e:
        return {"oserror": str(e)}


def launch(tag, body):
    for f in glob.glob(os.path.join(a.home, "kodi*.log")):
        os.remove(f)
    write(os.path.join(ud, "advancedsettings.xml"),
          "<advancedsettings version=\"1.0\">\n  <loglevel hide=\"false\">1</loglevel>\n</advancedsettings>\n")
    write(os.path.join(ud, "guisettings.xml"), """<settings version="2">
    <setting id="myvideos.extractthumb">true</setting>
    <setting id="myvideos.extractflags">true</setting>
    <setting id="services.webserver">true</setting>
    <setting id="services.webserverport">8080</setting>
    <setting id="services.webserverauthentication">false</setting>
</settings>
""")
    p = subprocess.Popen([a.kodi, "-p"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    say(f"== {tag}")
    t = time.time() + 150
    while time.time() < t and p.poll() is None and (rpc("JSONRPC.Ping", timeout=3) or {}).get("result") != "pong":
        time.sleep(2)
    if p.poll() is None:
        body(p)
    if p.poll() is None:
        rpc("Application.Quit")
        try:
            p.wait(timeout=40)
        except subprocess.TimeoutExpired:
            say(f"{tag}: Kodi did not quit, killing")
            p.kill()
            p.wait()
    say(f"{tag}: kodi exit code {p.returncode} (0x{p.returncode & 0xffffffff:08X})")
    time.sleep(3)
    for pat in ("kodi.log", "kodi.old.log", "*.dmp", "*crashlog*", "*stacktrace*"):
        for f in glob.glob(os.path.join(a.home, pat)):
            shutil.copy(f, os.path.join(a.out, f"{tag}-{os.path.basename(f)}"))
            say("collected", f"{tag}-{os.path.basename(f)}")


def start1(p):
    say("version", json.dumps(rpc("Application.GetProperties", {"properties": ["version"]})))
    say("enable", json.dumps(rpc("Addons.SetAddonEnabled", {"addonid": ADDON, "enabled": True})))
    time.sleep(2)
    say("addon", json.dumps(rpc("Addons.GetAddonDetails", {"addonid": ADDON, "properties": ["enabled", "version"]})))


def start2(p):
    folder = os.path.dirname(a.clip).replace("\\", "/") + "/"
    say("Videos > Files", folder, json.dumps(rpc("GUI.ActivateWindow", {"window": "videos", "parameters": [folder]})))
    time.sleep(5)
    if p.poll() is None:
        d = rpc("Files.GetFileDetails", {"file": a.clip, "media": "video",
                                         "properties": ["thumbnail", "streamdetails"]}, timeout=30)
        say("Files.GetFileDetails", json.dumps(d))
        thumb = ((d or {}).get("result") or {}).get("filedetails", {}).get("thumbnail", "")
        if thumb and p.poll() is None:
            url = "http://127.0.0.1:8080/image/" + urllib.parse.quote(thumb, safe="")
            try:
                with urllib.request.urlopen(url, timeout=30) as r:
                    say("GET /image/:", r.status, len(r.read()), "bytes")
            except Exception as e:  # noqa: BLE001
                say("GET /image/:", e)
    t = time.time() + 20
    while time.time() < t and p.poll() is None:
        time.sleep(1)
    say("Kodi after the requests:", "DEAD" if p.poll() is not None else "alive")


launch("start1", start1)
for db in glob.glob(os.path.join(ud, "Database", "Addons*.db")):
    con = sqlite3.connect(db)
    say("installed row", con.execute("SELECT addonID, enabled FROM installed WHERE addonID=?", (ADDON,)).fetchall())
    con.execute("UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID=?", (ADDON,))
    con.commit()
    con.close()
launch("start2", start2)
write(os.path.join(a.out, "report.txt"), "\n".join(report) + "\n")
