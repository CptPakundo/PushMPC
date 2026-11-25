#!/usr/bin/env python3
# nvidia-vfx-wheel.py: CI helper for NVIDIA's `nvidia-vfx` wheels (pypi.nvidia.com). Standard library only.
# Used by .github/workflows/nvidia-vfx.yml on GitHub's runners; never run on a user's machine (the Linux installer uses
# scripts/nvidia-vfx-fetch.sh, the Windows installer scripts/pushmpc-install.ps1).
#
#   fetch    <platform> <out-dir>          download the pinned wheel of scripts/nvidia-vfx-pin.json, check its SHA-256
#                                          against the pin AND the index's published hash, print the path
#   index    <version>                     the index's wheel names and SHA-256 (the pin is checked against it)
#   list     <wheel> [--json out.json]     every member: name, size, SHA-256 (the pin's per-file list is made from it)
#   extract  <wheel> <dir> [--all]         the pinned files only (or everything) into <dir>, flattened as the pin says
#   headers  <wheel>                       the #define selector strings and enums of the SDK headers in the wheel
#   verify   <platform> <dir>              the extracted files against the pin's per-file SHA-256 / size
#   compare  <wheel> <pushmpc-install.ps1> the Windows wheel's VFX files against $RuntimeFileHashesNvidia (markdown)
#   pe       <dir> <x64|ARM64>               every DLL's machine type and imports (the ARM64 wheel's structure check)
import hashlib, json, os, re, sys, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PIN = os.path.join(HERE, "nvidia-vfx-pin.json")
INDEX = "https://pypi.nvidia.com/nvidia-vfx/"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest().upper()


def load_pin():
    with open(PIN, encoding="utf-8") as f:
        return json.load(f)


def index_hashes():
    req = urllib.request.Request(INDEX, headers={"User-Agent": "pushmpc-ci"})
    html = urllib.request.urlopen(req, timeout=60).read().decode("utf-8", "replace")
    return {m.group(1): m.group(2).upper() for m in re.finditer(r'href="([^"#]+\.whl)#sha256=([0-9a-fA-F]{64})"', html)}


def cmd_index(version):
    for name, sha in sorted(index_hashes().items()):
        if f"-{version}-" in name:
            print(sha, name)


