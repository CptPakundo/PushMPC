#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PushFlow's RIFE models for pmml (the ml backend), out of PushFPS's release zip (pinned), checked against the model
pins in ml-runtime-pin.json. Used by Homebrew's pushmpc-setup (the Linux setup.sh does the same).

    pmml-models.py <models dir> <ml-runtime-pin.json> <download cache>
"""
import hashlib
import json
import pathlib
import sys
import urllib.request
import zipfile

ZIP_URL = "https://github.com/DosukaSOL/PushFPS/releases/download/v4.5.4/PushFPS-4.5.4-win64.zip"
ZIP_SHA = "4bf3fe869eb8a2ca579dd7ade849c0c6cb2bf959f1a434fd53849f8e715f0af5"


def sha(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main() -> int:
    dest, pin, cache = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
    dest.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    models = json.load(open(pin))["models"]["pushflow"]
    need = {models[r]["file"]: models[r]["sha256"] for r in ("fast", "quality")}
    if all((dest / f).exists() and sha(dest / f) == s for f, s in need.items()):
        print("PushFlow's models are in place")
        return 0
    z = cache / ZIP_URL.rsplit("/", 1)[1]
    if not (z.exists() and sha(z) == ZIP_SHA):
        req = urllib.request.Request(ZIP_URL, headers={"User-Agent": "pushmpc-setup/1"})
        with urllib.request.urlopen(req, timeout=600) as r, open(z, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
    if sha(z) != ZIP_SHA:
        print("PushFPS's release zip does not match its pinned SHA-256", file=sys.stderr)
        return 1
    keep = set(need) | {"models.json", "RIFE_LICENSE.txt"} | {f + ".txt" for f in need}
    with zipfile.ZipFile(z) as a:
        for n in a.namelist():
            base = n.replace("\\", "/").rsplit("/", 1)[-1]
            if "/runtime/experimental/pushflow/" in n.replace("\\", "/") and base in keep:
                (dest / base).write_bytes(a.read(n))
    bad = [f for f, s in need.items() if not (dest / f).exists() or sha(dest / f) != s]
    if bad:
        print("missing or not the pinned model: " + ", ".join(bad), file=sys.stderr)
        return 1
    print("PushFlow's models: " + ", ".join(sorted(need)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
