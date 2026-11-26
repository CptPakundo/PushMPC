#!/usr/bin/env python3
"""add-to-mpv.py <mpv source dir>: puts hwdec_d3d11gldx.c (D3D11 frames into OpenGL players on Windows) into mpv 0.42:
the file, its meson source line (with gl-dxinterop and d3d11) and its entry in the interop list. Idempotent."""
import pathlib, shutil, sys

src = pathlib.Path(sys.argv[1])
here = pathlib.Path(__file__).resolve().parent
shutil.copy(here / "hwdec_d3d11gldx.c", src / "video/out/opengl/hwdec_d3d11gldx.c")

def edit(rel, anchor, add, before=False):
    p = src / rel
    t = p.read_text(encoding="utf-8")
    if add.strip() in t:
        return
    if anchor not in t:
        sys.exit(f"add-to-mpv: {anchor!r} not found in {rel}")
    t = t.replace(anchor, add + anchor if before else anchor + add, 1)
    p.write_text(t, encoding="utf-8", newline="\n")

edit("meson.build", "    sources += files('video/out/d3d11/hwdec_d3d11va.c')\n",
     "    if features['gl-dxinterop']\n        sources += files('video/out/opengl/hwdec_d3d11gldx.c')\n    endif\n")
edit("video/out/gpu/hwdec.c", "extern const struct ra_hwdec_driver ra_hwdec_dxva2gldx;\n",
     "extern const struct ra_hwdec_driver ra_hwdec_d3d11gldx;\n")
edit("video/out/gpu/hwdec.c", "#if HAVE_GL_DXINTEROP_D3D9\n    &ra_hwdec_dxva2gldx,\n",
     "#if HAVE_GL_DXINTEROP && HAVE_D3D11 && HAVE_D3D_HWACCEL\n    &ra_hwdec_d3d11gldx,\n#endif\n", before=True)
print("add-to-mpv: hwdec_d3d11gldx added")
