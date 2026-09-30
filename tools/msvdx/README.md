# MSVDX (VXD370) userland prototype

Talks to the GMA500's H.264 decoder through `/dev/misc/poke`, from one
process. Not a driver yet: this is where the hardware gets understood.

- `msvdx.c` / `msvdx.h` -- soft reset, MMU page tables in contiguous memory,
  RENDEC buffers, firmware upload, the host/MTX message rings.
- `msvdxsmoke.c` -- one render message with a do-nothing command buffer.
- `msvdxdec.c` -- decode the first IDR picture of an H.264 file to NV12.
- `psb/` -- the H.264 decoder from psb_video's 2010 tree (the Moorestown code
  Wind River published under the MIT licence, which still speaks the DE2
  command format this firmware does): `psb_H264.c` unchanged, the command
  buffer encoders from `psb_cmdbuf.c` copied verbatim into `psb_cmdbuf.c`, and
  `psb_cmdbuf_host.c` / `psb_shim.h` in place of libva, DRM and wsbm.
- `va_h264.h` -- the H.264 parameter structures from libva (MIT).
- `hwdefs/` -- register and message definitions from Intel's MIT-licensed
  `psb_video` (github.com/intel/psb_video); licence in
  `hwdefs/LICENSE-psb_video.txt`.
- `hw.h` -- includes `hwdefs/` the way `psb_cmdbuf.c` does. `img_defs.h`
  refuses an OS it does not know, so `__linux__`/`LINUX` are defined around
  those headers only.

The firmware, `msvdx_fw.bin`, is Intel's and is not in this repository.

Build on the VAIO (the secondary gcc; the primary is gcc 2):

    gcc-x86 -O1 -std=gnu99 -I. -Ihwdefs -Ipsb \
        -I/boot/system/develop/headers/private/drivers -o msvdxdec \
        msvdxdec.c msvdx.c psb/psb_H264.c psb/psb_cmdbuf.c psb/psb_cmdbuf_host.c
    ./msvdxdec msvdx_fw.bin in.264 out.nv12 -v

## What the smoke test showed (2026-09-30)

    after open: MTX running (PC moving, 0x80902fa4 -> 0x809003dc),
                firmware filled both ring sizes with 100, MMU bypass off
    DMAC ch0:   SETUP 01282000 = our command buffer, CNT 5 -> 0
    MMU:        DIR_LIST_BASE2 = our page directory

So the firmware took the message, read the LLDMA record through our page
tables, and DMA'd the command buffer into the MTX. It then waits, because
the message says VLD and there is no bitstream: the entropy decoder never
signals end of slice. An empty command buffer cannot go further; the next
test is a real H.264 slice.

Two things learned getting here: `POKE_GET_PHYSICAL_ADDRESS` needs a
non-zero `size` (it calls `get_memory_map(address, size)`), and
`create_area(B_CONTIGUOUS)` works from userland.

## The first hardware decode is bit-exact (2026-09-30)

A 176x144 Constrained Baseline IDR picture (libx264, CAVLC, QP 23) decoded
on the VAIO's MSVDX and compared with ffmpeg's software decode of the same
file:

    completed: fence 1          decode: 930 us
    luma   PSNR inf  (mse 0)    chroma PSNR inf (mse 0)
    identical bytes: 38016 of 38016

![hardware (left) and ffmpeg (right)](docs/first-idr-hw-vs-ffmpeg.png)

Which licence each piece came from mattered, and it is worth writing down
because the obvious sources are the wrong ones:

- Intel's 2014 `intel/psb_video` (MIT) has the register headers and a
  `mrst/psb_H264.c`, but its `psb_cmdbuf.c` has moved to the Medfield (DE3)
  command format: register blocks as header-per-run, a newer 20-byte render
  message, and no DE2 RENDEC helpers at all. `mrst/` there is dead code.
- The Android mirror of the same driver
  (`android.googlesource.com/platform/hardware/intel/img/psb_video`) has the
  whole history from April 2010. A 2011 snapshot there carries
  "INTEL CONFIDENTIAL" headers in `hwdefs/` and was used only to compare the
  message layout -- nothing is copied from it.
- The first commit, `7e8d39a` (2010-04-17, "Copy wind river video driver"),
  is MIT throughout except one unused header, and is exactly the DE2 driver:
  register pairs (`CMD_REGVALPAIR_WRITE | count`, then reg/value), the
  RENDEC slice-info blocks, and the 32-byte FW_VA_RENDER message with the
  buffer size in bytes. That is what `psb/` is built from.

Relocations: `RELOC_SHIFT4` is `(address >> 4)` in the low 28 bits with the
command in the top four -- the kernel's `psb_apply_reloc()` shifts right by
the alignment shift and left by nothing, whatever the user-side arithmetic
suggests.
