#!/bin/sh
# Pack the MuPDF source tree you built the plugin from, for attaching to the
# plugin's release page (this is what the AGPL "corresponding source" offer
# in the .deb points to).
# Usage: sh packaging/make-mupdf-source.sh [/path/to/mupdf]   (default ../../mupdf)
set -e
M=${1:-../../mupdf}
VER=$(sed -n 's/^#define FZ_VERSION "\(.*\)"/\1/p' "$M/include/mupdf/fitz/version.h")
COMMIT=$(git -C "$M" rev-parse HEAD | cut -c1-8)
OUT=$PWD/mupdf-$VER-$COMMIT-source.tar.xz
D=$(cd "$M" && pwd)
tar -C "$(dirname "$D")" --exclude='*/build' --exclude='.git' --transform "s,^$(basename "$D"),mupdf-$VER-$COMMIT," \
    -cJf "$OUT" "$(basename "$D")"
echo "$OUT"
