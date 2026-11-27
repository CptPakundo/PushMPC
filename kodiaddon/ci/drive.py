"""Drive a running Kodi over JSON-RPC (TCP 9090, enabled by default for local programs) during a test case:
wait for playback, optionally seek / step a chapter / switch subtitles on, report player properties, then quit.
usage: drive.py <case> <seconds> [clips dir]   (case: play, seek, chapter, subs, browse)"""
import functools, json, socket, sys, time
print = functools.partial(print, flush=True)   # the output survives a kill

def rpc(method, params=None, timeout=5.0):
    """one request; Kodi also pushes notifications (Player.OnPlay ...) on the socket: skip them, return the reply"""
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

PLAYER = [1]

def props():
    r = rpc("Player.GetProperties", {"playerid": PLAYER[0], "properties": ["time", "totaltime", "speed", "currentaudiostream",
            "currentsubtitle", "subtitleenabled"]})
    if r and "error" in r:
        print("DRIVE rpc error", json.dumps(r["error"]))
    return (r or {}).get("result", {})

def ms(t):
    return ((t.get("hours", 0) * 60 + t.get("minutes", 0)) * 60 + t.get("seconds", 0)) * 1000 + t.get("milliseconds", 0)

case, secs = sys.argv[1], float(sys.argv[2])
if case == "enable":
    # first start: enable the add-on the supported way (an add-on copied in by hand is registered disabled)
    deadline = time.time() + 150
    while time.time() < deadline:
        try:
            r = rpc("Addons.SetAddonEnabled", {"addonid": "inputstream.pushmpc", "enabled": True})
            if r is not None:
                print("DRIVE enable", json.dumps(r))
                time.sleep(2)
                print("DRIVE details", json.dumps(rpc("Addons.GetAddonDetails", {"addonid": "inputstream.pushmpc", "properties": ["enabled", "version"]})))
                break
        except OSError:
            pass
        time.sleep(2)
    else:
        print("DRIVE: JSON-RPC never answered")
    try:
        rpc("Application.Quit")
    except OSError:
        pass
    sys.exit(0)
if case == "browse":
    # Kodi's Videos window on the clips folder: it extracts thumbnails and stream details of every clip (its probes)
    path = sys.argv[3].rstrip("/\\") + ("\\" if "\\" in sys.argv[3] else "/")
    deadline = time.time() + 120
    up = False
    while time.time() < deadline and not up:
        try:
            up = bool(rpc("JSONRPC.Ping"))
        except OSError:
            pass
        if not up:
            time.sleep(2)
    print("DRIVE up", up)
    print("DRIVE activate", json.dumps(rpc("GUI.ActivateWindow", {"window": "videos", "parameters": [path]}) if up else None))
    alive = up
    end = time.time() + secs
    while alive and time.time() < end:
        time.sleep(3)
        try:
            alive = (rpc("JSONRPC.Ping") or {}).get("result") == "pong"
        except OSError:
            alive = False
    if alive:
        r = rpc("Files.GetDirectory", {"directory": path, "media": "video", "properties": ["thumbnail", "streamdetails"]}, timeout=15)
        for f in ((r or {}).get("result") or {}).get("files") or []:
            sd = (f.get("streamdetails") or {}).get("video") or [{}]
            print("DRIVE file", f.get("label"), "thumbnail", bool(f.get("thumbnail")), "video", json.dumps(sd[0] if sd else {}))
    print("DRIVE browse_ok", alive)
    try:
        rpc("Application.Quit")
    except OSError:
        pass
    sys.exit(0)
deadline = time.time() + 90
while time.time() < deadline:
    try:
        r = rpc("Player.GetActivePlayers")
        if r and r.get("result"):
            PLAYER[0] = r["result"][0].get("playerid", 1)
            print("DRIVE players", json.dumps(r["result"]))
            break
    except OSError:
        pass
    time.sleep(1)
else:
    print("DRIVE: no active player"); sys.exit(0)
print("DRIVE: playing")
# the clock starts when Kodi shows the first picture (libass font setup can take seconds on a runner)
for _ in range(60):
    if ms(props().get("time", {})) > 300:
        break
    time.sleep(0.5)
time.sleep(min(4.0, secs))
p0 = props(); w0 = time.time(); print("DRIVE props0", json.dumps(p0))
if case == "seek":
    # Kodi's accurate seek decodes from the keyframe before 12 s and drops the pictures before the target: on a slow
    # runner the clock reaches 12 s some seconds later (Kodi 22, run 45942669950: 3.0 s). Wait for it (10 s at most),
    # then require a jump plain playback cannot explain (the clock more than 3 s ahead of where it would be without
    # the seek) inside [12 s, 16.5 s]
    t0 = ms(p0.get("time", {}))
    rpc("Player.Seek", {"playerid": PLAYER[0], "value": {"time": {"hours": 0, "minutes": 0, "seconds": 12, "milliseconds": 0}}})
    ws = time.time()
    while True:
        time.sleep(0.5)
        p1 = props(); t1 = ms(p1.get("time", {}))
        if t1 >= 12000 or time.time() - ws > 10:
            break
    plain = t0 + (time.time() - w0) * 1000.0
    print("DRIVE props1", json.dumps(p1))
    print(f"DRIVE seek {t0} -> {t1} ms, {time.time() - ws:.1f} s after the seek; plain playback would be at {plain:.0f} ms")
    print("DRIVE seek_ok", 12000 <= t1 <= 16500 and t1 > plain + 3000)
elif case == "chapter":
    t0 = ms(p0.get("time", {}))
    rpc("Input.ExecuteAction", {"action": "chapterorbigstepforward"})
    time.sleep(3)
    p1 = props(); print("DRIVE props1", json.dumps(p1))
    print("DRIVE chapter_ok", ms(p1.get("time", {})) >= 9500 and ms(p1.get("time", {})) > t0 + 4000)
elif case == "subs":
    subs = rpc("Player.GetProperties", {"playerid": PLAYER[0], "properties": ["subtitles"]})
    print("DRIVE subtitles", json.dumps(subs))
    for _ in range(5):   # libass may still be loading fonts: ask again until Kodi takes it
        r = rpc("Player.SetSubtitle", {"playerid": PLAYER[0], "subtitle": 0, "enable": True})
        print("DRIVE setsubtitle", json.dumps(r))
        time.sleep(2)
        p1 = props()
        if p1.get("subtitleenabled") and p1.get("currentsubtitle"):
            break
    print("DRIVE props1", json.dumps(p1))
    print("DRIVE subs_ok", bool(p1.get("subtitleenabled")) and bool(p1.get("currentsubtitle")))
    print("DRIVE audio_ok", bool(p1.get("currentaudiostream")))
rest = max(0.0, secs - (time.time() - (deadline - 90)))
time.sleep(min(rest, secs))
print("DRIVE final", json.dumps(props()))
try:
    rpc("Application.Quit")
except OSError:
    pass
