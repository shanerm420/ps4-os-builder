# The two boot files

| File | What it is |
|---|---|
| `bzImage` | the PS4 kernel, release `7.3.4-PS4Linux-baikal+` |
| `initramfs.cpio.gz` | the initramfs that finds the image, mounts it and switches root |

Neither is committed here - they are binaries from another project. Two ways to
get them.

## Take them out of a PS4 XFCE Linux package

They are inside it, under `assets/ps4linux/`, and they are not specific to XFCE -
the boot chain hands over to whatever rootfs the package carries:

```sh
PkgTool.Core pkg_extract PS4-XFCE-Linux.pkg /tmp/x
cp /tmp/x/*/assets/ps4linux/bzImage \
   /tmp/x/*/assets/ps4linux/initramfs.cpio.gz boot/
```

`pkg_extract` comes with the OpenOrbis toolchain; `PkgTool.Core` also accepts
`pkg_extract` with the package and an output directory.

## Build them

- Kernel: [ps4-linux-kernel](https://github.com/shanerm420/ps4-linux-kernel) -
  release `v7.3.4-ps4linux`, with `build.sh` and the config.
- Initramfs: `scripts/build-initramfs.sh` in
  [ps4-linux-desktop](https://github.com/shanerm420/ps4-linux-desktop). It
  splices the boot function into the feeRnt base; the result does not depend on
  the distribution.

`scripts/make-pkg.sh` checks that the initramfs really carries
`functions/55-ps4linux.sh`, so a wrong file is caught before packaging.
