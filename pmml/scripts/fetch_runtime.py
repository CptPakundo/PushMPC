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
import tarfile
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


class Archive:
    """A zip (nupkg, whl) or a tar.gz (tgz) read the same way; unsafe member names are refused."""

    def __init__(self, path: Path):
        self.path = path
        self.tar = path.name.endswith((".tgz", ".tar.gz"))
        self.handle = tarfile.open(path, "r:gz") if self.tar else zipfile.ZipFile(path)
        self.members = []
        for m in (self.handle.getmembers() if self.tar else self.handle.infolist()):
            name = m.name if self.tar else m.filename
            p = PurePosixPath(name)
            if p.is_absolute() or ".." in p.parts:
                raise SystemExit(f"::error::unsafe entry {name} in {path.name}")
            is_file = m.isfile() if self.tar else not m.is_dir()
            if is_file:
                self.members.append((name, m, m.size if self.tar else m.file_size))

    def open(self, member):
        return self.handle.extractfile(member) if self.tar else self.handle.open(member)

    def close(self):
        self.handle.close()


def extract(archive: Path, patterns: list[str], dest: Path, flatten: bool = True, strip: str | None = None) -> list[str]:
    dest.mkdir(parents=True, exist_ok=True)
    got = []
    a = Archive(archive)
    for name, member, _ in a.members:
        if not any(fnmatch.fnmatch(name, pat) or fnmatch.fnmatch(name, pat.replace("**/", "")) for pat in patterns):
            continue
        rel = PurePosixPath(name).name if flatten else (name[len(strip):] if strip and name.startswith(strip) else name)
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        with a.open(member) as src, out.open("wb") as dst:
            shutil.copyfileobj(src, dst)
        got.append(rel)
    a.close()
    return got


def licence_files(archive: Path, dest: Path) -> None:
    a = Archive(archive)
    for name, member, _ in a.members:
        base = PurePosixPath(name).name.lower()
        if base.startswith("license") or "thirdpartynotices" in base or base.startswith("notice"):
            (dest / "licenses").mkdir(parents=True, exist_ok=True)
            with a.open(member) as src, (dest / "licenses" / f"{archive.stem[:40]}-{PurePosixPath(name).name}").open("wb") as dst:
                shutil.copyfileobj(src, dst)
    a.close()


def list_archive(archive: Path) -> None:
    a = Archive(archive)
    for name, _, size in a.members:
        low = name.lower()
        if low.endswith((".dll", ".so", ".dylib", ".h", ".json", ".xml")) or ".so." in low or "license" in low:
            print(f"    {size:>11}  {name}")
    a.close()


def soname_links(dest: Path, files: list[str]) -> list[str]:
    """Wheels ship libfoo.so.13.4.36 without the libfoo.so.13 name their users load: copy it under that name."""
    import re
    added = []
    for f in files:
        m = re.match(r"^(.*\.so\.\d+)\.\d+(\.\d+)*$", f)
        if m and not (dest / m.group(1)).exists():
            shutil.copy2(dest / f, dest / m.group(1))
            added.append(m.group(1))
    return added


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
            for plain, versioned in (("libonnxruntime.so", f.startswith("libonnxruntime.so.")),
                                     ("libonnxruntime.dylib", f.startswith("libonnxruntime.") and f.endswith(".dylib") and f != "libonnxruntime.dylib")):
                if versioned and not (dest / plain).exists():
                    shutil.copy2(dest / f, dest / plain)
                    files.append(plain)
        headers = spec.get("headers") if isinstance(spec, dict) else entry.get("headers")
        if a.headers and headers:
            extract(arc, [headers], dest / "include")
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
        files += soname_links(dest, files)
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
            Archive(arc).close()   # refuses unsafe member names
            with zipfile.ZipFile(arc) as z:
                z.extractall(dest)
            record["ncnn"] = {"version": pin["ncnn"]["version"], "sha256": digest}
            print(f"ok      ncnn {pin['ncnn']['version']} -> {dest}")
        else:
            print(f"skip    ncnn: no {plat} package")

    (out / "fetched.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
