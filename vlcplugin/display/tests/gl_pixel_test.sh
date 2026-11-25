#!/bin/bash
# gl_pixel_test.sh: PushMPC's OpenGL output fork (pushmpc_gl) against VLC's own gl output, on the screen. A 320x180 still
# is shown 4x upscaled in a 1280x720 window under Xvfb (Mesa llvmpipe) and the screen is grabbed:
#   - pushmpc_gl with upscaling off must match VLC's gl (the fork changes nothing else);
#   - pushmpc_gl with FSR must be different, keep the mean level, and have sharper edges than VLC's bilinear.
# Run inside xvfb-run with a 1280x720x24 screen. Needs VLC, ffmpeg and python3; env: PLUGDIR (folder with the plugin), OUT.
set -u
OUT=${OUT:-res/gl-pixel}
mkdir -p "$OUT"
export VLC_PLUGIN_PATH="$PLUGDIR"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=320x180,format=rgb24" -frames:v 1 "$OUT/still.png"
grab() {   # $1 = name, rest = VLC options
    local name=$1; shift
    vlc -I dummy --no-audio --no-video-title-show --no-osd --no-embedded-video --zoom=4 --image-duration=8 --play-and-exit \
        -vv "$@" "$OUT/still.png" > "$OUT/$name.log" 2>&1 &
    local pid=$!
    sleep 4
    ffmpeg -hide_banner -loglevel error -y -f x11grab -video_size 1280x720 -i "$DISPLAY+0,0" -frames:v 1 \
        -f rawvideo -pix_fmt rgb24 "$OUT/$name.rgb"
    ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgb24 -video_size 1280x720 -i "$OUT/$name.rgb" "$OUT/$name.png"
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    grep -hE 'using vout display module|PushMPC FSR' "$OUT/$name.log" | sort | uniq -c
}
grab stock --vout=gl
grab off --vout=pushmpc_gl --pushmpc-gl-upscale=off
grab fsr --vout=pushmpc_gl --pushmpc-gl-upscale=fsr
grep -q 'using vout display module "pushmpc_gl"' "$OUT/fsr.log" || { echo "::error::pushmpc_gl did not open"; exit 1; }
grep -q 'PushMPC FSR: 320x180 -> ' "$OUT/fsr.log" || { echo "::error::pushmpc_gl did not upscale with FSR"; exit 1; }
python3 - "$OUT" <<'EOF'
import sys
out = sys.argv[1]
W, H = 1280, 720
def load(n):
    with open(f"{out}/{n}.rgb", "rb") as f:
        return f.read()
def stats(a, b):
    d = sorted(abs(x - y) for x, y in zip(a, b))
    return sum(d) / len(d), d[int(len(d) * 0.999)], d[-1]
def mean(a):
    return sum(a) / len(a)
def edges(a):   # mean |horizontal gradient| of the green channel over the picture area
    g = a[1::3]
    s = 0
    for y in range(H):
        r = g[y * W:(y + 1) * W]
        s += sum(abs(r[x] - r[x - 1]) for x in range(1, W))
    return s / (W * H)
stock, off, fsr = load("stock"), load("off"), load("fsr")
m_o, p_o, x_o = stats(stock, off)
m_f, p_f, x_f = stats(stock, fsr)
e_s, e_f = edges(stock), edges(fsr)
print(f"off vs stock: mean |diff| {m_o:.3f}, 99.9% {p_o}, max {x_o}")
print(f"fsr vs stock: mean |diff| {m_f:.3f}, 99.9% {p_f}, max {x_f}")
print(f"mean level: stock {mean(stock):.2f}, fsr {mean(fsr):.2f}; edge energy: stock {e_s:.3f}, fsr {e_f:.3f} ({100 * (e_f / e_s - 1):+.1f}%)")
bad = []
if mean(stock) < 20: bad.append("the stock picture is (almost) black: nothing was grabbed")
if m_o > 0.5 or p_o > 3: bad.append("pushmpc_gl with upscaling off differs from VLC's gl")
if m_f < 0.3: bad.append("FSR output is the same as VLC's bilinear")
if abs(mean(fsr) - mean(stock)) > 0.02 * mean(stock) + 1: bad.append("FSR changed the mean level")
if e_f < e_s * 1.03: bad.append("FSR is not sharper than VLC's bilinear")
for b in bad: print(f"::error::{b}")
sys.exit(1 if bad else 0)
EOF
