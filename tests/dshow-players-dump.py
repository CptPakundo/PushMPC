#!/usr/bin/env python3
"""dshow-players-dump.py <out dir> <player> <exe>: learns where a DirectShow player keeps its external-filter list.

Runs on a hosted Windows runner (the integrations job dshow-players), with PushMPC Filter registered. For one player
(kmplayer64x, kmplayer, gom, zoom) it snapshots HKCU\\Software and the player's files, starts the player, drives its
"add external filter" dialog with pywinauto (best effort: every step is logged with a screenshot, a missing control
does not stop the rest), closes the player so it saves, snapshots again and writes the differences:
  <out>/<player>/registry.diff   changed lines of `reg export HKCU\\Software`
  <out>/<player>/files.diff      added / changed / removed files (size, sha1)
  <out>/<player>/changed/        copies of changed settings files (ini, xml, cfg, ...)
  <out>/<player>/steps.txt, step-*.png
  <out>/<player>/found.txt       registry values and files that now name PushMPC Filter (its CLSID or name)
Requires: pip install pywinauto pillow.
"""
import difflib, hashlib, os, re, shutil, subprocess, sys, time
from pathlib import Path

CLSID = "798D4EC3-E59C-4A4E-A2EE-C804AEAE04CC"
PATTERN = re.compile(CLSID + "|PushMPC Filter", re.I)

# The dialog paths, from the players' own documentation and SVP's guides (KMPlayer 64X has MPC-HC's settings window;
# 32-bit KMPlayer has PotPlayer's; GOM: Preferences > Filter > Advanced Settings; Zoom Player: Options > Smart Play).
# Steps: ("key", keys) | ("click", regex of a control's name) | ("pick", regex: select a list / tree item) | ("wait", s)
STEPS = {
    "kmplayer64x": [("key", "o"), ("wait", 2), ("pick", r"^External Filters$"), ("click", r"^Add Filter"), ("wait", 2),
                    ("pick", r"PushMPC Filter"), ("click", r"^OK$"), ("wait", 1), ("click", r"^Prefer$"), ("click", r"^OK$")],
    "kmplayer": [("key", "{F2}"), ("wait", 2), ("pick", r"Filter Control"), ("pick", r"Filter Priority|External"),
                 ("click", r"Add registered filter|Add Filter|Add"), ("wait", 2), ("pick", r"PushMPC Filter"), ("click", r"^OK$"),
                 ("pick", r"^Prefer"), ("click", r"^Apply$"), ("click", r"^OK$|^Close$")],
    "gom": [("key", "{F5}"), ("wait", 2), ("pick", r"^Filter"), ("pick", r"Advanced"), ("pick", r"^Advanced$"),
            ("click", r"Add Filter"), ("wait", 2), ("pick", r"PushMPC Filter"), ("click", r"^OK$"), ("pick", r"^Highest$"),
            ("click", r"^OK$")],
    "zoom": [("key", "o"), ("wait", 2), ("pick", r"Playback"), ("pick", r"Smart Play"), ("click", r"Video|Decoder|Edit"),
             ("wait", 2), ("pick", r"H\.?264|AVC"), ("click", r"Edit|Add|Chain"), ("wait", 2), ("pick", r"PushMPC Filter"),
             ("click", r"^OK$|^Add$"), ("click", r"^OK$|^Save$|^Close$")],
}


def reg_export(path: Path):
    subprocess.run(["reg", "export", r"HKCU\Software", str(path), "/y"], capture_output=True)
    try:
        return path.read_text(encoding="utf-16").splitlines()
    except Exception:
        return []


def manifest(roots):
    out = {}
    for r in roots:
        r = Path(r)
        if not r.exists():
            continue
        for p in r.rglob("*"):
            try:
                if p.is_file() and p.stat().st_size < 64 * 1024 * 1024:
                    out[str(p)] = (p.stat().st_size, hashlib.sha1(p.read_bytes()).hexdigest())
            except OSError:
                pass
    return out


