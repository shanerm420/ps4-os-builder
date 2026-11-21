# PS4 OS Builder

Build your own Linux for a jailbroken PS4 - with **your** operating system in it.

This is the same build that produces
[PS4 XFCE Linux](https://github.com/shanerm420/ps4-linux-desktop), with the Linux
distribution left out - and it is a separate project, with its own copy of the
installer in `app/`. You bring a rootfs; this repository turns it into a PS4 PKG
that installs it, boots it, and starts it afterwards, with the same menu, the
same graphics-memory setting, the same autoload handling and the same recovery
path. The result is named after what you put in it: **PS4 `NAME` Linux**.

**Nothing here has run on a console.** The app, the boot logic and the packaging
are the ones described in the other repository, where every hardware row of the
test matrix is still `untested`. This kit inherits that status.

## What is in here, and what is not

| Part | Where it comes from |
|---|---|
| The app (menu, install, Start Linux, VRAM, autoload, recovery) | `app/` in this repository. `scripts/vendor.sh` copies it to `vendor/app` and renames it for you |
| Kernel `bzImage` and `initramfs.cpio.gz` | you put them in `boot/` - take them out of a PS4 XFCE Linux package, or build them (see below) |
| The Linux distribution | **yours**, in `distro/rootfs.tar.zst` |
| Files you want added to it | `distro/overlay/` |
| The package build | `scripts/make-pkg.sh` (this repository) |

The distribution is the only part that was removed. Everything else is the code
that already knows how to put a Linux on a PS4 and start it.

## Quick start

```sh
# 1. what your package will be called: PS4 <NAME> Linux
echo GNOME > distro/NAME

# 2. your operating system, as a tar of its root filesystem
cp /path/to/your-rootfs.tar.zst distro/rootfs.tar.zst
#    (also accepted: .tar.xz, .tar.gz, plain .tar)

# 3. the two boot files, from a PS4 XFCE Linux package or built yourself
cp bzImage initramfs.cpio.gz boot/

# 4. the app source, copied to vendor/app and renamed to your NAME
bash scripts/vendor.sh

# 5. check the rootfs, then build
bash scripts/check-rootfs.sh
bash scripts/build-image.sh          # distro/ + overlay/ -> out/rootfs.tar.zst
bash scripts/make-pkg.sh             # -> out/pkg/PS4-GNOME-Linux.pkg
```

`make-pkg.sh` needs a **built** OpenOrbis toolchain:

```sh
curl -LO https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
tar -xzf toolchain-llvm-18.tar.gz
export OO_PS4_TOOLCHAIN=$PWD/OpenOrbis/PS4Toolchain
```

## Where the boot files come from

A PS4 PKG is just a container: the app inside it carries `assets/ps4linux/`, and
that directory holds the kernel, the initramfs and the rootfs. So the two boot
files can be lifted from any PS4 XFCE Linux package:

```sh
PkgTool.Core pkg_extract PS4-XFCE-Linux.pkg /tmp/x
cp /tmp/x/*/assets/ps4linux/bzImage /tmp/x/*/assets/ps4linux/initramfs.cpio.gz boot/
```

Or build them yourself: the kernel is its own project
([ps4-linux-kernel](https://github.com/shanerm420/ps4-linux-kernel), release
`7.3.4-PS4Linux-baikal+`), and the initramfs is built by
`scripts/build-initramfs.sh` in the other repository. Both are unchanged by
anything you do here - the boot chain does not care which distribution it hands
over to.

## Your distribution has to hold up its end

The boot chain does three things and then gets out of the way: it finds the image,
mounts its single ext4 filesystem, and `switch_root`s into it. That means your
rootfs must:

- be **x86-64**, and fit in the space the app reserves (32 GB by default);
- have an init the kernel can execute (`/sbin/init`, or `/init`);
- come up on its own - the console has no keyboard until your display manager or
  getty does. Autologin is strongly recommended;
- use the **PS4 kernel's modules**, or none at all. A distribution built around a
  different kernel version will not load its own modules; either ship a rootfs
  that does not need them, or build the modules against `7.3.4-PS4Linux-baikal+`.

[`docs/YOUR-OS.md`](docs/YOUR-OS.md) has the details, and
`scripts/check-rootfs.sh` checks the mechanical parts for you.

## Graphics

This kit does not carry a graphics stack, because that belongs to your
distribution. Be aware of what that means: **stock Mesa does not bind the PS4
GPU.** Liverpool and Gladius are GCN parts whose device ids are not in Mesa's
tables, so a normal distribution comes up on llvmpipe - software rendering.

The fix is in the other repository: `patches/ps4-mesa.patch` plus
`scripts/build-mesa.sh` build Mesa with those two chips registered, and the
result is installed to `/opt/ps4-mesa` and selected through libglvnd. Do the same
in your rootfs (or copy that directory and the two vendor files across) and you
get the same graphics as PS4 XFCE Linux.

## What the build produces

`out/pkg/PS4-<NAME>-Linux.pkg`, with the same TITLE_ID (`PS4LNX001`) as the
other build, because it is the same app. Installing one replaces the other -
one Linux at a time per console. The install location
(`/user/app/ps4linux.img`, `/data/linux/boot`) is shared for the same reason.

## Honest limits

- Nothing has been run on a console. Not this kit, not the build it comes from.
- The kernel and initramfs are the proven-by-inspection ones; your rootfs is not.
  `check-rootfs.sh` catches the mechanical mistakes, not a distribution that
  cannot start a display.
- The app adopts a loader payload (it ships a placeholder); you still need a
  current `ps4-linux-*.bin` payload on a USB stick or in `/data/payloads` before
  Linux can be started.

## Licence

The scripts here are MIT. The app in `app/` is OpenOrbis (GPL-3.0): a copy of the
app published in [ps4-linux-desktop](https://github.com/shanerm420/ps4-linux-desktop),
kept in this repository so the two projects are separate and this one builds on
its own. It carries its own notices. Your distribution keeps its own licences -
do not bundle Sony-extracted blobs, and do not redistribute a distribution that
forbids it. See `NOTICE`.
