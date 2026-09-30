# MSVDX (VXD370) userland prototype

Talks to the GMA500's H.264 decoder through `/dev/misc/poke`, from one
process. Not a driver yet: this is where the hardware gets understood.

- `msvdx.c` / `msvdx.h` -- soft reset, MMU page tables in contiguous memory,
  RENDEC buffers, firmware upload, the host/MTX message rings.
- `msvdxsmoke.c` -- one render message with a do-nothing command buffer.
- `hwdefs/` -- register and message definitions from Intel's MIT-licensed
  `psb_video` (github.com/intel/psb_video); licence in
  `hwdefs/LICENSE-psb_video.txt`.
- `hw.h` -- includes `hwdefs/` the way `psb_cmdbuf.c` does. `img_defs.h`
  refuses an OS it does not know, so `__linux__`/`LINUX` are defined around
  those headers only.

The firmware, `msvdx_fw.bin`, is Intel's and is not in this repository.

Build on the VAIO (the secondary gcc; the primary is gcc 2):

    gcc-x86 -O1 -Ihwdefs -I/boot/system/develop/headers/private/drivers \
        -o msvdxsmoke msvdxsmoke.c msvdx.c

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
