#!/bin/bash
# Get a working build toolchain with no root, no sudo, no Docker.
#
# This is the recipe behind PS4 XFCE Linux and ps4-os-builder: on a machine where
# sudo is blocked, every build dependency still arrives, because
#   * apt is given its own private state directory (Dir=...), so it never needs to
#     write to /var/lib/apt or /var/cache/apt and never needs root, and
#   * dpkg-deb -x unpacks the downloaded .debs into that directory instead of
#     installing them into /.
# Nothing outside PREFIX is touched, and nothing is installed system-wide.
#
#   bash bootstrap-toolchain.sh                  # -> ~/.cache/ps4-toolchain
#   bash bootstrap-toolchain.sh --prefix /opt/tc # somewhere else
#   bash bootstrap-toolchain.sh --no-llvm        # skip the compiler (already have one)
#   source ~/.cache/ps4-toolchain/env.sh         # then: clang, pahole, flex, bison, bc
#
# What it sets up
#   PREFIX/host   apt packages for this machine's own distribution, unpacked:
#                 flex, bison, bc, pahole, zstd, cpio, kmod, libelf-dev,
#                 libssl-dev, libncurses-dev, rsync, xz-utils, file
#   PREFIX/llvm   the official LLVM release tarball: clang, ld.lld, llvm-ar,
#                 llvm-objcopy, llvm-nm, llvm-strip, llvm-readelf
#   PREFIX/bin    wrappers, so each tool finds its own libraries
#   PREFIX/env.sh PATH + the few variables the build needs
#
# Three traps this handles, all of which cost real time to find:
#   1. `apt-get download` fails without root: the package lists live in the root
#      state directory. Use `--download-only install` with Dir= instead - that
#      resolves dependencies too, which `download` does not.
#   2. llvm-ar / llvm-objcopy are NOT in a clang-only install (conda's clang, or a
#      distro's clang package). The kernel's BTF and archive steps need them. The
#      official LLVM tarball has all of them; that is why it is used here.
#   3. Never put a target sysroot on LD_LIBRARY_PATH. Host tools then load the
#      sysroot's glibc and crash with stack smashing. Wrappers here only ever add
#      PREFIX/host's own libraries, and only for the tools that need them.
set -euo pipefail

PREFIX=${PREFIX:-$HOME/.cache/ps4-toolchain}
LLVM_VER=${LLVM_VER:-18.1.8}
LLVM_TARBALL=${LLVM_TARBALL:-clang+llvm-$LLVM_VER-x86_64-linux-gnu-ubuntu-18.04}
DO_LLVM=1

while [ $# -gt 0 ]; do
	case $1 in
		--prefix) PREFIX=$2; shift 2 ;;
		--no-llvm) DO_LLVM=0; shift ;;
		-h|--help) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

HOST=$PREFIX/host
BIN=$PREFIX/bin
mkdir -p "$HOST"/{etc/apt,var/lib/dpkg,var/cache/apt/archives/partial} "$BIN"

say() { printf '\n== %s\n' "$*"; }

