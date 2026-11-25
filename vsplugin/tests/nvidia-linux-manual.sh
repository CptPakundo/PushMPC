#!/bin/sh
# nvidia-linux-manual.sh: the real-GPU test of PushMPC's Linux NVIDIA backend (RTX VSR, RTX Video HDR, VFG, cut guard),
# for a tester with an NVIDIA RTX GPU on Linux and the proprietary driver (570.190+ / 580.82+ / 590.44+). GitHub's
# hosted runners have no NVIDIA GPU, so this is the only place the backend runs for real. About 15 minutes.
#
#   sh nvidia-linux-manual.sh <build-dir-with-pushmpc_vs.so> [clip.mkv]
#
# It needs: the plugin and pushmpc-nvvfx-selftest from the CI artifact "pushmpc_vs-linux-nvidia" (or a local build with
# -DPMVS_NVIDIA_LINUX=ON), python3 with the VapourSynth wheel (pip install vapoursynth==82), ffmpeg and nvidia-smi;
# optionally mpv built with VapourSynth support and a 1080p 23.976 fps clip with a hard cut and a pan.
# Nothing is installed system-wide: the runtime goes to a scratch folder (deleted at the end unless KEEP=1).
# Send back the whole output (it ends with a summary) and $OUT/*.png.
set -u
BIN=${1:?usage: nvidia-linux-manual.sh <dir with pushmpc_vs.so> [clip]}
CLIP=${2:-}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUT=${OUT:-$(mktemp -d /tmp/pushmpc-nvl.XXXXXX)}
export PUSHMPC_NVVFX_DIR="$OUT/nvidia-vfx" PUSHMPC_VS_LOG_DIR="$OUT/logs" XDG_CACHE_HOME="$OUT/cache"
mkdir -p "$PUSHMPC_VS_LOG_DIR"
pass=0; fail=0
ok() { pass=$((pass + 1)); echo "PASS  $*"; }
no() { fail=$((fail + 1)); echo "FAIL  $*"; }
step() { echo; echo "=== $*"; }

step "0. Machine"
uname -a
nvidia-smi --query-gpu=name,driver_version,compute_cap,memory.total,utilization.gpu --format=csv || no "nvidia-smi"
python3 -c "import vapoursynth; print('VapourSynth', vapoursynth.__version__)" || no "python3 + VapourSynth"

step "1. Runtime fetch (the installer's script, NVIDIA's licence accepted for this test)"
if sh "$REPO/scripts/nvidia-vfx-fetch.sh" --dest "$PUSHMPC_NVVFX_DIR" --accept-nvidia-license && sh "$REPO/scripts/nvidia-vfx-fetch.sh" --check --dest "$PUSHMPC_NVVFX_DIR"; then
    ok "runtime installed and verified"
else no "runtime fetch"; fi

step "2. Self-test: VFG on a synthetic moving box (native; RTX 30 also with --ampere)"
"$BIN/pushmpc-nvvfx-selftest" --gpu && ok "VFG native self-test" || echo "note: native VFG fails on RTX 30 / 20 by design (exit $?)"
cc=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader | head -n 1)
if [ "$cc" = "8.6" ]; then
    "$BIN/pushmpc-nvvfx-selftest" --gpu --ampere && ok "RTX 30 compatibility self-test (cached for Auto in $XDG_CACHE_HOME/pushmpc)" || no "RTX 30 compatibility self-test"
fi

