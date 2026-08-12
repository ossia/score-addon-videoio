# nv12-external-oes-probe

Answers one question, in isolation from score: **does a
`GL_TEXTURE_EXTERNAL_OES` sampler read an NV12 `NvBufSurface` correctly?**

It allocates the surface the way `ArgusSession` does, fills it with a known
red/blue split through the CPU mapping, imports the whole frame as ONE EGLImage
(fourcc `NV12`, `PLANE0_*` + `PLANE1_*`), samples it through
`samplerExternalOES` into an FBO, reads that back and checks the colours.

It exists because score's own `grabTo` writes black for *every* rung on the
Jetson, CPU staging included, so it cannot distinguish "the external sampler is
broken" from "the readback is broken". This probe asks only the first question,
and answering it separately is what made the difference.

Result on the Orin NX (L4T r36.4.4, driver 540.4.0, 2026-08-12): **PASS** —
left pixel R=231 G=0 B=1, right R=0 G=0 B=241. So the import path and the
external sampler are sound, and a black frame in score is somewhere else.

Note the allocator's behaviour it also demonstrates: at 640x480 with
`disablePitchPadding`, plane 1 lands at offset 393216 = 6 x 64 KB, not at the
tight 768 x 480 = 368640. Plane offsets are 64 KB-aligned and must be read from
`NvBufSurfacePlaneParams` rather than derived.

## Building

No compiler on the board. Cross-compile with the Yocto recipe sysroot:

    SR=<build-argus>/tmp/work/armv8a-oe4t-linux/ossia-score/3.8.2+git/recipe-sysroot
    CC=<...>/recipe-sysroot-native/usr/bin/aarch64-oe4t-linux/aarch64-oe4t-linux-clang
    $CC --target=aarch64-oe4t-linux --sysroot=$SR -I$SR/usr/include \
        nv12-external-oes-probe.c -o oes-probe \
        -L$SR/usr/lib -lEGL -lGLESv2 -lnvbufsurface -ldl -lpthread

Run it with nothing else holding the GPU (stop the score service first).