# --- 1. which packages does this machine's own distribution have --------------
. /etc/os-release
ID=${ID:-debian}
CODENAME=${VERSION_CODENAME:-${UBUNTU_CODENAME:-stable}}
case "$ID" in
	ubuntu) MIRROR=${MIRROR:-http://archive.ubuntu.com/ubuntu}; COMPONENTS="main universe" ;;
	*)      MIRROR=${MIRROR:-http://deb.debian.org/debian};     COMPONENTS="main contrib" ;;
esac
say "host: $PRETTY_NAME ($CODENAME), mirror $MIRROR"

# Host tools must run on THIS machine, so they come from THIS release. A package
# from a newer release links a newer glibc and will not start.
{
	for c in $COMPONENTS; do echo "deb [trusted=yes] $MIRROR $CODENAME $c"; done
	for c in $COMPONENTS; do echo "deb [trusted=yes] $MIRROR $CODENAME-updates $c"; done
} > "$HOST/etc/apt/sources.list"
: > "$HOST/var/lib/dpkg/status"

# [trusted=yes]: this scratch tree has no keyring of its own. Packages still come
# from the distribution's own mirror; nothing is verified by a third party.
APT="apt-get -o Dir=$HOST -o Dir::Etc::sourcelist=$HOST/etc/apt/sources.list
     -o Dir::Etc::sourceparts=/dev/null -o Dir::State::status=$HOST/var/lib/dpkg/status
     -o Dir::Cache=$HOST/var/cache/apt -o Dir::State=$HOST/var/lib/apt
     -o APT::Get::List-Cleanup=0 -o Acquire::Retries=3"

WANT=${WANT:-flex bison bc pahole zstd cpio kmod libelf-dev libssl-dev libncurses-dev rsync xz-utils file}
say "fetching host tools: $WANT"
# shellcheck disable=SC2086
$APT update -qq 2>&1 | grep -vE 'GPG error|public key is not available|preferences' || true
# shellcheck disable=SC2086
$APT -y -qq --download-only --no-install-recommends install $WANT || {
	echo "could not download every package; the list above says which" >&2
}
for d in "$HOST"/var/cache/apt/archives/*.deb; do
	[ -f "$d" ] && dpkg-deb -x "$d" "$HOST"
done

# --- 2. wrappers ---------------------------------------------------------------
# Debian is merged-usr; a tree unpacked this way has usr/lib but not lib.
mkdir -p "$HOST/usr/lib64"
[ -e "$HOST/lib" ] || ln -sfn usr/lib "$HOST/lib"
[ -e "$HOST/lib64" ] || ln -sfn usr/lib64 "$HOST/lib64"

LIBS=$HOST/usr/lib/x86_64-linux-gnu:$HOST/lib/x86_64-linux-gnu:$HOST/usr/lib:$HOST/lib
for d in "$HOST"/usr/bin "$HOST"/bin "$HOST"/usr/sbin "$HOST"/sbin; do
	[ -d "$d" ] || continue
	for t in "$d"/*; do
		[ -f "$t" ] && [ -x "$t" ] || continue
		n=$(basename "$t")
		[ -e "$BIN/$n" ] && continue
		printf '#!/bin/sh\nLD_LIBRARY_PATH=%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} exec %s "$@"\n' \
			"$LIBS" "$t" > "$BIN/$n"
		chmod +x "$BIN/$n"
	done
done
# bison and flex want their data files from the same tree they came from.
echo "export BISON_PKGDATADIR=$HOST/usr/share/bison" >> "$PREFIX/env.sh.tmp"
echo "export M4=$BIN/m4" >> "$PREFIX/env.sh.tmp"

# --- 3. the compiler -----------------------------------------------------------
if [ "$DO_LLVM" = 1 ] && [ ! -x "$PREFIX/llvm/bin/clang" ]; then
	say "fetching LLVM $LLVM_VER (official tarball: clang, lld, llvm-ar, llvm-objcopy)"
	mkdir -p "$PREFIX/llvm"
	curl -fL --retry 3 "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$LLVM_TARBALL.tar.xz" \
		| tar -xJ -C "$PREFIX/llvm" --strip-components=1
fi

# The ubuntu-18.04 LLVM build links libtinfo.so.5, which no current release ships.
# If clang will not start, borrow it from a release that still had it.
if [ "$DO_LLVM" = 1 ] && [ -x "$PREFIX/llvm/bin/clang" ] && ! "$PREFIX/llvm/bin/clang" --version >/dev/null 2>&1; then
	say "clang needs libtinfo.so.5 - fetching it"
	T=$PREFIX/libtinfo5
	mkdir -p "$T/etc/apt" "$T/var/lib/dpkg" "$T/var/cache/apt/archives/partial"
	printf 'deb [trusted=yes] http://deb.debian.org/debian bullseye main\n' > "$T/etc/apt/sources.list"
	: > "$T/var/lib/dpkg/status"
	TAPT="apt-get -o Dir=$T -o Dir::Etc::sourcelist=$T/etc/apt/sources.list -o Dir::Etc::sourceparts=/dev/null -o Dir::State::status=$T/var/lib/dpkg/status -o Dir::Cache=$T/var/cache/apt -o Dir::State=$T/var/lib/apt -o APT::Get::List-Cleanup=0"
	# shellcheck disable=SC2086
	$TAPT update -qq 2>/dev/null || true
	# shellcheck disable=SC2086
	$TAPT -y -qq --download-only install libtinfo5 2>/dev/null || true
	for d in "$T"/var/cache/apt/archives/*.deb; do [ -f "$d" ] && dpkg-deb -x "$d" "$T"; done
	# Copy out libtinfo ONLY. Unpacking libtinfo5 also drags in that release's
	# libc6, and putting that on LD_LIBRARY_PATH makes every tool on the machine
	# load an older glibc and die - the exact trap this script exists to avoid.
	mkdir -p "$PREFIX/libtinfo5-libs"
	for f in "$T"/lib/x86_64-linux-gnu/libtinfo.so.5* "$T"/usr/lib/x86_64-linux-gnu/libtinfo.so.5*; do
		[ -e "$f" ] && cp -a "$f" "$PREFIX/libtinfo5-libs/"
	done
	LLVMLIBS=$PREFIX/libtinfo5-libs
fi

# LLVM's own binaries go through wrappers too, so only they see libtinfo5.
if [ "$DO_LLVM" = 1 ] && [ -d "$PREFIX/llvm/bin" ]; then
	for t in "$PREFIX"/llvm/bin/*; do
		[ -f "$t" ] && [ -x "$t" ] || continue
		n=$(basename "$t")
		[ -e "$BIN/$n" ] && continue
		if [ -n "${LLVMLIBS:-}" ]; then
			printf '#!/bin/sh\nLD_LIBRARY_PATH=%s${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} exec %s "$@"\n' \
				"$LLVMLIBS" "$t" > "$BIN/$n"
			chmod +x "$BIN/$n"
		else
			ln -sf "$t" "$BIN/$n"
		fi
	done
fi

# --- 4. env --------------------------------------------------------------------
# Deliberately no LD_LIBRARY_PATH here: every tool that needs one has a wrapper.
# A global LD_LIBRARY_PATH is how a build box ends up loading the wrong libc.
{
	echo "# generated by bootstrap-toolchain.sh - source me"
	echo "export PATH=$BIN:\$PATH"
	echo "export BISON_PKGDATADIR=$HOST/usr/share/bison"
	echo "export M4=$BIN/m4"
} > "$PREFIX/env.sh"
rm -f "$PREFIX/env.sh.tmp"

# --- 5. say what actually works ------------------------------------------------
say "checking"
export PATH="$BIN:$PATH"
export BISON_PKGDATADIR="$HOST/usr/share/bison"
FAIL=0
TOOLS="flex bison bc pahole zstd cpio depmod"
[ "$DO_LLVM" = 1 ] && TOOLS="clang ld.lld llvm-ar llvm-objcopy llvm-nm llvm-strip $TOOLS"
for t in $TOOLS; do
	if command -v "$t" >/dev/null 2>&1 && "$t" --version >/dev/null 2>&1; then
		printf '  ok    %-14s %s\n' "$t" "$("$t" --version 2>/dev/null | head -1 | cut -c1-40)"
	else
		printf '  MISS  %s\n' "$t"
		FAIL=1
	fi
done

echo
echo "prefix:  $PREFIX"
echo "use it:  source $PREFIX/env.sh"
if [ "$FAIL" = 1 ]; then
	echo
	echo "Something is missing. Two things to try:"
	echo "  * if it is a host tool, re-run with WANT=\"...\" to add packages;"
	echo "  * if it is an LLVM tool, this release did not carry it - set"
	echo "    LLVM_VER=19.1.7 (or newer) and delete $PREFIX/llvm, then re-run."
	exit 1
fi
echo "A kernel build from here:  make LLVM=1 olddefconfig && make LLVM=1 -j\$(nproc)"