def cmd_fetch(platform, out):
    pin = load_pin()
    w = pin["wheels"][platform]
    idx = index_hashes()
    if idx.get(w["wheel"]) != w["sha256"].upper():
        sys.exit(f"index hash for {w['wheel']} is {idx.get(w['wheel'])}, the pin says {w['sha256']}: refusing")
    os.makedirs(out, exist_ok=True)
    dest = os.path.join(out, w["wheel"])
    if not (os.path.exists(dest) and sha256_file(dest) == w["sha256"].upper()):
        req = urllib.request.Request(w["url"], headers={"User-Agent": "pushmpc-ci"})
        with urllib.request.urlopen(req, timeout=600) as r, open(dest + ".part", "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
        os.replace(dest + ".part", dest)
    got = sha256_file(dest)
    if got != w["sha256"].upper():
        os.remove(dest)
        sys.exit(f"SHA-256 mismatch for {w['wheel']}: {got}")
    print(dest)


def members(wheel):
    out = []
    with zipfile.ZipFile(wheel) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            h = hashlib.sha256()
            with z.open(i) as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    h.update(b)
            out.append({"name": i.filename, "size": i.file_size, "sha256": h.hexdigest().upper()})
    return out


def cmd_list(wheel, rest):
    m = members(wheel)
    for e in m:
        print(f"{e['sha256']} {e['size']:>12} {e['name']}")
    if "--json" in rest:
        with open(rest[rest.index("--json") + 1], "w", encoding="utf-8") as f:
            json.dump(m, f, indent=1)


def pinned_files(platform):
    return load_pin()["wheels"][platform].get("files", [])


def cmd_extract(wheel, out, rest):
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(wheel) as z:
        if "--all" in rest:
            z.extractall(out)
            return
        for e in pinned_files(rest[0] if rest and rest[0] != "--all" else "linux_x86_64"):
            dest = os.path.join(out, e["to"])
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(e["name"]) as s, open(dest, "wb") as d:
                while True:
                    b = s.read(1 << 20)
                    if not b:
                        break
                    d.write(b)
            print("extracted", e["to"])


def cmd_headers(wheel):
    with zipfile.ZipFile(wheel) as z:
        for i in z.infolist():
            if not i.filename.lower().endswith((".h", ".hpp")):
                continue
            text = z.read(i).decode("utf-8", "replace")
            print(f"===== {i.filename} ({i.file_size} bytes)")
            for line in text.splitlines():
                s = line.strip()
                if s.startswith("#define NVVFX") or s.startswith("#define NVCV_") or re.match(r"^NVCV_\w+\s*=", s) or "NvCV_Status NvCV_API" in s \
                        or "NvVFX_API" in s or s.startswith("typedef") or re.match(r"^(unsigned|int|void|float|const|NvCV)\s", s) and ";" in s and "(" not in s[:3]:
                    print("  " + s)


def cmd_verify(platform, d):
    bad = 0
    for e in pinned_files(platform):
        p = os.path.join(d, e["to"])
        if not os.path.exists(p):
            print("MISSING", e["to"]); bad += 1; continue
        s, h = os.path.getsize(p), sha256_file(p)
        ok = s == e["size"] and h == e["sha256"].upper()
        print("OK  " if ok else "BAD ", e["to"], s, h)
        bad += 0 if ok else 1
    sys.exit(1 if bad else 0)


def cmd_compare(wheel, ps1):
    text = open(ps1, encoding="utf-8-sig").read()
    block = text[text.index("$RuntimeFileHashesNvidia = @{"):]
    block = block[: block.index("\n}")]
    pins = {}
    for m in re.finditer(r"'([^']+)'\s*=\s*'([0-9A-F]{64}) (\d+)'", block):
        pins[m.group(1).lower()] = (m.group(1), m.group(2), int(m.group(3)))   # keyed by the full relative path
    base = lambda k: k.replace("\\", "/").split("/")[-1]
    wheelfiles = {}
    for e in members(wheel):
        wheelfiles.setdefault(os.path.basename(e["name"]).lower(), []).append(e)
    rows = []
    vfx = [k for k in pins if "nvidia-vfg" in k or base(k) in ("nvngx_truehdr.dll", "nvngx_vsr.dll")]
    print("| PushMPC pin (Veyra 2.2.1 package) | pinned SHA-256 / size | wheel member | wheel SHA-256 / size | result |")
    print("|---|---|---|---|---|")
    matched = 0
    for k in sorted(vfx):
        rel, sha, size = pins[k]
        cands = wheelfiles.get(base(k), [])
        if not cands:
            print(f"| `{rel}` | {sha[:12]}… / {size:,} | (none) | – | **absent from the wheel** |"); rows.append((rel, "absent")); continue
        best = next((c for c in cands if c["sha256"] == sha), cands[0])
        res = "**identical**" if best["sha256"] == sha and best["size"] == size else ("size equal, hash differs" if best["size"] == size else "differs")
        matched += res == "**identical**"
        print(f"| `{rel}` | {sha[:12]}… / {size:,} | `{best['name']}` | {best['sha256'][:12]}… / {best['size']:,} | {res} |")
        rows.append((rel, res))
    print(f"\n{matched} of {len(vfx)} pinned VFX files are byte-identical in {os.path.basename(wheel)}")
    # the full hashes, for the record
    print("\nfull hashes:")
    for k in sorted(vfx):
        rel, sha, size = pins[k]
        for c in wheelfiles.get(base(k), []):
            print(f"  {rel}: pin {sha} {size} | wheel {c['name']} {c['sha256']} {c['size']}")


PE_MACHINES = {0x8664: "x64", 0xAA64: "ARM64", 0xA641: "ARM64EC", 0x14C: "x86"}


def pe_info(path):
    """machine and imported DLLs (with the CUDA driver functions) of a PE file, from its headers only"""
    import struct
    b = open(path, "rb").read()
    if b[:2] != b"MZ":
        return None
    nt = struct.unpack_from("<I", b, 0x3C)[0]
    if b[nt:nt + 4] != b"PE\0\0":
        return None
    machine, nsec = struct.unpack_from("<HH", b, nt + 4)
    optsize = struct.unpack_from("<H", b, nt + 20)[0]
    opt = nt + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", b, ddir + 8)[0]
    secs = [struct.unpack_from("<8sIIII", b, opt + optsize + 40 * i) for i in range(nsec)]

    def off(rva):
        for _, vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        return None

    def cstr(o):
        return b[o:b.index(b"\0", o)].decode("ascii", "replace")
    dlls, cuda = [], []
    o = off(imp_rva) if imp_rva else None
    while o is not None:
        oft, _, _, name, ft = struct.unpack_from("<IIIII", b, o)
        if not name:
            break
        dll = cstr(off(name))
        dlls.append(dll)
        if dll.lower() == "nvcuda.dll":
            t = off(oft or ft)
            while t is not None:
                v = struct.unpack_from("<Q" if magic == 0x20B else "<I", b, t)[0]
                if not v:
                    break
                if not v >> 63:
                    cuda.append(cstr(off(v & 0x7FFFFFFF) + 2))
                t += 8 if magic == 0x20B else 4
        o += 20
    return PE_MACHINES.get(machine, hex(machine)), dlls, cuda


def cmd_pe(d, want):
    bad = 0
    for root, _, fs in os.walk(d):
        for f in sorted(fs):
            if not f.lower().endswith((".dll", ".pyd")):
                continue
            info = pe_info(os.path.join(root, f))
            if not info:
                continue
            m, dlls, cuda = info
            ok = m == want
            bad += 0 if ok else 1
            print(f"{'OK ' if ok else 'BAD'} {m:6} {f}: imports {', '.join(dlls)}")
            if cuda:
                print(f"        nvcuda.dll: {len(cuda)} functions; cuModuleLoadData {'yes' if 'cuModuleLoadData' in cuda else 'no'}, cuDeviceGetAttribute {'yes' if 'cuDeviceGetAttribute' in cuda else 'no'}")
    sys.exit(1 if bad else 0)


def main(a):
    if len(a) < 2:
        sys.exit(__doc__ or "see the header")
    c = a[1]
    if c == "index": cmd_index(a[2])
    elif c == "fetch": cmd_fetch(a[2], a[3])
    elif c == "list": cmd_list(a[2], a[3:])
    elif c == "extract": cmd_extract(a[2], a[3], a[4:])
    elif c == "headers": cmd_headers(a[2])
    elif c == "verify": cmd_verify(a[2], a[3])
    elif c == "compare": cmd_compare(a[2], a[3])
    elif c == "pe": cmd_pe(a[2], a[3])
    else: sys.exit("unknown command " + c)


if __name__ == "__main__":
    main(sys.argv)
