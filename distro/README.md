# Your operating system goes here

Two things, plus optionally a third:

| Path | What it is |
|---|---|
| `NAME` | one word - the build is called "PS4 `NAME` Linux" |
| `rootfs.tar.zst` | a tar of your distribution's root filesystem (`.tar.xz`, `.tar.gz` and plain `.tar` also work) |
| `overlay/` | files copied over the rootfs before packaging: configs, a user, autologin |

`rootfs.tar.zst` is not committed (see `.gitignore`) - it is usually hundreds of
megabytes. Keep your own copy wherever you build.

The tarball is the *contents* of the root filesystem, so `tar -tf` should print
`./etc`, `./usr`, `./bin` and so on, not `./my-distro/etc`. If you made it from a
mounted image:

```sh
sudo tar -C /mnt/image -cf - . | zstd -19 -T0 -o rootfs.tar.zst
```

Then:

```sh
bash scripts/check-rootfs.sh    # catches the cheap mistakes
bash scripts/build-image.sh     # applies overlay/, repacks as zstd
```

`scripts/build-image.sh` unpacks, copies `overlay/` over the tree and repacks -
so anything in `overlay/` wins. That is the place for the changes a console needs
(an autologin, a `ps4` user, a network config) without rebuilding your
distribution.
