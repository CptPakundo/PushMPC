# The output's colour for mpv (vsplugin/src/mp_out.h): mpv reads an output frame's colour only from its _MP_IMAGE,
# which the plugin copies from the source frame. Against the stub core (CI, no GPU): a PQ source on an SDR display is
# tone-mapped by the core (hdrInput 1, hdrOutput 0), so every output's blob must say SDR (sRGB BT.709, the matrix the
# output was made with, limited), with the HDR metadata cleared, no Dolby Vision pointer, the *_orig trio equal to the
# new values and light AUTO, and every other byte unchanged. An SDR source keeps its blob byte for byte.
# The blob is mpv 0.39-0.42's struct mp_image on a 64-bit process (mp_params.h: anchor 28, repr.sys 208).
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs.dll     PUSHMPC_STUB_DIR  folder of the stub pushcore.dll
import os
import struct
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
W, H, A, S = 320, 240, 28, 208
assert struct.calcsize("P") == 8


def blob(prim, trc, system, levels, light):
    b = bytearray(400)
    struct.pack_into("<ii", b, 0, W, H)
    struct.pack_into("<i", b, 8, 1000)
    struct.pack_into("<iiii", b, A, W, H, 1, 1)
    struct.pack_into("<ii", b, A + 20, prim, trc)
    struct.pack_into("<ff", b, A + 28 + 32, 0.005, 1000.0)       # hdr.min_luma, max_luma
    struct.pack_into("<ii", b, S, system, levels)
    struct.pack_into("<Q", b, S + 24, 0x12345678)                 # repr.dovi
    struct.pack_into("<iiii", b, S + 32, prim, trc, system, light)  # primaries_orig, transfer_orig, sys_orig, light
    struct.pack_into("<i", b, S + 48, 1)                          # chroma_location (not to be touched)
    return bytes(b)


def raw(v):
    return bytes(v) if isinstance(v, (bytes, bytearray, memoryview)) else str(v).encode("latin1")


def run(src, colour_space, args, chroma):
    c = core.std.BlankClip(format=vs.YUV420P10, width=W, height=H, length=48, fpsnum=24000, fpsden=1001, color=[400, 512, 512])
    c = core.std.SetFrameProps(c, _SARNum=1, _SARDen=1, _ColorSpace=colour_space, _ColorRange=1, _MP_IMAGE=src)
    out = core.pushmpc.Enhance(c, smoother="2", sharper="off", adapter="warp", core_dir=os.environ["PUSHMPC_STUB_DIR"], chroma=chroma, display_hdr=0, **args)
    return [raw(out.get_frame(i).props["_MP_IMAGE"]) for i in (0, 1, 2)]


bad = 0
pq = blob(6, 12, 4, 1, 1)
patched = {A + 20: 3, A + 24: 2, S: None, S + 4: 1, S + 32: 3, S + 36: 2, S + 40: None, S + 44: 0}
for chroma, system in (("420", 2), ("444", 1)):   # 4:2:0: the egress's BT.709; RGB: pushmpc.vpy's rule at 320x240 (170m)
    for i, ob in enumerate(run(pq, 9, dict(matrix=9, transfer=16, primaries=9, range=1), chroma)):
        want = dict(patched); want[S] = system; want[S + 40] = system
        got = {o: struct.unpack_from("<i", ob, o)[0] for o in want}
        hdr_zero = ob[A + 28:A + 176] == bytes(148)
        dovi = struct.unpack_from("<Q", ob, S + 24)[0]
        changed = set(range(A + 20, A + 176)) | set(range(S, S + 8)) | set(range(S + 24, S + 48))
        rest = all(ob[k] == pq[k] for k in range(len(pq)) if k not in changed)
        ok = got == want and hdr_zero and dovi == 0 and rest and len(ob) == len(pq)
        print(f"PQ source, chroma {chroma}, output {i}: {'ok' if ok else 'WRONG'} (fields {got}, hdr cleared {hdr_zero}, dovi {dovi:#x}, other bytes unchanged {rest})")
        bad += not ok
sdr = blob(3, 1, 2, 1, 1)
for i, ob in enumerate(run(sdr, 1, dict(matrix=1, transfer=1, primaries=1, range=1), "420")):
    ok = ob == sdr
    print(f"SDR source, output {i}: blob {'unchanged' if ok else 'CHANGED'}")
    bad += not ok
if bad:
    print(f"::error::{bad} output(s) with a wrong _MP_IMAGE colour")
    sys.exit(1)
print("the output's _MP_IMAGE colour: every case as expected")
