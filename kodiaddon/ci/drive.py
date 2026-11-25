"""Drive a running Kodi over JSON-RPC (TCP 9090, enabled by default for local programs) during a test case:
wait for playback, optionally seek / step a chapter / switch subtitles on, report player properties, then quit.
usage: drive.py <case> <seconds>   (case: play, seek, chapter, subs)"""
import json, socket, sys, time

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

def props():
    r = rpc("Player.GetProperties", {"playerid": 1, "properties": ["time", "totaltime", "speed", "currentaudiostream",
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
deadline = time.time() + 90
while time.time() < deadline:
    try:
        r = rpc("Player.GetActivePlayers")
        if r and r.get("result"):
            break
    except OSError:
        pass
    time.sleep(1)
else:
    print("DRIVE: no active player"); sys.exit(0)
print("DRIVE: playing")
time.sleep(min(4.0, secs))
p0 = props(); print("DRIVE props0", json.dumps(p0))
if case == "seek":
    rpc("Player.Seek", {"playerid": 1, "value": {"time": {"hours": 0, "minutes": 0, "seconds": 12, "milliseconds": 0}}})
    time.sleep(3)
    p1 = props(); print("DRIVE props1", json.dumps(p1))
    print("DRIVE seek_ok", 12000 <= ms(p1.get("time", {})) <= 16500)
elif case == "chapter":
    t0 = ms(p0.get("time", {}))
    rpc("Input.ExecuteAction", {"action": "chapterorbigstepforward"})
    time.sleep(3)
    p1 = props(); print("DRIVE props1", json.dumps(p1))
    print("DRIVE chapter_ok", ms(p1.get("time", {})) >= 10000 and ms(p1.get("time", {})) > t0 + 4000)
elif case == "subs":
    rpc("Player.SetSubtitle", {"playerid": 1, "subtitle": "on"})
    time.sleep(2)
    p1 = props(); print("DRIVE props1", json.dumps(p1))
    print("DRIVE subs_ok", bool(p1.get("subtitleenabled")) and bool(p1.get("currentsubtitle")))
    print("DRIVE audio_ok", bool(p1.get("currentaudiostream")))
rest = max(0.0, secs - (time.time() - (deadline - 90)))
time.sleep(min(rest, secs))
print("DRIVE final", json.dumps(props()))
try:
    rpc("Application.Quit")
except OSError:
    pass
