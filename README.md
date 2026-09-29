# GMA500 driver for Haiku

[English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md)

A graphics driver for Intel SCH US15W (Poulsbo, "GMA500") machines such as the
Sony VAIO P. It gives Haiku a **hardware cursor** on this chip, which the VESA
driver cannot do.

Without it, app_server draws the mouse pointer in software: on every move it
saves what is under the pointer, draws the pointer, and restores the old
pixels. On a 1.33 GHz Atom with no 2D acceleration that work is visible. With
this driver the display engine composites the pointer at scan-out instead, and
none of that happens.

## Install with pkgman

| Haiku | Commands |
| --- | --- |
| 32-bit x86 (x86_gcc2) | `pkgman add-repo https://pkgman.rainygirl.com/x86_gcc2`<br>`pkgman install gma500` |

**Reboot after installing.** The kernel loads graphics drivers at boot, so the
driver does nothing until the machine restarts.

If `pkgman add-repo` fails with `Operation not supported`, the network kit of
that image has no TLS; use `http://pkgman.rainygirl.com/x86_gcc2` instead.

## Install from source

On the Haiku machine:

```sh
make                      # builds the driver and the accelerant
make install              # installs both into ~/config/non-packaged
```

Then reboot.

```sh
make uninstall            # removes both; reboot to go back to VESA
```

## Checking that it worked

After rebooting:

```sh
ls /dev/graphics/
```

`poulsbo` should be listed next to `vesa`. To see which one app_server
actually picked:

```sh
listimage $(ps | grep [a]pp_server | head -1 | awk '{print $2}') | grep accelerant
```

It should print `poulsbo.accelerant`. If it prints `vesa.accelerant`, the
driver did not take over - see [AGENTS.md](AGENTS.md).

## Going back to VESA

Installed with pkgman:

```sh
pkgman uninstall gma500
```

Installed from source:

```sh
make uninstall
```

Either way, reboot afterwards. app_server falls back to the VESA driver on its
own when `/dev/graphics/poulsbo` is gone, so a broken driver cannot leave the
machine without a display - and if it ever does, ssh in and remove the files.

## About 2D acceleration

The driver drives the chip's PowerVR SGX535 2D engine and offers the screen to
screen copy, rectangle fill and invert hooks. A copy measures about 13 times
faster than the same copy done by the CPU on a VAIO P.

Haiku's app_server never calls those hooks, and that is not something a driver
can fix. `AccelerantHWInterface` always allocates a back buffer in main memory,
so app_server composes each frame there and pushes finished rectangles to the
screen - it never copies inside the frame buffer, which is the one thing the
blit hook could take over. Measured on a VAIO P, an 800x500 window-drag step
costs 2.4 ms inside the back buffer plus 1.6 ms to push out, against 4.1 ms for
the same copy on the engine, so the cached-memory path is already the faster
one.

The hooks are kept because they are correct and cost nothing, and because
anything that drives this accelerant directly can use them. See
[AGENTS.md](AGENTS.md) for the full measurements.

## Sprite plane

The chip has no overlay plane - Intel's SCH US15W datasheet lists only Display,
Cursor and VGA planes, and never uses the word "overlay". It does have a sprite
plane, which the driver offers through Haiku's overlay hooks: a second RGB
surface composited by the display engine, positioned anywhere on screen.

There is no YUV conversion and no scaling; display planes on this chip are RGB
only and show their source 1:1. Note also that Haiku's own overlay path does not
currently hand the pixel buffer to the application correctly, so the hooks are
not usable from an app until that is fixed. See [AGENTS.md](AGENTS.md).

## What this driver does not do

- **No 3D acceleration.** There is no public documentation for the 3D side of
  the SGX535.
- **No mode setting.** The mode the BIOS set is kept as it is. On the machines
  this targets the panel is a fixed resolution anyway.
- **No DPMS beyond "on".**

## AI disclosure

Parts of this driver were developed with the assistance of AI coding tools
(Anthropic's Claude). All code has been reviewed and tested by the author on
real hardware.

## License

MIT. Development notes - what the registers do, what was measured, and which
two mistakes cost the most time - are in [AGENTS.md](AGENTS.md).
