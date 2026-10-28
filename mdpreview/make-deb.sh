#!/bin/sh
# Build a .deb of the Markdown Preview (MuPDF) Geany plugin.
#
# Run it after compiling the plugin:
#   make -f Makefile.standalone MUPDF=/path/to/mupdf
#   MUPDF=/path/to/mupdf sh make-deb.sh        (default MUPDF: ../../mupdf)
# Then:
#   sudo apt install ./geany-plugin-mdpreview_*.deb
#
# MUPDF is the MuPDF source tree the plugin was built with. It is needed to
# record the exact MuPDF version and commit in the package, because the AGPL
# requires you to say where the corresponding source can be found.
# Without a git checkout, set MUPDF_VERSION and MUPDF_COMMIT yourself.
set -e
cd "$(dirname "$0")"

PKG=geany-plugin-mdpreview
VERSION=${VERSION:-0.4}
MAINTAINER=${MAINTAINER:-"Matti Palmström <sjukfan@gmail.com>"}
HOMEPAGE=${HOMEPAGE:-https://github.com/MattiP451/markdown-preview-mupdf}
MUPDF=${MUPDF:-../../mupdf}

die() { echo "make-deb.sh: $*" >&2; exit 1; }

test -f mdpreview.so || die "mdpreview.so not found. Build it first:  make -f Makefile.standalone MUPDF=/path/to/mupdf"
for f in README NEWS AUTHORS ChangeLog COPYING packaging/copyright packaging/AGPL-3.0.txt packaging/THIRD-PARTY-NOTICES.txt; do
	test -f "$f" || die "missing $f"
done

# --- which MuPDF is inside the .so? ---
if test -z "$MUPDF_VERSION" && test -f "$MUPDF/include/mupdf/fitz/version.h"; then
	MUPDF_VERSION=$(sed -n 's/^#define FZ_VERSION "\(.*\)"/\1/p' "$MUPDF/include/mupdf/fitz/version.h")
fi
if test -z "$MUPDF_COMMIT" && test -d "$MUPDF/.git"; then
	MUPDF_COMMIT=$(git -C "$MUPDF" rev-parse HEAD)
	if test -n "$(git -C "$MUPDF" status --porcelain --untracked-files=no 2>/dev/null)"; then
		die "$MUPDF has local changes. The AGPL source offer must match what was built; commit them or build a clean checkout (and publish the changes)."
	fi
fi
test -n "$MUPDF_VERSION" && test -n "$MUPDF_COMMIT" || die "cannot tell which MuPDF was used. Set MUPDF=/path/to/mupdf (a git checkout) or MUPDF_VERSION and MUPDF_COMMIT."
MUPDF_SHORT=$(echo "$MUPDF_COMMIT" | cut -c1-8)
FTYEAR=${FTYEAR:-$(sed -n 's/^Portions of this software are copyright (c) \([0-9]*\) .*/\1/p' packaging/THIRD-PARTY-NOTICES.txt | head -1)}

ARCH=$(dpkg --print-architecture)
LIBDIR=$(pkg-config --variable=libdir geany 2>/dev/null || true)
test -n "$LIBDIR" || LIBDIR=/usr/lib/$(gcc -print-multiarch)
GEANY_VER=$(dpkg-query -W -f='${Version}' geany | sed 's/-[^-]*$//')
# Debian's geany package "Provides: geany-abi-NNNNN"; a plugin only loads
# into a Geany with the same ABI, so depend on exactly that.
ABI=$(dpkg-query -W -f='${Provides}' geany | tr ',' '\n' | grep -o 'geany-abi-[0-9]*' || true)

ROOT=debian-build/${PKG}_${VERSION}_${ARCH}
DOC=$ROOT/usr/share/doc/$PKG
rm -rf debian-build
mkdir -p "$ROOT/DEBIAN" "$ROOT$LIBDIR/geany" "$DOC"

install -m 644 mdpreview.so "$ROOT$LIBDIR/geany/mdpreview.so"
strip --strip-unneeded "$ROOT$LIBDIR/geany/mdpreview.so"

install -m 644 README NEWS AUTHORS "$DOC/"
install -m 644 packaging/AGPL-3.0.txt "$DOC/"
gzip -9n -c ChangeLog > "$DOC/changelog.gz"
# minimal Debian-format changelog for the package itself
{
	echo "$PKG ($VERSION) unstable; urgency=low"
	echo
	echo "  * Built with MuPDF $MUPDF_VERSION ($MUPDF_SHORT)."
	echo
	echo " -- $MAINTAINER  $(date -R)"
} | gzip -9n > "$DOC/changelog.Debian.gz"
gzip -9n -c packaging/THIRD-PARTY-NOTICES.txt > "$DOC/THIRD-PARTY-NOTICES.txt.gz"
sed -e "s|@HOMEPAGE@|$HOMEPAGE|g" -e "s|@VERSION@|$VERSION|g" \
    -e "s|@MUPDF_VERSION@|$MUPDF_VERSION|g" -e "s|@MUPDF_COMMIT@|$MUPDF_COMMIT|g" \
    -e "s|@MUPDF_SHORT@|$MUPDF_SHORT|g" -e "s|@FTYEAR@|$FTYEAR|g" \
    packaging/copyright > "$DOC/copyright"
chmod 644 "$DOC/copyright"

DEPENDS="geany (>= $GEANY_VER), libgtk-3-0t64 | libgtk-3-0, libc6"
test -n "$ABI" && DEPENDS="$DEPENDS, $ABI"
SIZE=$(du -sk "$ROOT" | cut -f1)

cat > "$ROOT/DEBIAN/control" <<CTL
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $SIZE
Depends: $DEPENDS
Section: editors
Priority: optional
Homepage: $HOMEPAGE
Description: Markdown preview for Geany, rendered with MuPDF
 Shows a live preview of the current Markdown document in Geany's
 sidebar. Re-flows on resize, shows local images, follows links to
 other Markdown files, supports text selection and custom CSS.
 MuPDF is built into the plugin; no WebKit needed.
 .
 Licence: because MuPDF (AGPL-3.0-or-later) is linked into the plugin,
 this binary package is distributed under the AGPL-3.0. Source:
 $HOMEPAGE
CTL

dpkg-deb --root-owner-group --build "$ROOT" .
rm -rf debian-build
ls -l ${PKG}_${VERSION}_${ARCH}.deb
