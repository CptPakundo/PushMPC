#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetches pmml's runtimes from their official sources by the pins in scripts/ml-runtime-pin.json.

Used by CI and by the Linux/macOS installers (the Windows installer reads the same pin file). Only native libraries,
headers and licence files are extracted; nothing is executed. Every download is checked against its pinned SHA-256;
an entry pinned as TODO is skipped unless --allow-unpinned is given (CI prints its hash for the pin file).

  python -I fetch_runtime.py --pin scripts/ml-runtime-pin.json --platform win-x64 --out DIR \
      [--flavors core,directml] [--eps trt-rtx,openvino] [--ncnn] [--headers] [--list] [--allow-unpinned]

Layout written:
  DIR/ort/<flavor>/        onnxruntime library (+ DirectML.dll for directml), include/ with --headers
  DIR/eps/<ep>/            the plugin EP's libraries and ep.json {"name", "epName", "library", "version"}
  DIR/ncnn/                the ncnn release package (lib, include) with --ncnn
  DIR/fetched.json         what was fetched, with hashes
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, want: str | None, cache: Path, allow_unpinned: bool) -> tuple[Path, str]:
    name = PurePosixPath(url.split("?")[0]).name
    target = cache / name
    if target.exists() and want and want != "TODO" and sha256(target).lower() == want.lower():
        print(f"cached  {name}")
        return target, want.lower()
    if (not want or want == "TODO") and not allow_unpinned:
        raise SystemExit(f"::error::{name} has no pinned SHA-256 (TODO); rerun with --allow-unpinned to print it")
    print(f"fetch   {url}")
    tmp = target.with_suffix(target.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "pmml-fetch/1"})
    with urllib.request.urlopen(req, timeout=300) as r, tmp.open("wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    digest = sha256(tmp)
    if want and want != "TODO" and digest.lower() != want.lower():
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"::error::{name}: SHA-256 {digest} does not match the pinned {want}")
    tmp.replace(target)
    print(f"sha256  {digest}  {name}" + ("   (UNPINNED)" if not want or want == "TODO" else ""))
    return target, digest


def safe_members(z: zipfile.ZipFile):
    for info in z.infolist():
        p = PurePosixPath(info.filename)
        if p.is_absolute() or ".." in p.parts:
            raise SystemExit(f"::error::unsafe entry {info.filename}")
        yield info


def extract(archive: Path, patterns: list[str], dest: Path, flatten: bool = True, strip: str | None = None) -> list[str]:
    dest.mkdir(parents=True, exist_ok=True)
    got = []
    with zipfile.ZipFile(archive) as z:
        for info in safe_members(z):
            if info.is_dir():
                continue
            name = info.filename
            if not any(fnmatch.fnmatch(name, pat) or fnmatch.fnmatch(name, pat.replace("**/", "")) for pat in patterns):
                continue
            rel = PurePosixPath(name).name if flatten else (name[len(strip):] if strip and name.startswith(strip) else name)
            out = dest / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, out.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            got.append(rel)
    return got


def licence_files(archive: Path, dest: Path) -> None:
    with zipfile.ZipFile(archive) as z:
        for info in safe_members(z):
            base = PurePosixPath(info.filename).name.lower()
            if not info.is_dir() and (base.startswith("license") or "thirdpartynotices" in base or base.startswith("notice")):
                (dest / "licenses").mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, (dest / "licenses" / f"{archive.stem[:40]}-{PurePosixPath(info.filename).name}").open("wb") as dst:
                    shutil.copyfileobj(src, dst)