step "3. Synthetic clips through the plugin: timing, counts, colours"
python3 - "$BIN" "$OUT" <<'PY'
import os, sys, time, vapoursynth as vs
binp, out = sys.argv[1], sys.argv[2]
core = vs.core
core.std.LoadPlugin(os.path.join(binp, "pushmpc_vs.so"))
sel = core.pushmpc.SelectBackend()
t = lambda v: v.decode() if isinstance(v, bytes) else v
print("SelectBackend:", t(sel["backend"]), "|", t(sel["why"]), "| interp:", t(sel["interp"]))
def moving(w, h, bits, n=96, cut=48):
    # a bright box crossing a grey field at 12 px a frame, then a hard cut to a flat purple shot at frame `cut`
    fmt = vs.YUV420P10 if bits > 8 else vs.YUV420P8
    s = 1 << (bits - 8)
    bw, bh = (w // 8) & ~1, (h // 8) & ~1
    box = core.std.BlankClip(format=fmt, width=bw, height=bh, length=1, fpsnum=24000, fpsden=1001, color=[200 * s, 128 * s, 128 * s])
    wide = core.std.AddBorders(box, left=w, right=w + 2 * n * 12 - bw, top=(h - bh) // 2 & ~1, bottom=h - bh - ((h - bh) // 2 & ~1), color=[64 * s, 128 * s, 128 * s])
    frames = [core.std.Crop(wide, left=w + i * 12 - w // 2, right=wide.width - (w + i * 12 - w // 2) - w) for i in range(cut)]
    other = core.std.BlankClip(frames[0], color=[180 * s, 90 * s, 160 * s], length=n - cut)
    return core.std.Splice(frames + [other])
cases = [("1080p 8-bit, display 240 Hz (Auto: VSR to 4K, VFG)", 1920, 1080, 8, {"display_fps": 240.0}),
         ("1080p 8-bit, display 60 Hz, HDR output (RTX Video HDR)", 1920, 1080, 8, {"display_fps": 60.0, "display_hdr": 1}),
         ("4K 10-bit HDR source, 120 Hz (no VSR; VFG on RGB10A2)", 3840, 2160, 10, {"display_fps": 120.0, "transfer": 16, "matrix": 9, "primaries": 9}),
         ("1080p, x4 High, VSR level 4 fixed", 1920, 1080, 8, {"smoother": "4", "fg_quality": "High", "sharper": "4"})]
for name, w, h, bits, args in cases:
    src = moving(w, h, bits)
    try:
        o = core.pushmpc.Enhance(src, backend="nvidia", **args)
    except vs.Error as e:
        print("FAIL", name, e); continue
    k = o.num_frames // src.num_frames
    t0 = time.perf_counter()
    for i in range(o.num_frames):
        f = o.get_frame(i)
    dt = time.perf_counter() - t0
    kinds = [o.get_frame(i).props.get("PushMPCFrameKind", -1) for i in range(o.num_frames)]
    print(f"RESULT {name}: x{k} {o.width}x{o.height} {o.format.name} {o.fps}; {o.num_frames} frames in {dt:.2f} s = {o.num_frames / dt:.1f} fps "
          f"(source-equivalent {src.num_frames / dt:.1f} fps); real {kinds.count(0)}, generated {kinds.count(1)}, repeated {kinds.count(2)}")
    # the cut pair: the generated frames of the pair (47, 48) must equal real frame 47 (the guard holds A)
    if k > 1:
        a = o.get_frame(47 * k); g = o.get_frame(47 * k + 1)
        same = bytes(memoryview(a[0])) == bytes(memoryview(g[0]))
        print(f"RESULT {name}: cut pair held: {'yes' if same else 'NO'}")
    core.imwri.Write(o[47 * k + 1:47 * k + 2].resize.Bicubic(format=vs.RGB24, matrix_in_s="709"), "PNG", os.path.join(out, name.split(",")[0].replace(" ", "_") + "_cutpair_%d.png"), firstnum=0).get_frame(0) if hasattr(core, "imwri") else None
    core.imwri.Write(o[20 * k + 1:20 * k + 2].resize.Bicubic(format=vs.RGB24, matrix_in_s="709"), "PNG", os.path.join(out, name.split(",")[0].replace(" ", "_") + "_motion_%d.png"), firstnum=0).get_frame(0) if hasattr(core, "imwri") else None
PY
[ $? = 0 ] && ok "synthetic clips ran (check the RESULT lines: fps >= the output rate you need, cut pair held: yes)" || no "synthetic clips"

step "4. Real clip through vspipe (optional): ms per frame under load"
if [ -n "$CLIP" ] && command -v vspipe >/dev/null 2>&1; then
    cat > "$OUT/clip.vpy" <<EOF
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("$BIN/pushmpc_vs.so")
c = core.bs.VideoSource("$CLIP") if hasattr(core, "bs") else core.ffms2.Source("$CLIP")
c = core.resize.Bicubic(c, format=vs.YUV420P10 if c.format.bits_per_sample > 8 else vs.YUV420P8)
core.pushmpc.Enhance(c[:720], backend="nvidia", display_fps=240.0).set_output()
EOF
    nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader
    /usr/bin/time -v vspipe -p "$OUT/clip.vpy" -- > /dev/null 2> "$OUT/vspipe.txt" && ok "real clip: $(grep -E 'Output|fps' "$OUT/vspipe.txt" | tail -2 | tr '\n' ' ')" || no "vspipe real clip (see $OUT/vspipe.txt)"
else echo "skipped (no clip or no vspipe)"; fi

step "5. mpv (optional): play 60 s, two seeks, then quit"
if [ -n "$CLIP" ] && command -v mpv >/dev/null 2>&1; then
    timeout 120 mpv --no-config --vo=gpu-next --hwdec=no --length=60 --vf="vapoursynth=file=$REPO/vsplugin/scripts/pushmpc.vpy:buffered-frames=8:concurrent-frames=4" \
        --script-opts= --msg-level=all=status,vapoursynth=v "$CLIP" > "$OUT/mpv.txt" 2>&1 && ok "mpv played" || no "mpv (see $OUT/mpv.txt)"
    echo "Look while it plays: smooth pans, no ghosting at cuts, sharp text; mpv's stats (press i) show the output rate."
else echo "skipped"; fi

step "6. Logs"
tail -n 60 "$PUSHMPC_VS_LOG_DIR"/* 2>/dev/null
step "Summary: $pass passed, $fail failed; pictures and logs in $OUT"
nvidia-smi --query-gpu=name,driver_version,compute_cap --format=csv,noheader
[ "${KEEP:-0}" = 1 ] || rm -rf "$PUSHMPC_NVVFX_DIR" "$XDG_CACHE_HOME"
[ "$fail" = 0 ]
