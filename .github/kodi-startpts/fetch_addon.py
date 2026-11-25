"""Install a binary add-on the way Kodi's official repository serves it: read the <dir> entries of the bundled
repository.xbmc.org/addon.xml, find the add-on in each dir's addons.xml, download <datadir>/<id>/<id>-<version>.zip
and unpack it into <kodi home>/addons.
usage: fetch_addon.py <kodi install dir> <kodi home> <addon id>"""
import gzip, os, re, sys, urllib.request, zipfile, io
import xml.etree.ElementTree as ET

kodi, home, aid = sys.argv[1:4]
repo = ET.parse(os.path.join(kodi, "addons", "repository.xbmc.org", "addon.xml")).getroot()
print("repository.xbmc.org", repo.get("version"))


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "kodi-ci"}), timeout=60) as r:
        data = r.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data


def vkey(v):
    return [int(x) if x.isdigit() else 0 for x in re.split(r"[.\-~]", v)]


best = None
for d in repo.iter("dir"):
    info, datadir = d.findtext("info"), d.findtext("datadir")
    print("dir", d.attrib, info, datadir)
    if not info or not datadir:
        continue
    try:
        idx = ET.fromstring(get(info))
    except Exception as e:  # noqa: BLE001
        print("  cannot read", info, e)
        continue
    for ad in idx.iter("addon"):
        if ad.get("id") != aid:
            continue
        plats = [e.findtext("platform") for e in ad.iter("extension") if e.findtext("platform")]
        paths = [e.findtext("path") for e in ad.iter("extension") if e.findtext("path")]
        print("  found", aid, ad.get("version"), "platform", plats, "path", paths)
        if "windows-x86_64" not in " ".join(plats).split():
            continue
        if best is None or vkey(ad.get("version")) > vkey(best[0]):
            best = (ad.get("version"), datadir.rstrip("/"), paths[0] if paths else f"{aid}+windows-x86_64/{aid}-{ad.get('version')}.zip")
if not best:
    sys.exit(f"{aid} not found for Windows x64 in the official repository")
ver, datadir, rel = best
cands = [rel, f"{aid}+windows-x86_64/{aid}+windows-x86_64-{ver}.zip", f"{aid}/{aid}+windows-x86_64-{ver}.zip",
         f"{aid}+windows-x86_64/{aid}-{ver}.zip"]
data = None
for c in dict.fromkeys(cands):
    url = f"{datadir}/{c}"
    try:
        data = get(url)
        print("download", url)
        break
    except Exception as e:  # noqa: BLE001
        print("  no", url, e)
if data is None:
    for d_ in (f"{datadir}/{aid}/", f"{datadir}/{aid}+windows-x86_64/"):
        try:
            print(d_, re.findall(r'href="([^"]+.zip)"', get(d_).decode("utf-8", "replace")))
        except Exception as e:  # noqa: BLE001
            print("  no listing", d_, e)
    sys.exit("download failed")
z = zipfile.ZipFile(io.BytesIO(data))
z.extractall(os.path.join(home, "addons"))
print("installed", sorted(set(n.split("/")[0] for n in z.namelist())))
with open(os.path.join(home, "addons", aid, "addon.xml"), encoding="utf-8") as f:
    print(f.read()[:600])