def list_archive(archive: Path) -> None:
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            low = info.filename.lower()
            if low.endswith((".dll", ".so", ".dylib", ".h", ".json", ".xml")) or ".so." in low or "license" in low:
                print(f"    {info.file_size:>11}  {info.filename}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin", required=True)
    ap.add_argument("--platform", required=True, help="win-x64, win-arm64, linux-x64, linux-arm64, osx-arm64")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache")
    ap.add_argument("--flavors", default="core")
    ap.add_argument("--eps", default="")
    ap.add_argument("--ncnn", action="store_true")
    ap.add_argument("--headers", action="store_true")
    ap.add_argument("--list", action="store_true", help="print the native files of every archive")
    ap.add_argument("--allow-unpinned", action="store_true")
    a = ap.parse_args()
    pin = json.loads(Path(a.pin).read_text(encoding="utf-8"))
    out = Path(a.out).resolve()
    cache = Path(a.cache).resolve() if a.cache else out / "download"
    cache.mkdir(parents=True, exist_ok=True)
    plat = a.platform
    record: dict = {"platform": plat, "ort": {}, "eps": {}, "ncnn": None}

    for flavor in [f for f in a.flavors.split(",") if f]:
        entry = pin["onnxruntime"].get(flavor)
        if not entry:
            raise SystemExit(f"::error::unknown flavor {flavor}")
        spec = entry["platforms"].get(plat)
        if spec is None:
            print(f"skip    ort/{flavor}: no {plat} build")
            continue
        dest = out / "ort" / flavor
        if isinstance(spec, dict):
            arc, digest = download(spec["url"], spec.get("sha256"), cache, a.allow_unpinned)
            pattern = [spec["extract"]]
        else:
            arc, digest = download(entry["url"], entry.get("sha256"), cache, a.allow_unpinned)
            pattern = [spec]
        if a.list:
            list_archive(arc)
        files = extract(arc, pattern, dest)
        licence_files(arc, dest)
        # a versioned library name (wheels: libonnxruntime.so.1.27.1) also under the plain name pmml loads
        for f in list(files):
            if f.startswith("libonnxruntime.so.") and not (dest / "libonnxruntime.so").exists():
                shutil.copy2(dest / f, dest / "libonnxruntime.so")
                files.append("libonnxruntime.so")
        if a.headers and entry.get("headers"):
            extract(arc, [entry["headers"]], dest / "include")
        if flavor == "directml":
            rt = pin["onnxruntime"]["directmlRuntime"]
            rarc, rdig = download(rt["url"], rt["sha256"], cache, a.allow_unpinned)
            files += extract(rarc, [rt["platforms"][plat]], dest)
            licence_files(rarc, dest)
            if a.headers:
                extract(rarc, ["include/*.h"], dest / "include")
        record["ort"][flavor] = {"version": entry["version"], "sha256": digest, "files": files}
        print(f"ok      ort/{flavor} {entry['version']}: {', '.join(files)}")

    for ep in [e for e in a.eps.split(",") if e]:
        entry = pin["eps"].get(ep)
        if not entry:
            raise SystemExit(f"::error::unknown EP {ep}")
        spec = entry["platforms"].get(plat)
        if not spec:
            print(f"skip    eps/{ep}: no {plat} package")
            continue
        arc, digest = download(spec["url"], spec["sha256"], cache, a.allow_unpinned)
        if a.list:
            list_archive(arc)
        dest = out / "eps" / ep
        files = extract(arc, spec.get("extract", entry["extract"]), dest)
        lib = next((f for f in files for pat in entry["library"] if fnmatch.fnmatch(f.lower(), pat.lower()) and
                    f.lower().endswith((".dll", ".so", ".dylib"))), None)
        if not lib:
            lib = next((f for f in files for pat in entry["library"] if fnmatch.fnmatch(f.lower(), pat.lower())), None)
        (dest / "ep.json").write_text(json.dumps({"name": ep, "epName": entry["epName"], "library": lib, "version": entry["version"]}, indent=2), encoding="utf-8")
        record["eps"][ep] = {"version": entry["version"], "sha256": digest, "library": lib, "files": files}
        print(f"ok      eps/{ep} {entry['version']}: library {lib}; {len(files)} files")
        if not lib:
            print(f"::warning::eps/{ep}: no file matched {entry['library']}")

    if a.ncnn:
        key = "osx" if plat.startswith("osx") else plat
        spec = pin["ncnn"]["platforms"].get(key)
        if spec:
            arc, digest = download(spec["url"], spec["sha256"], cache, a.allow_unpinned)
            dest = out / "ncnn"
            shutil.rmtree(dest, ignore_errors=True)
            with zipfile.ZipFile(arc) as z:
                for info in safe_members(z):
                    pass
                z.extractall(dest)
            record["ncnn"] = {"version": pin["ncnn"]["version"], "sha256": digest}
            print(f"ok      ncnn {pin['ncnn']['version']} -> {dest}")
        else:
            print(f"skip    ncnn: no {plat} package")

    (out / "fetched.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
