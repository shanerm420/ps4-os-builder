#!/bin/bash
# Mechanical checks on the rootfs you supplied.
#
# This is not a test of whether your distribution boots - nothing here can tell you
# that. It catches the mistakes that are cheap to catch: the wrong architecture, no
# init, kernel modules for a kernel this console is not running, a kernel image
# sitting inside the rootfs, and no PS4-patched Mesa.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)

SRC=""
for c in "$HERE/distro/rootfs.tar.zst" "$HERE/distro/rootfs.tar.xz" \
         "$HERE/distro/rootfs.tar.gz" "$HERE/distro/rootfs.tar"; do
	[ -f "$c" ] && { SRC=$c; break; }
done
[ -n "$SRC" ] || { echo "no rootfs in distro/ - nothing to check" >&2; exit 1; }

case "$SRC" in
	*.zst) command -v zstd >/dev/null || { echo "zstd is not installed (needed for .zst)" >&2; exit 1; }; DEC="zstd -dc" ;;
	*.xz)  command -v xz >/dev/null   || { echo "xz is not installed (needed for .xz)" >&2; exit 1; };   DEC="xz -dc" ;;
	*.gz)  command -v gzip >/dev/null || { echo "gzip is not installed" >&2; exit 1; };                   DEC="gzip -dc" ;;
	*)     DEC="cat" ;;
esac

TMP=$(mktemp -d)
LIST=$(mktemp)
trap 'rm -rf "$TMP" "$LIST"' EXIT

echo "reading $(basename "$SRC") ..."
$DEC "$SRC" | tar -tf - > "$LIST" 2>/dev/null || { echo "cannot read the tarball" >&2; exit 1; }
ENTRIES=$(wc -l < "$LIST")
have() { grep -qE "^(\./)?$1/?$" "$LIST"; }

FAIL=0
WARN=0
ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; FAIL=1; }
warn() { echo "  warn  $*"; WARN=1; }

echo "structure"
[ "$ENTRIES" -gt 100 ] && ok "$ENTRIES entries" || bad "only $ENTRIES entries - is this really a root filesystem?"

# One decompression pass for the handful of files worth looking inside.
$DEC "$SRC" | tar -xf - -C "$TMP" --wildcards \
	'./bin/sh' './usr/bin/sh' './bin/dash' './usr/bin/dash' \
	'./sbin/init' './init' './etc/os-release' './usr/lib/os-release' 2>/dev/null || true

# Merged-usr distributions make /bin a symlink to usr/bin, and os-release is
# usually a symlink too, so follow one level before believing the file is missing.
resolve() {
	p=$TMP/$1
	if [ -L "$p" ]; then
		t=$(readlink "$p")
		case $t in /*) p=$TMP$t ;; *) p=$(dirname "$p")/$t ;; esac
	fi
	printf '%s' "$p"
}
SHELL_BIN=""
for c in bin/sh usr/bin/sh bin/dash usr/bin/dash; do
	q=$(resolve "$c")
	[ -f "$q" ] && { SHELL_BIN=$q; break; }
done
OSREL=""
for c in etc/os-release usr/lib/os-release; do
	q=$(resolve "$c")
	[ -f "$q" ] && { OSREL=$q; break; }
done

echo "init"
if have 'sbin/init' || have 'init' || have 'usr/lib/systemd/systemd' || have 'usr/sbin/init'; then
	ok "an init is present"
else
	bad "no /sbin/init, /init or /usr/lib/systemd/systemd - the kernel has nothing to execute"
fi

echo "architecture"
ARCH_OK=0
if [ -n "$SHELL_BIN" ] && [ -f "$SHELL_BIN" ]; then
	MACHINE=$(od -An -tx1 -j18 -N2 "$SHELL_BIN" | tr -d ' \n')
	if [ "$MACHINE" = "3e00" ]; then
		ok "the shell binary is x86-64"
		ARCH_OK=1
	elif [ -n "$MACHINE" ] && [ "$MACHINE" != "0000" ]; then
		bad "the shell is not x86-64 (e_machine 0x$MACHINE) - the PS4 cannot run it"
		ARCH_OK=1
	fi
fi
if [ "$ARCH_OK" = 0 ]; then
	# Merged-usr trees often ship /bin as a symlink and no shell of their own in
	# the paths we looked at, so fall back to the library directories, which are a
	# reliable architecture marker.
	if have 'usr/lib/x86_64-linux-gnu' || have 'lib/x86_64-linux-gnu'; then
		ok "x86-64 library directories present"
	elif have 'usr/lib/aarch64-linux-gnu' || have 'usr/lib/arm-linux-gnueabihf'; then
		bad "this is an ARM rootfs - the PS4 is x86-64"
	elif have 'usr/lib/i386-linux-gnu' || have 'lib/i386-linux-gnu'; then
		bad "this is a 32-bit rootfs - the PS4 needs x86-64"
	else
		warn "cannot confirm the architecture (no x86_64-linux-gnu library directory)"
	fi
fi

echo "distribution"
if [ -n "$OSREL" ]; then
	PRETTY=$(sed -n 's/^PRETTY_NAME=//p' "$OSREL" | tr -d '"' | head -1)
	ok "${PRETTY:-unnamed} (from /etc/os-release)"
else
	warn "no /etc/os-release - not fatal, just anonymous"
fi

echo "kernel modules"
MODS=$(sed -n 's|^\(\./\)\?lib/modules/\([^/]*\)/.*|\2|p' "$LIST" | sort -u | head -5)
if [ -n "$MODS" ]; then
	echo "$MODS" | sed 's/^/        /'
	if echo "$MODS" | grep -q '^7\.3\.4-PS4Linux'; then
		ok "modules match the PS4 kernel"
	else
		warn "modules are for a different kernel than 7.3.4-PS4Linux-baikal+ -"
		warn "the console boots the PS4 kernel, so these will not load"
	fi
else
	ok "no /lib/modules - nothing to mismatch"
fi

echo "things that should not be in a rootfs"
if grep -qE '^(\./)?boot/(vmlinuz|bzImage|vmlinuz-)' "$LIST"; then
	warn "/boot holds a kernel image - the console boots the PS4 kernel, this one is dead weight"
else
	ok "no kernel image in /boot"
fi

echo "graphics"
if grep -qE '^(\./)?opt/ps4-mesa/' "$LIST"; then
	ok "/opt/ps4-mesa is present"
else
	warn "no /opt/ps4-mesa - stock Mesa does not bind the PS4 GPU, expect llvmpipe"
	warn "see patches/ps4-mesa.patch and scripts/build-mesa.sh in ps4-linux-desktop"
fi

echo
if [ "$FAIL" = 1 ]; then
	echo "result: something has to be fixed before this can boot"
	exit 1
fi
[ "$WARN" = 1 ] && echo "result: nothing fatal, but read the warnings above"
[ "$WARN" = 0 ] && echo "result: all checks passed"
exit 0
