#!/bin/bash
# Copy this repository's own app into vendor/app and rename it for this build.
#
# The app source is in app/: the PS4 Linux installer with the distribution left
# out - same menu, same install, same Start Linux, same graphics-memory setting,
# same autoload handling, same recovery path. It is a copy of the app published
# in ps4-linux-desktop, kept here so this project builds on its own; nothing is
# downloaded from the other repository.
#
# Two things carry the name your package gets: APP_NAME in the source (the menu
# header) and TITLE in the Makefile (the package title). Both are rewritten in a
# working copy, so app/ stays exactly as published.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)

NAME=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$HERE/distro/NAME" | grep -m1 . || true)
[ -n "$NAME" ] || { echo "distro/NAME is empty - put one word in it, e.g. GNOME" >&2; exit 1; }
TITLE="PS4 $NAME Linux"

[ -f "$HERE/app/installer/main.c" ] || { echo "app/ is missing from this checkout" >&2; exit 1; }

rm -rf "$HERE/vendor"
mkdir -p "$HERE/vendor"
cp -a "$HERE/app" "$HERE/vendor/app"

APP=$HERE/vendor/app
# stb_truetype.h is third-party and not committed; fetch it into the working copy.
sh "$HERE/scripts/get-stb.sh" "$APP/ui"

sed -i "s|^#define APP_NAME .*|#define APP_NAME \"$TITLE\"|" "$APP/installer/main.c"
# Only the install branch of the Makefile: the else block is PS4LNX001. The remove
# branch belongs to the ChromeOS remover, which is not part of this build.
sed -i "/^else$/,/^endif$/ s|^TITLE       := .*|TITLE       := $TITLE|" "$APP/Makefile"

grep -qF "APP_NAME \"$TITLE\"" "$APP/installer/main.c" \
	|| { echo "could not rename the app source" >&2; exit 1; }
sed -n '/^else$/,/^endif$/p' "$APP/Makefile" | grep -qF "TITLE       := $TITLE" \
	|| { echo "could not rename the package title" >&2; exit 1; }

echo "vendor/app is this repository's app, named \"$TITLE\""