def roots_for(exe: Path):
    appdata, local = os.environ.get("APPDATA", ""), os.environ.get("LOCALAPPDATA", "")
    roots = [exe.parent]
    for base in (appdata, local):
        if base and Path(base).exists():
            roots += [d for d in Path(base).iterdir() if d.is_dir() and re.search(r"km|gom|gretech|zoom|inmatrix|pandora", d.name, re.I)]
    return roots


def drive(player: str, exe: Path, out: Path):
    from pywinauto import Application, Desktop
    from PIL import ImageGrab
    log = (out / "steps.txt").open("w", encoding="utf-8")
    app = Application(backend="uia").start(f'"{exe}"', timeout=30)
    time.sleep(10)
    pid = app.process

    def windows():
        return [w for w in Desktop(backend="uia").windows() if w.process_id() == pid]

    def find(rx):
        r = re.compile(rx, re.I)
        for w in windows():
            for c in [w] + w.descendants():
                try:
                    if r.search(c.window_text() or ""):
                        return c
                except Exception:
                    pass
        return None

    for i, (kind, arg) in enumerate(STEPS[player]):
        ok = False
        try:
            if kind == "wait":
                time.sleep(arg); ok = True
            elif kind == "key":
                w = windows()
                if w:
                    w[0].set_focus(); w[0].type_keys(arg); ok = True
            else:
                c = find(arg)
                if c is not None:
                    if kind == "pick":
                        for how in ("select", "click_input"):
                            try:
                                getattr(c, how)(); ok = True; break
                            except Exception:
                                pass
                    else:
                        c.click_input(); ok = True
        except Exception as e:
            log.write(f"step {i} {kind} {arg!r}: exception {e}\n")
        log.write(f"step {i} {kind} {arg!r}: {'ok' if ok else 'not found'}\n")
        try:
            ImageGrab.grab().save(out / f"step-{i:02d}.png")
        except Exception:
            pass
        time.sleep(1)
    log.write("windows at the end: " + " | ".join(w.window_text() for w in windows()) + "\n")
    for w in reversed(windows()):   # dialogs first, then the main window: the player saves its settings on close
        try:
            w.close()
        except Exception:
            pass
        time.sleep(2)
    try:
        app.wait_for_process_exit(timeout=20)
    except Exception:
        app.kill()
    log.close()


def main():
    out_root, player, exe = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    out = out_root / player
    (out / "changed").mkdir(parents=True, exist_ok=True)
    roots = roots_for(exe)
    reg0, man0 = reg_export(out / "before.reg"), manifest(roots)
    try:
        drive(player, exe, out)
    except Exception as e:
        (out / "steps.txt").open("a", encoding="utf-8").write(f"driver failed: {e}\n")
    time.sleep(3)
    roots = roots_for(exe)   # a settings folder may only exist now
    reg1, man1 = reg_export(out / "after.reg"), manifest(roots)
    (out / "registry.diff").write_text("\n".join(l for l in difflib.unified_diff(reg0, reg1, "before", "after", n=2, lineterm="")), encoding="utf-8")
    lines, found = [], []
    for p in sorted(set(man0) | set(man1)):
        a, b = man0.get(p), man1.get(p)
        if a == b:
            continue
        lines.append(f"{'added' if not a else 'removed' if not b else 'changed'} {p} {b or a}")
        if b and Path(p).stat().st_size < 4 * 1024 * 1024:
            dst = out / "changed" / re.sub(r"[:\\/]+", "_", p)
            shutil.copy2(p, dst)
            data = Path(p).read_bytes()
            if PATTERN.search(data.decode("utf-16", "ignore")) or PATTERN.search(data.decode("latin-1")):
                found.append(f"file {p}")
    (out / "files.diff").write_text("\n".join(lines), encoding="utf-8")
    before = set(reg0)
    found += [f"registry {l.strip()}" for l in reg1 if PATTERN.search(l) and l not in before]
    (out / "found.txt").write_text("\n".join(found) if found else "PushMPC Filter not found in the changes", encoding="utf-8")
    print(f"{player}: {len(lines)} file changes, registry diff {sum(1 for _ in reg1) - sum(1 for _ in reg0):+d} lines; "
          f"PushMPC entry {'FOUND' if found else 'not found'}")


if __name__ == "__main__":
    main()
