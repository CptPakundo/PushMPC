#!/usr/bin/env python3
"""Builds and validates the PushMPC IINA plugin package (integrations/iina).

  build.py validate                       check the plugin folder (Info.json, files, preferences keys) without shaders
  build.py build [--cache DIR] [--out DIR] [--allow-unpinned]
                                          fetch the pinned shaders (shaders.lock.json), verify their hashes, lint them
                                          (IINA renders through OpenGL 4.1: no //!COMPUTE), generate shaders.js, check
                                          the JavaScript with `node --check` when node exists, and pack
                                          <out>/PushMPC-<version>.iinaplgz (a zip of the plugin folder's contents, as
                                          `iina-plugin pack` makes it)

A lock entry whose sha256 says TODO-FIRST-RUN is fetched only with --allow-unpinned (CI's first run prints the hash to
pin). Exit code 0 = all good."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(HERE, "PushMPC.iinaplugin")
LOCK = os.path.join(HERE, "shaders.lock.json")
ARTCNN_TOOLS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "vsplugin", "tools")
PERMISSIONS = {"show-osd", "show-alert", "video-overlay", "network-request", "file-system"}
errors = []


def err(msg):
    errors.append(msg)
    print("ERROR: " + msg)


def validate():
    info_path = os.path.join(PLUGIN, "Info.json")
    try:
        info = json.load(open(info_path, encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        err(f"Info.json: {e}")
        return None
    for k, t in (("name", str), ("identifier", str), ("version", str), ("entry", str), ("author", dict)):
        if not isinstance(info.get(k), t):
            err(f"Info.json: required field '{k}' missing or not a {t.__name__}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(info.get("version", ""))):
        err("Info.json: version must be major.minor.patch")
    if not re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+){2,}", str(info.get("identifier", ""))):
        err("Info.json: identifier must be reverse-domain (a.b.c)")
    if not isinstance(info.get("author", {}).get("name"), str):
        err("Info.json: author.name is required")
    for k in ("entry", "globalEntry", "preferencesPage", "helpPage"):
        v = info.get(k)
        if v and not v.startswith("http") and not os.path.isfile(os.path.join(PLUGIN, v)):
            err(f"Info.json: {k} '{v}' does not exist in the plugin folder")
    bad = set(info.get("permissions", [])) - PERMISSIONS
    if bad:
        err(f"Info.json: unknown permissions {sorted(bad)}")
    known = {"name", "identifier", "version", "entry", "author", "description", "globalEntry", "preferencesPage", "preferenceDefaults",
             "helpPage", "subProviders", "sidebarTab", "permissions", "allowedDomains", "ghRepo", "ghVersion", "localized"}
    for k in info:
        if k not in known:
            err(f"Info.json: unknown field '{k}'")
    # every preference the page binds has a default, and every default is bound or used by main.js
    page = open(os.path.join(PLUGIN, info.get("preferencesPage", "preferences.html")), encoding="utf-8").read() if info.get("preferencesPage") else ""
    keys = set(re.findall(r'data-pref-key="([^"]+)"', page))
    defaults = set((info.get("preferenceDefaults") or {}).keys())
    if keys - defaults:
        err(f"preferences page keys without defaults: {sorted(keys - defaults)}")
    main = open(os.path.join(PLUGIN, info["entry"]), encoding="utf-8").read() if info.get("entry") else ""
    for k in defaults:
        if f'"{k}"' not in main:
            err(f"preference '{k}' is never read by {info.get('entry')}")
    # permissions the code needs
    if "core.osd(" in main and "show-osd" not in info.get("permissions", []):
        err("main.js calls core.osd but lacks the show-osd permission")
    if re.search(r"utils\.exec|file\.(write|read|exists)|utils\.resolvePath", main) and "file-system" not in info.get("permissions", []):
        err("main.js uses the file system / exec but lacks the file-system permission")
    print(f"Info.json: {info.get('name')} {info.get('version')} ({info.get('identifier')}), permissions {info.get('permissions')}, preferences {sorted(defaults)}")
    return info


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def fetch(entry, cache, allow_unpinned):
    name = entry["name"]
    path = os.path.join(cache, name)
    data = open(path, "rb").read() if os.path.isfile(path) else None
    def ok(d):
        if "gitBlobSha1" in entry:
            return git_blob_sha1(d) == entry["gitBlobSha1"]
        pin = entry.get("sha256", "")
        return pin.startswith("TODO") or hashlib.sha256(d).hexdigest().upper() == pin.upper()
    if data is None or not ok(data):
        pin = entry.get("sha256", "")
        if "gitBlobSha1" not in entry and pin.startswith("TODO") and not allow_unpinned:
            err(f"{name}: not pinned yet (sha256 {pin}); run with --allow-unpinned once to learn the hash")
            return None
        req = urllib.request.Request(entry["url"], headers={"User-Agent": "pushmpc-ci"})
        data = urllib.request.urlopen(req, timeout=60).read()
        os.makedirs(cache, exist_ok=True)
        open(path, "wb").write(data)
    sha256 = hashlib.sha256(data).hexdigest().upper()
    if "gitBlobSha1" in entry and git_blob_sha1(data) != entry["gitBlobSha1"]:
        err(f"{name}: git blob SHA-1 {git_blob_sha1(data)} != pinned {entry['gitBlobSha1']}")
        return None
    pin = entry.get("sha256", "")
    if pin and not pin.startswith("TODO") and sha256 != pin.upper():
        err(f"{name}: SHA-256 {sha256} != pinned {pin}")
        return None
    if pin.startswith("TODO"):
        print(f"::warning::{name} is not pinned: sha256 {sha256} (put it into shaders.lock.json)")
    print(f"{name}: {len(data)} bytes, sha256 {sha256}")
    return data


def lint(name, text):
    if "//!COMPUTE" in text:
        err(f"{name}: has a compute pass (//!COMPUTE): IINA's OpenGL 4.1 renderer cannot run it")
    if "//!HOOK" not in text:
        err(f"{name}: no //!HOOK: not an mpv user shader")
    passes = text.count("//!HOOK")
    when = text.count("//!WHEN")
    print(f"  {name}: {passes} passes, {when} WHEN conditions{'' if when else ' (runs at every size)'}")


def build(cache, out, allow_unpinned):
    info = validate()
    if errors:
        return
    lock = json.load(open(LOCK, encoding="utf-8"))
    files = {}
    licences = []
    for e in lock["shaders"]:
        data = fetch(e, cache, allow_unpinned)
        if data is None:
            continue
        text = data.decode("utf-8")
        lint(e["name"], text)
        files[e["name"]] = text
        licences.append(f"{e['name']}: {e['what']}. Licence: {e['licence']}. Source: {e['source']} ({e['url']}). The full licence text is in the file's header.")
    # ArtCNN (MIT) as fragment passes, generated from the pinned ONNX files (vsplugin/models/artcnn.lock.json):
    # the upstream GLSL is compute-only, which IINA's OpenGL 4.1 cannot run
    sys.path.insert(0, ARTCNN_TOOLS)
    import artcnn_tool  # noqa: E402
    try:
        artcnn_tool.fetch(artcnn_tool.DEFAULT_LOCK, cache, ["ArtCNN_C4F16.onnx", "ArtCNN_C4F32.onnx"])
        alock = json.load(open(artcnn_tool.DEFAULT_LOCK, encoding="utf-8"))
        for m in ("C4F16", "C4F32"):
            name = f"ArtCNN_{m}_fragment.glsl"
            dst = os.path.join(cache, name)
            n = artcnn_tool.write_glsl(artcnn_tool.layers_from_onnx(os.path.join(cache, f"ArtCNN_{m}.onnx")), dst, f"ArtCNN {m}")
            text = open(dst, encoding="utf-8").read()
            lint(name, text)
            files[name] = text
            licences.append(f"{name}: ArtCNN {m} luma doubler ({n} fragment passes, generated by PushMPC's artcnn_tool.py from the upstream ONNX "
                            f"weights at commit {alock['commit']}). Licence: MIT ({alock['copyright']}). Source: {alock['repository']}. The licence text is in the file's header.")
    except (OSError, SystemExit, ValueError) as e:
        err(f"ArtCNN fragment shaders: {e}")
    for q, p in lock["presets"].items():
        for content in ("live", "anime"):
            for n in p[content]:
                if n not in files:
                    err(f"preset {q}/{content} names {n}, which is not bundled")
    if errors:
        return
    stage = os.path.join(out, "stage", "PushMPC.iinaplugin")
    shutil.rmtree(os.path.dirname(stage), ignore_errors=True)
    shutil.copytree(PLUGIN, stage, ignore=shutil.ignore_patterns(".*"))
    bundle = {"version": info["version"] + "+" + hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()[:12],
              "presets": {q: {"live": p["live"], "anime": p["anime"]} for q, p in lock["presets"].items()}, "files": files}
    with open(os.path.join(stage, "shaders.js"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// generated by integrations/iina/build.py from shaders.lock.json: the bundled mpv user shaders (licences in SHADER-LICENSES.txt)\n")
        f.write("module.exports = " + json.dumps(bundle, ensure_ascii=False) + ";\n")
    with open(os.path.join(stage, "SHADER-LICENSES.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Shaders bundled with the PushMPC IINA plugin (PushMPC itself: GPL-3.0)\n\n" + "\n\n".join(licences) + "\n")
    node = shutil.which("node")
    if node:
        for js in ("main.js", "shaders.js"):
            r = subprocess.run([node, "--check", os.path.join(stage, js)], capture_output=True, text=True)
            if r.returncode:
                err(f"node --check {js}: {r.stderr.strip()}")
            else:
                print(f"node --check {js}: ok")
        # the bundle loads as IINA loads a module, and every preset resolves
        probe = ("const module = {}; " + open(os.path.join(stage, "shaders.js"), encoding="utf-8").read() +
                 "\nconst b = module.exports; for (const q in b.presets) for (const c of ['live','anime']) for (const n of b.presets[q][c]) if (!b.files[n]) throw new Error(n);"
                 "\nconsole.log('shaders.js: ' + Object.keys(b.files).length + ' shaders, presets ' + Object.keys(b.presets).join(','));")
        probe_js = os.path.join(out, "stage", "probe.js")
        open(probe_js, "w", encoding="utf-8").write(probe)
        r = subprocess.run([node, probe_js], capture_output=True, text=True)
        if r.returncode:
            err("shaders.js does not load as a module: " + r.stderr.strip())
        else:
            print(r.stdout.strip())
    else:
        print("node not found: JavaScript syntax not checked")
    if errors:
        return
    pkg = os.path.join(out, f"PushMPC-{info['version']}.iinaplgz")
    with zipfile.ZipFile(pkg, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, names in os.walk(stage):
            for n in sorted(names):
                full = os.path.join(root, n)
                z.write(full, os.path.relpath(full, stage).replace(os.sep, "/"))
    with zipfile.ZipFile(pkg) as z:
        names = z.namelist()
        if "Info.json" not in names or info["entry"] not in names or "shaders.js" not in names:
            err(f"{pkg}: Info.json, {info['entry']} or shaders.js missing at the archive root")
        json.loads(z.read("Info.json"))
    print(f"package: {pkg} ({os.path.getsize(pkg)} bytes, {len(names)} files: {', '.join(names)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["validate", "build"])
    ap.add_argument("--cache", default=os.path.join(HERE, ".cache"))
    ap.add_argument("--out", default=os.path.join(HERE, "dist"))
    ap.add_argument("--allow-unpinned", action="store_true")
    a = ap.parse_args()
    if a.action == "validate":
        validate()
    else:
        build(a.cache, a.out, a.allow_unpinned)
    print("iina plugin: " + ("FAILED, %d errors" % len(errors) if errors else "ok"))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
