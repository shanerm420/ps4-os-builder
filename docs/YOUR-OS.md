# What your distribution has to do

The boot chain is deliberately dumb, because it has to work with an operating
system it has never seen:

1. the app reserves a file on the PS4 drive (or uses the USB drive) and copies the
   kernel, the initramfs and your rootfs tarball next to it;
2. the loader boots the kernel with the initramfs;
3. the initramfs finds the image, creates the filesystem in it, unpacks your
   tarball into it, and `switch_root`s;
4. from there it is your distribution: its init, its display manager, its desktop.

Everything below follows from that. The exact steps are in
`initramfs/functions/55-ps4linux.sh` in
[ps4-linux-desktop](https://github.com/shanerm420/ps4-linux-desktop) - that file
is the contract, and it is short.

## Architecture and size

- **x86-64 only.** The console is a Jaguar-based x86-64.
- The tarball is unpacked into the space the app reserved. 32 GB is the default
  and it has to hold your distribution *plus* room for updates and user files.
  A rootfs that fills the image leaves nothing for `/var` and `/home`.
- Compression is zstd, because the PS4 kernel has `CONFIG_ZSTD_DECOMPRESS`.
  `scripts/build-image.sh` produces that for you.

## Init and login

- There must be something for the kernel to execute: `/sbin/init`, `/init`, or
  `systemd` at `/usr/lib/systemd/systemd`. `check-rootfs.sh` fails without one.
- **Autologin matters.** The console's only input at that moment is a USB keyboard
  or a controller, and HDMI output. A rootfs that stops at a text login is not
  broken, but it is a bad first impression - configure the display manager to log
  in automatically.
- Create a user with a known password, and give it `sudo` or `wheel`. The XFCE
  build uses `ps4` / `ps4linux`; there is nothing special about those names, they
  are just what the guide says.

## Kernel modules

The console runs the **PS4 kernel**, not your distribution's. So:

- modules built for a different version will not load (`check-rootfs.sh` warns
  when `/lib/modules` does not match `7.3.4-PS4Linux-baikal+`);
- the PS4 kernel has the hardware it needs built in - UFS, dm-crypt, the GPU
  firmware - so a distribution that needs no modules at all works;
- if your distribution needs modules, build them against that kernel and put them
  in `/lib/modules/7.3.4-PS4Linux-baikal+`.

## Graphics

This is the part that decides whether the result is usable.

Stock Mesa does not bind the PS4 GPU. Liverpool (original/Slim) and Gladius (Pro)
are GCN parts whose device ids are missing from Mesa's chip tables, so radeonsi
refuses the card and the desktop comes up on llvmpipe - software rendering.

The fix lives in the other repository: `patches/ps4-mesa.patch` adds the two chips
as Bonaire-class GFX7 parts, and `scripts/build-mesa.sh` builds Mesa with them,
unprivileged, against Debian 13's own build dependencies. The result is installed
to `/opt/ps4-mesa` and selected through libglvnd so an `apt upgrade` cannot take
it away.

You have two options:

- **copy it across**: take `/opt/ps4-mesa`, `/etc/ld.so.conf.d/00-ps4-mesa.conf`,
  `/usr/share/glvnd/egl_vendor.d/10-ps4-mesa.json` and the two
  `libGLX_ps4-mesa.so.0` / `libEGL_ps4-mesa.so.0` symlinks from a PS4 XFCE Linux
  image into your rootfs (that is what `distro/overlay/` is for) - provided your
  distribution is Debian 13-based, so the glibc and libLLVM match;
- **build it yourself** with the patch, against your distribution's libraries.

Either way, check it after boot with `glxinfo -B`: it should name a Radeon. If it
says llvmpipe, the GPU did not bind and the desktop is running on the CPU.

## Sound, network, input

- **Audio** goes over HDMI; the PS4 kernel has the driver built in.
- **Wi-Fi** is MediaTek. Do not put `regulatory.db` in an initramfs - it deadlocks
  that driver and takes DHCP down with it. In the rootfs it is fine.
- **Input**: DualShock 4 over USB or Bluetooth, and any USB keyboard and mouse.

## What the console does *not* give you

- No working bootloader of yours: the kernel and initramfs come from the package,
  always. Changing them means building a new package, not editing the install.
- No access to the PS4 system's files from Linux. The install is one file inside
  Sony's encrypted filesystem; Linux sees a loop device, nothing more.
- No dual boot. One Linux at a time: the app id is shared, so installing another
  build replaces this one.

## Checking it before you trust it

`scripts/check-rootfs.sh` is mechanical: architecture, init, modules, size, a
stray kernel, the patched Mesa. It cannot tell you whether your desktop comes up,
whether the GPU binds, or whether the display manager autologins. Those need the
console, and `docs/TEST-MATRIX.md` in the other repository is where the results
belong.
