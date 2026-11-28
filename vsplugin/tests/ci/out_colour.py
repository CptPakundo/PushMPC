# The output's colour for mpv (vsplugin/src/mp_out.h): mpv reads an output frame's colour only from its _MP_IMAGE,
# which the plugin copies from the source frame. Against the stub core (CI, no GPU), whose output is HDR10 only for an
# HDR source on an HDR display (pushcore_stub.cpp: hdrOutput = HDR input && displayHdr), else SDR:
#   PQ on an SDR display (the tone map), 4:2:0 and 4:4:4: sRGB / BT.709 / the output's matrix / limited, the HDR
#     metadata cleared, no Dolby Vision pointer, the *_orig trio = the new values, light AUTO;
#   HLG on an HDR display: PQ / BT.2020 / BT.2020 / limited, the same clearing;
#   PQ on an HDR display, BT.709 limited SDR in 4:2:0: the blob byte for byte;
#   BT.601 full-range SDR in 4:2:0: only the matrix (BT.709, the egress's) and the levels (limited), sys_orig with them;
#   SDR in 4:4:4 (RGB, converted by pushmpc.vpy): only the matrix (its rule: 170m at 320x240);
# and every byte the case does not name unchanged. The blob is mpv 0.39-0.42's struct mp_image on a 64-bit process
# (mp_params.h: anchor 28, repr.sys 208, dovi 232, *_orig 240, light 252).
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs.dll     PUSHMPC_STUB_DIR  folder of the stub pushcore.dll
import os
import struct
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
W, H, A, S = 320, 240, 28, 208
assert struct.calcsize("P") == 8
SYS = {1: 2, 6: 1, 9: 4}   # H.273 matrix -> libplacebo system


def blob(prim, trc, system, levels, light):
    b = bytearray(400)
    struct.pack_into("<ii", b, 0, W, H)
    struct.pack_into("<i", b, 8, 1000)
    struct.pack_into("<iiii", b, A, W, H, 1, 1)
    struct.pack_into("<ii", b, A + 20, prim, trc)
    struct.pack_into("<ff", b, A + 28 + 32, 0.005, 1000.0)            # hdr.min_luma, max_luma
    struct.pack_into("<ii", b, S, system, levels)
    struct.pack_into("<Q", b, S + 24, 0x12345678)                     # repr.dovi
    struct.pack_into("<iiii", b, S + 32, prim, trc, system, light)    # primaries_orig, transfer_orig, sys_orig, light
    struct.pack_into("<i", b, S + 48, 1)                              # chroma_location (never touched)
    return bytes(b)


def raw(v):
    return bytes(v) if isinstance(v, (bytes, bytearray, memoryview)) else str(v).encode("latin1")


def run(src, cs, cr, args, chroma, display_hdr):
    c = core.std.BlankClip(format=vs.YUV420P10, width=W, height=H, length=48, fpsnum=24000, fpsden=1001, color=[400, 512, 512])
    c = core.std.SetFrameProps(c, _SARNum=1, _SARDen=1, _ColorSpace=cs, _ColorRange=cr, _MP_IMAGE=src)
    out = core.pushmpc.Enhance(c, smoother="2", sharper="off", adapter="warp", core_dir=os.environ["PUSHMPC_STUB_DIR"], chroma=chroma, display_hdr=display_hdr, **args)
    return [raw(out.get_frame(i).props["_MP_IMAGE"]) for i in (0, 1, 2)]


def check(name, src, outs, fields, cleared):
    """fields: {offset: int} that must hold; cleared: the HDR block zeroed and dovi 0 (a new transfer)"""
    global bad
    changed = set()
    for o in fields:
        changed |= set(range(o, o + 4))
    if cleared:
        changed |= set(range(A + 28, A + 176)) | set(range(S + 24, S + 32))
    for i, ob in enumerate(outs):
        got = {o: struct.unpack_from("<i", ob, o)[0] for o in fields}
        ok = len(ob) == len(src) and got == fields and all(ob[k] == src[k] for k in range(len(src)) if k not in changed)
        if cleared:
            ok = ok and ob[A + 28:A + 176] == bytes(148) and struct.unpack_from("<Q", ob, S + 24)[0] == 0
        print(f"{name}, output {i}: {'ok' if ok else 'WRONG'} (fields {got}, want {fields})")
        bad += not ok


bad = 0
pq = blob(6, 12, 4, 1, 1)
hlg = blob(6, 13, 4, 1, 2)
sdr = blob(3, 1, 2, 1, 1)
sdr601full = blob(3, 1, 1, 2, 1)
for chroma, mx in (("420", 1), ("444", 6)):   # 4:2:0: the egress's BT.709; RGB: pushmpc.vpy's rule at 320x240 (170m)
    check(f"PQ on an SDR display, chroma {chroma}", pq, run(pq, 9, 1, dict(matrix=9, transfer=16, primaries=9, range=1), chroma, 0),
          {A + 20: 3, A + 24: 2, S: SYS[mx], S + 4: 1, S + 32: 3, S + 36: 2, S + 40: SYS[mx], S + 44: 0}, True)
check("HLG on an HDR display", hlg, run(hlg, 9, 1, dict(matrix=9, transfer=18, primaries=9, range=1), "420", 1),
      {A + 20: 6, A + 24: 12, S: 4, S + 4: 1, S + 32: 6, S + 36: 12, S + 40: 4, S + 44: 0}, True)
check("PQ on an HDR display", pq, run(pq, 9, 1, dict(matrix=9, transfer=16, primaries=9, range=1), "420", 1), {}, False)
check("SDR BT.709 limited", sdr, run(sdr, 1, 1, dict(matrix=1, transfer=1, primaries=1, range=1), "420", 0), {}, False)
check("SDR BT.601 full range", sdr601full, run(sdr601full, 6, 0, dict(matrix=6, transfer=1, primaries=1, range=0), "420", 0),
      {S: 2, S + 4: 1, S + 40: 2}, False)
check("SDR in 4:4:4 (RGB)", sdr, run(sdr, 1, 1, dict(matrix=1, transfer=1, primaries=1, range=1), "444", 0), {S: 1, S + 4: 1, S + 40: 1}, False)
if bad:
    print(f"::error::{bad} output(s) with a wrong _MP_IMAGE colour")
    sys.exit(1)
print("the output's _MP_IMAGE colour: every case as expected")
