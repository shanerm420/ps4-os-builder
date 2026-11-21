#!/bin/bash
# Stage the boot files and your rootfs into the app, then build the PKG.
#
# Inputs: boot/bzImage, boot/initramfs.cpio.gz, out/rootfs.tar.zst, vendor/app/.
# Output: out/pkg/PS4-<NAME>-Linux.pkg
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$HERE/out
APP=$HERE/vendor/app

NAME=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$HERE/distro/NAME" | grep -m1 . || true)
[ -n "$NAME" ] || { echo "distro/NAME is empty - put one word in it, e.g. GNOME" >&2; exit 1; }
TITLE="PS4 $NAME Linux"

export OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN:-}
[ -n "$OO_PS4_TOOLCHAIN" ] || { echo "set OO_PS4_TOOLCHAIN to a built OpenOrbis toolchain" >&2; exit 1; }
[ -x "$OO_PS4_TOOLCHAIN/bin/linux/PkgTool.Core" ] || {
	echo "no PkgTool.Core in $OO_PS4_TOOLCHAIN/bin/linux - the git repository alone is not a built toolchain" >&2
	exit 1
}

[ -f "$APP/installer/main.c" ] || { echo "no app source: run scripts/vendor.sh" >&2; exit 1; }
[ -f "$HERE/boot/bzImage" ] || { echo "no kernel: put bzImage in boot/ (see README)" >&2; exit 1; }
[ -f "$HERE/boot/initramfs.cpio.gz" ] || { echo "no initramfs: put initramfs.cpio.gz in boot/" >&2; exit 1; }
[ -f "$OUT/rootfs.tar.zst" ] || { echo "no image: run scripts/build-image.sh first" >&2; exit 1; }

echo "checking inputs ..."
zstd -t "$OUT/rootfs.tar.zst" >/dev/null
if command -v cpio >/dev/null; then
	zcat "$HERE/boot/initramfs.cpio.gz" | cpio -t --quiet | grep -qx 'functions/55-ps4linux.sh' \
		|| { echo "the initramfs has no PS4 boot function - it will not hand over to your rootfs" >&2; exit 1; }
fi

A=$APP/assets/ps4linux
rm -rf "$A"
mkdir -p "$A"
ln -f "$HERE/boot/bzImage" "$A/bzImage"
ln -f "$HERE/boot/initramfs.cpio.gz" "$A/initramfs.cpio.gz"
ln -f "$OUT/rootfs.tar.zst" "$A/rootfs.tar.zst"
# The app adopts a real loader payload at install time; this one only marks the
# slot. It must not look like a payload (see looks_like_loader() in the app).
printf 'PLACEHOLDER - the app replaces this with a real loader payload at install time.\n' \
	> "$A/ps4linux-loader.bin"
( cd "$A" && sha256sum bzImage initramfs.cpio.gz ps4linux-loader.bin rootfs.tar.zst | sha256sum | cut -c1-32 ) > "$A/build-id"

echo "building \"$TITLE\" ..."
( cd "$APP" && make -s clean && make -s APP=install TITLE="$TITLE" ) \
	|| { echo "the app failed to build" >&2; exit 1; }

PKG=$(ls -1 "$APP"/*.pkg 2>/dev/null | head -1)
[ -n "$PKG" ] || { echo "the build produced no PKG" >&2; exit 1; }

SAFE=$(printf '%s' "$NAME" | tr ' ' '-' | tr -cd 'A-Za-z0-9._-')
mkdir -p "$OUT/pkg"
FINAL=$OUT/pkg/PS4-$SAFE-Linux.pkg
mv -f "$PKG" "$FINAL"
( cd "$OUT/pkg" && sha256sum "$(basename "$FINAL")" > "$(basename "$FINAL").sha256" )

echo
echo "PKG:  $FINAL"
echo "size: $(stat -c %s "$FINAL") bytes"
echo "title: $TITLE"
echo "sha256: $(cut -d' ' -f1 "$FINAL.sha256")"
