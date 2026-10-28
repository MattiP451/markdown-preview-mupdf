#!/bin/sh
# Regenerate THIRD-PARTY-NOTICES.txt from a MuPDF source tree.
# Usage: sh gen-notices.sh /path/to/mupdf > THIRD-PARTY-NOTICES.txt
set -e
M=${1:?usage: sh gen-notices.sh /path/to/mupdf}
test -f "$M/include/mupdf/fitz.h" || { echo "not a MuPDF tree: $M" >&2; exit 1; }

VER=$(sed -n 's/^#define FZ_VERSION "\(.*\)"/\1/p' "$M/include/mupdf/fitz/version.h")
COMMIT=$(git -C "$M" rev-parse HEAD 2>/dev/null || echo unknown)
FTYEAR=$(sed -n 's/^CHANGES BETWEEN .* (\([0-9]*\)-.*/\1/p' "$M/thirdparty/freetype/docs/CHANGES" | head -1)

sect() { printf '\n\n%s\n%s\n\n' "================================================================" "$1"; }
file() { sed 's/[[:space:]]*$//' "$1"; }

cat <<HDR
THIRD-PARTY NOTICES for geany-plugin-mdpreview
==============================================

The mdpreview.so plugin statically links MuPDF $VER (git commit $COMMIT,
unmodified) and the third-party libraries and data files that MuPDF bundles.
This file reproduces their licence notices, as most of those licences require
for binary redistribution. It was generated from that MuPDF source tree with
packaging/gen-notices.sh.

Build assumed: MuPDF built with XCFLAGS="-fPIC -DTOFU_CJK" (all Noto fonts
and Charis SIL included, CJK fonts left out).

Overall: the plugin source is GPL-2.0-or-later, MuPDF is AGPL-3.0-or-later,
so the compiled plugin is distributed under the AGPL-3.0 (see AGPL-3.0.txt).
All components below are compatible with that.

Contents
  1.  MuPDF (Artifex Software, Inc.) ................ AGPL-3.0-or-later
  2.  FreeType ...................................... FreeType License (FTL)
  3.  HarfBuzz ...................................... MIT
  4.  cmark-gfm ..................................... BSD-2-Clause / MIT parts
  5.  lcms2 (lcms2mt) ............................... MIT
  6.  OpenJPEG ..................................... BSD-2-Clause
  7.  libjpeg (Independent JPEG Group) ............... IJG
  8.  zlib .......................................... zlib
  9.  Brotli ........................................ MIT
 10.  MuJS .......................................... ISC
 11.  Gumbo HTML parser ............................. Apache-2.0
 12.  jbig2dec ...................................... AGPL-3.0-or-later
 13.  extract ....................................... AGPL-3.0-or-later
 14.  Adobe CMap resources .......................... BSD-3-Clause
 15.  URW++ base-35 fonts ........................... OFL-1.1
 16.  Noto fonts (Google) ........................... OFL-1.1
 17.  Charis SIL font ............................... OFL-1.1
 18.  tex-hyphen hyphenation patterns ............... MIT / permissive
 19.  Artifex ICC colour profiles ................... part of MuPDF (AGPL)
HDR

sect "1. MuPDF"
cat <<EOT
Copyright (C) 2004-2026 Artifex Software, Inc.
Licensed under the GNU Affero General Public License, version 3 or (at your
option) any later version. Full text: AGPL-3.0.txt. Source: https://github.com/ArtifexSoftware/mupdf
Commercial licences are available from Artifex Software, Inc.
EOT

sect "2. FreeType"
cat <<EOT
Portions of this software are copyright (c) $FTYEAR The FreeType Project
(www.freetype.org). All rights reserved.

FreeType is used under the FreeType License (FTL); text follows.
EOT
printf '\n'; file "$M/thirdparty/freetype/docs/FTL.TXT"

sect "3. HarfBuzz"
file "$M/thirdparty/harfbuzz/COPYING"

sect "4. cmark-gfm"
file "$M/thirdparty/cmark-gfm/COPYING"

sect "5. lcms2 (lcms2mt)"
file "$M/thirdparty/lcms2/LICENSE"

sect "6. OpenJPEG"
file "$M/thirdparty/openjpeg/LICENSE"

sect "7. libjpeg (Independent JPEG Group)"
cat <<EOT
This software is based in part on the work of the Independent JPEG Group.

EOT
file "$M/thirdparty/libjpeg/README" | sed -n '/^LEGAL ISSUES$/,/^REFERENCES$/p' | sed '$d'

sect "8. zlib"
file "$M/thirdparty/zlib/LICENSE"

sect "9. Brotli"
file "$M/thirdparty/brotli/LICENSE"

sect "10. MuJS"
file "$M/thirdparty/mujs/COPYING"

sect "11. Gumbo HTML parser"
cat <<EOT
Copyright 2010 Google Inc. All Rights Reserved.
Licensed under the Apache License, Version 2.0. Full text:
/usr/share/common-licenses/Apache-2.0 on Debian systems, or
http://www.apache.org/licenses/LICENSE-2.0
EOT

sect "12. jbig2dec"
file "$M/thirdparty/jbig2dec/LICENSE"
echo "(Full AGPL-3.0 text: AGPL-3.0.txt)"

sect "13. extract"
head -12 "$M/thirdparty/extract/COPYING" | sed 's/[[:space:]]*$//'
echo "(AGPL-3.0, Artifex Software, Inc.; full text: AGPL-3.0.txt)"

sect "14. Adobe CMap resources"
sed -n 's/^%%Copyright: \{0,1\}//p' "$M/resources/cmaps/90ms-RKSJ-H" | sed '/^-*$/d' | sed 's/[[:space:]]*$//'

sect "15. URW++ base-35 fonts (Nimbus, Dingbats, Standard Symbols PS)"
file "$M/resources/fonts/urw/OFL.txt"

sect "16. Noto fonts (Google)"
cat <<EOT
The non-CJK Noto fonts (symbols, math, music, emoji and the per-script
fonts) are built in as fallback fonts. Copyright (c) The Noto Project Authors
and Google LLC; every font file carries its own copyright string. The CJK
fonts are not included in this build.
Used under the SIL Open Font License, Version 1.1:

EOT
file "$M/resources/fonts/noto/COPYING"

sect "17. Charis SIL"
file "$M/resources/fonts/sil/OFL.txt"

sect "18. tex-hyphen hyphenation patterns"
echo "Languages built in: $(unzip -Z1 "$M/resources/hyphen/hyph-std.zip" | tr '\n' ' ')"
echo "Source: https://github.com/hyphenation/tex-hyphen. Licence header of each pattern set:"
for l in $(unzip -Z1 "$M/resources/hyphen/hyph-std.zip"); do
  f=$(ls "$M"/resources/hyphen/license/hyph-"$l"*.info | head -1)
  printf '\n--- %s ---\n' "$(basename "$f")"
  sed -n '/^% title/,/^% language/p;/^% licence/,/^% hyphenmins/p' "$f" | sed '$d' | sed 's/^% \{0,1\}//' | sed 's/[[:space:]]*$//'
done

sect "19. Artifex ICC colour profiles"
echo "Copyright Artifex Software. Part of MuPDF, distributed under the AGPL-3.0-or-later."
