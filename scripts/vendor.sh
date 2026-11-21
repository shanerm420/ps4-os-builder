#!/bin/bash
# Fetch the PS4 app source and rename it for this build.
#
# The app is the same code that ships as PS4 XFCE Linux; only its name changes, so
# the home screen says which distribution this package actually carries. Two lines
# carry that name: APP_NAME in the source (the menu header) and TITLE in the
# Makefile (the package title).
#
# PIN is the release the code is taken from. Bump it when the other repository
# publishes a new one; nothing else here depends on the version.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
PIN=${PIN:-v1.0.0}
REPO=${REPO:-https://github.com/shanerm420/ps4-linux-desktop}

NAME=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$HERE/distro/NAME" | grep -m1 . || true)
[ -n "$NAME" ] || { echo "distro/NAME is empty - put one word in it, e.g. GNOME" >&2; exit 1; }
TITLE="PS4 $NAME Linux"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "fetching $REPO at $PIN ..."
curl -fsSL "$REPO/archive/refs/tags/$PIN.tar.gz" -o "$TMP/src.tar.gz"
tar -xzf "$TMP/src.tar.gz" -C "$TMP"
SRC=$(find "$TMP" -maxdepth 1 -type d -name 'ps4-linux-desktop-*' | head -1)
[ -n "$SRC" ] || { echo "the archive did not contain ps4-linux-desktop" >&2; exit 1; }

rm -rf "$HERE/vendor"
mkdir -p "$HERE/vendor"
cp -a "$SRC/pkg/installer" "$HERE/vendor/app"

APP=$HERE/vendor/app
sed -i "s|^#define APP_NAME .*|#define APP_NAME \"$TITLE\"|" "$APP/installer/main.c"
# Only the install branch of the Makefile: the else block is PS4LNX001. The remove
# branch belongs to the ChromeOS remover, which is not part of this build.
sed -i "/^else$/,/^endif$/ s|^TITLE       := .*|TITLE       := $TITLE|" "$APP/Makefile"

grep -qF "APP_NAME \"$TITLE\"" "$APP/installer/main.c" \
	|| { echo "could not rename the app source" >&2; exit 1; }
sed -n '/^else$/,/^endif$/p' "$APP/Makefile" | grep -qF "TITLE       := $TITLE" \
	|| { echo "could not rename the package title" >&2; exit 1; }

echo "vendor/app is $PIN, named \"$TITLE\""
