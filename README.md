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

## What this driver does not do

- **No 2D or 3D acceleration.** The drawing engine on this chip is a PowerVR
  SGX535 with no public documentation. Only the display half of the chip, which
  is Intel's and i915-shaped, is used here.
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
