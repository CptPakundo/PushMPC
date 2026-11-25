#!/bin/bash
# mac_pixel_test.sh: PushMPC's macOS OpenGL output fork (pushmpc_macosx) against VLC's own macosx output, on the screen of
# the runner's logged-in GUI session. A 320x180 still is shown fullscreen (upscaled to the screen) and captured:
#   - pushmpc_macosx with upscaling off must match VLC's macosx output (the fork changes nothing else);
#   - with FSR it must be different, keep the mean level, and have sharper edges than VLC's bilinear.
# The picture is letterboxed at a known place (fullscreen, not macOS's native fullscreen space).
# env: VLC (the VLC binary), PLUGDIR (folder with the plugin), OUT; INTF (default: minimal_macosx, then macosx).
set -u
OUT=${OUT:-res/mac-pixel}
mkdir -p "$OUT"
export VLC_PLUGIN_PATH="$PLUGDIR"
python3 - "$OUT/still.png" <<'EOF'
import sys, zlib, struct
W, H = 320, 180
rows = []
for y in range(H):
    r = bytearray([0])
    for x in range(W):
        v = 200 if ((x // 8 + y // 8) & 1) else 60          # squares
        if y > H // 2: v = 16 + (235 - 16) * x // (W - 1)  # gradient
        if abs(x - y * W // H) < 1: v = 235                # a thin diagonal
        c = (v, v, v)
        if H // 8 <= y < H // 4 and W // 8 <= x < W // 4: c = (220, 40, 40)   # a red block
        r += bytes(c)
    rows.append(bytes(r))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + \
      chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
EOF
screencapture -x -t bmp "$OUT/desktop.bmp" || { echo "::error::screencapture failed"; exit 1; }
# VLC's macosx interface asks about metadata retrieval on its first run (VLCMainWindow.m, key VLCFirstRun): answer it
defaults write org.videolan.vlc VLCFirstRun -date "2026-01-01 00:00:00 +0000"
grab() {   # $1 = name, $2 = interface, rest = VLC options
    local name=$1 intf=$2; shift 2
    # fullscreen (not macOS's native fullscreen space): the picture is letterboxed at a known place on the screen
    "$VLC" -I "$intf" --no-audio --no-video-title-show --no-osd --fullscreen --no-macosx-nativefullscreenmode \
        --image-duration=12 --play-and-exit --no-metadata-network-access -vv "$@" "$OUT/still.png" > "$OUT/$name.log" 2>&1 &
    local pid=$!
    sleep 8
    screencapture -x -t bmp "$OUT/$name.bmp"
    kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    grep -hE 'using vout display module|using interface module|PushMPC FSR|vout display error|passing over|cannot|failed' "$OUT/$name.log" | sort | uniq -c | head -12
}
ok=""
intf=${INTF:-macosx}   # (minimal_macosx: "interface default initialization failed" with VLC.app 3.0.x)
echo "== VLC's own macosx output"
grab stock "$intf" --vout=macosx
stock=1
if ! grep -q 'using vout display module "macosx"' "$OUT/stock.log"; then
    stock=""
    why=$(grep -ahm1 'macosx vout display error' "$OUT/stock.log" | sed 's/^\[[0-9a-f]*\] //')
    echo "::warning::VLC's own macosx output cannot open on this runner ($why): pushmpc_macosx with upscaling off is the reference"
fi
ok=$intf
echo "== pushmpc_macosx"
grab off "$ok" --vout=pushmpc_macosx --pushmpc-gl-upscale=off
grep -q 'using vout display module "pushmpc_macosx"' "$OUT/off.log" || { echo "::error::pushmpc_macosx did not open"; tail -30 "$OUT/off.log"; exit 1; }
[ -n "$stock" ] || cp "$OUT/off.bmp" "$OUT/stock.bmp"
echo "stock_reference=${stock:-0}" > "$OUT/reference.txt"
grab fsr "$ok" --vout=pushmpc_macosx --pushmpc-gl-upscale=fsr
grep -q 'using vout display module "pushmpc_macosx"' "$OUT/fsr.log" || { echo "::error::pushmpc_macosx did not open"; tail -30 "$OUT/fsr.log"; exit 1; }
grep -h 'PushMPC FSR: ' "$OUT/fsr.log" | head -2
for n in desktop stock off fsr; do sips -s format png "$OUT/$n.bmp" --out "$OUT/$n.png" >/dev/null 2>&1 || true; done
python3 - "$OUT" <<'EOF'
import sys, struct
out = sys.argv[1]
def load(n):
    b = open(f"{out}/{n}.bmp", "rb").read()
    off, = struct.unpack_from("<I", b, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", b, 18)
    bpp //= 8
    stride = (w * bpp + 3) & ~3
    top = h < 0; h = abs(h)
    px = []
    for y in range(h):
        sy = y if top else h - 1 - y
        row = b[off + sy * stride: off + sy * stride + w * bpp]
        px.append([(row[x * bpp + 2], row[x * bpp + 1], row[x * bpp]) for x in range(w)])
    return w, h, px
have_stock = open(f"{out}/reference.txt").read().strip().endswith("1")
w, h, desk = load("desktop")
_, _, stock = load("stock"); _, _, off = load("off"); _, _, fsr = load("fsr")
# fullscreen: the 16:9 picture is letterboxed in the middle of the screen
s = min(w / 320, h / 180)
vw, vh = int(320 * s), int(180 * s)
x0, y0 = (w - vw) // 2 + 8, (h - vh) // 2 + 8
x1, y1 = x0 + vw - 16, y0 + vh - 16
print(f"screen {w}x{h}, picture area {x0}..{x1} x {y0}..{y1} ({s:.2f}x)")
def red(p): # the still's red block must be where the picture is (else the capture shows something else)
    c = p[y0 - 8 + vh * 3 // 16][x0 - 8 + vw * 3 // 16]
    return c[0] > c[1] + 80 and c[0] > c[2] + 80
if not red(stock): print(f"::error::the stock capture does not show the picture at the expected place"); sys.exit(1)
def region(p): return [c for y in range(y0, y1) for x in range(x0, x1) for c in p[y][x]]
S, O, F = region(stock), region(off), region(fsr)
def stats(a, b):
    d = sorted(abs(x - y) for x, y in zip(a, b))
    return sum(d) / len(d), d[int(len(d) * 0.999)], d[-1]
def edges(p):
    s = n = 0
    for y in range(y0, y1):
        for x in range(x0 + 1, x1):
            s += abs(p[y][x][1] - p[y][x - 1][1]); n += 1
    return s / n
mean = lambda a: sum(a) / len(a)
m_o, p_o, x_o = stats(S, O); m_f, p_f, x_f = stats(S, F)
e_s, e_f = edges(stock), edges(fsr)
print(("" if have_stock else "(no stock reference: off vs off) ") + f"off vs stock: mean |diff| {m_o:.3f}, 99.9% {p_o}, max {x_o}")
print(f"fsr vs stock: mean |diff| {m_f:.3f}, 99.9% {p_f}, max {x_f}")
print(f"mean level: stock {mean(S):.2f}, fsr {mean(F):.2f}; edge energy: stock {e_s:.3f}, fsr {e_f:.3f} ({100 * (e_f / e_s - 1):+.1f}%)")
bad = []
if have_stock and (m_o > 0.5 or p_o > 3): bad.append("pushmpc_macosx with upscaling off differs from VLC's macosx output")
if m_f < 0.3: bad.append("FSR output is the same as VLC's bilinear")
if abs(mean(F) - mean(S)) > 0.02 * mean(S) + 1: bad.append("FSR changed the mean level")
if e_f < e_s * 1.03: bad.append("FSR is not sharper than VLC's bilinear")
for b in bad: print(f"::error::{b}")
sys.exit(1 if bad else 0)
EOF
