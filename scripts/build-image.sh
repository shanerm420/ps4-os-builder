#!/bin/bash
# distro/rootfs.tar.* + distro/overlay/ -> out/rootfs.tar.zst
#
# No root, no chroot: the tarball is unpacked into a scratch directory, the overlay
# is copied over it, and the result is repacked the way the initramfs expects
# (zstd, single top-level tree). The kernel needs CONFIG_ZSTD_DECOMPRESS, which the
# PS4 kernel has.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$HERE/out

SRC=""
for c in "$HERE/distro/rootfs.tar.zst" "$HERE/distro/rootfs.tar.xz" \
         "$HERE/distro/rootfs.tar.gz" "$HERE/distro/rootfs.tar"; do
	[ -f "$c" ] && { SRC=$c; break; }
done
[ -n "$SRC" ] || {
	echo "no rootfs found. Put one of these in distro/:" >&2
	echo "  rootfs.tar.zst  rootfs.tar.xz  rootfs.tar.gz  rootfs.tar" >&2
	exit 1
}

command -v zstd >/dev/null || { echo "zstd is not installed" >&2; exit 1; }

mkdir -p "$OUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "unpacking $(basename "$SRC") ..."
case "$SRC" in
	*.zst) zstd -dc "$SRC" | tar -xpf - -C "$STAGE" ;;
	*.xz)  xz -dc "$SRC"  | tar -xpf - -C "$STAGE" ;;
	*.gz)  gzip -dc "$SRC" | tar -xpf - -C "$STAGE" ;;
	*)     tar -xpf "$SRC" -C "$STAGE" ;;
esac

if [ -d "$HERE/distro/overlay" ] && [ -n "$(ls -A "$HERE/distro/overlay")" ]; then
	echo "applying distro/overlay/ ..."
	tar -cf - -C "$HERE/distro/overlay" --exclude=./README.md . | tar -xpf - -C "$STAGE"
fi

echo "repacking as zstd (this takes a few minutes) ..."
tar -cf - -C "$STAGE" . | zstd -19 -T0 -q -f -o "$OUT/rootfs.tar.zst"
zstd -t "$OUT/rootfs.tar.zst"

echo "out/rootfs.tar.zst: $(stat -c %s "$OUT/rootfs.tar.zst") bytes, $(tar -tf "$OUT/rootfs.tar.zst" 2>/dev/null | wc -l) entries"
