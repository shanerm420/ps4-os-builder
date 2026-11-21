#!/bin/sh
# Fetch stb_truetype.h, the third-party header the app rasterises its text with.
#
# It is ~200 KB of code nobody edits, so it is not committed. vendor.sh runs this
# for you; run it by hand if you build app/ directly.
#
#   scripts/get-stb.sh [destination-dir]      default: app/ui
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-$HERE/app/ui}
URL=https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h

if [ -f "$DEST/stb_truetype.h" ]; then
	echo "have $DEST/stb_truetype.h"
	exit 0
fi

mkdir -p "$DEST"
if curl -fsSL "$URL" -o "$DEST/stb_truetype.h"; then
	echo "got $DEST/stb_truetype.h"
else
	rm -f "$DEST/stb_truetype.h"
	echo "could not fetch $URL" >&2
	exit 1
fi
