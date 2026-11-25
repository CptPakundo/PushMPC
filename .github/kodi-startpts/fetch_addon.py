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
        print("  found", aid, ad.get("version"), "platform", plats)
        if plats and not any(p in ("all", "windx64", "windows-x86_64") or "win" in p and "64" in p for p in " ".join(plats).split()):
            continue
        if best is None or vkey(ad.get("version")) > vkey(best[0]):
            best = (ad.get("version"), datadir.rstrip("/"))
if not best:
    sys.exit(f"{aid} not found for Windows x64 in the official repository")
ver, datadir = best
url = f"{datadir}/{aid}/{aid}-{ver}.zip"
print("download", url)
z = zipfile.ZipFile(io.BytesIO(get(url)))
z.extractall(os.path.join(home, "addons"))
print("installed", sorted(set(n.split("/")[0] for n in z.namelist())))
with open(os.path.join(home, "addons", aid, "addon.xml"), encoding="utf-8") as f:
    print(f.read()[:600])
