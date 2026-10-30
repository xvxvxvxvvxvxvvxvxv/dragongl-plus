#!/bin/sh
# DragonGL+ - portable package builder
# Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
#
# Turns a finished build directory into a ready-to-play archive:
#
#   tools/package.sh <build-dir> <linux|windows> [out-dir]
#
#   DragonGL-Plus-<platform>/
#     DragonGL-Plus(.exe)     the game (client); double-click to play
#     dragongl_server(.exe)   dedicated server (also started by "Play Solo")
#     data/                   game data (bestiary, items, spells)
#     README-PLAY.txt         how to play (EN/IT)
#     LICENSE.txt, licenses/  GPL-3.0 + third-party licenses
#
# The binaries are self-contained: fonts, shaders, icon and the whole
# soundtrack are inside the executable; on Windows the C runtime is
# linked statically, on Linux only glibc >= 2.27 and the system graphics
# driver are needed.
set -eu

BUILD=${1:?usage: package.sh <build-dir> <linux|windows> [out-dir]}
PLATFORM=${2:?usage: package.sh <build-dir> <linux|windows> [out-dir]}
OUT=${3:-dist}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^#define DRAGONGL_VERSION_STR "\([0-9.]*\).*"/\1/p' "$ROOT/src/client/screens.c")
VERSION=${VERSION:-dev}

case "$PLATFORM" in
linux)   EXE=""; ARCH=linux-x86_64 ;;
windows) EXE=".exe"; ARCH=windows-x64 ;;
*) echo "unknown platform: $PLATFORM" >&2; exit 1 ;;
esac

NAME="DragonGL-Plus-$ARCH"   # stable name: releases/latest/download/<name> links
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/data" "$STAGE/licenses"

cp "$BUILD/dragongl_client$EXE" "$STAGE/DragonGL-Plus$EXE"
echo "$VERSION" > "$STAGE/VERSION.txt"
cp "$BUILD/dragongl_server$EXE" "$STAGE/dragongl_server$EXE"
cp "$ROOT/data/bestiary.json" "$ROOT/data/items.json" "$ROOT/data/spells.json" "$STAGE/data/"
cp "$ROOT/res/README-PLAY.txt" "$STAGE/"
cp "$ROOT/LICENSE.txt" "$STAGE/"

T="$ROOT/third_party"
cp "$T/glfw/LICENSE.md"               "$STAGE/licenses/GLFW-zlib.md"
cp "$T/volk/LICENSE.md"               "$STAGE/licenses/volk-MIT.md"
cp "$T/vulkan/LICENSE.md"             "$STAGE/licenses/Vulkan-Headers.md"
cp "$T/vulkan/LICENSE-Apache-2.0.txt" "$STAGE/licenses/Vulkan-Headers-Apache-2.0.txt"
cp "$T/vulkan/LICENSE-MIT.txt"        "$STAGE/licenses/Vulkan-Headers-MIT.txt"
cp "$T/miniaudio/LICENSE"             "$STAGE/licenses/miniaudio.txt"
cp "$T/wepoll/LICENSE"                "$STAGE/licenses/wepoll-BSD-2.txt"
cp "$T/fonts/OFL-Cinzel.txt"          "$STAGE/licenses/Font-Cinzel-OFL.txt"
cp "$T/fonts/OFL-AlegreyaSans.txt"    "$STAGE/licenses/Font-AlegreyaSans-OFL.txt"
# cJSON: the MIT notice is the header comment of the source file
sed -n '2,/^\*\//p' "$ROOT/src/shared/cJSON.c" | sed '$d' | sed 's/^  //' > "$STAGE/licenses/cJSON-MIT.txt"
cat > "$STAGE/licenses/stb.txt" <<'EOF'
stb_truetype.h and stb_image_write.h by Sean Barrett and contributors
(https://github.com/nothings/stb) - public domain (Unlicense) or MIT,
at your choice. The full license text is at the end of each header in
the DragonGL source tree (third_party/stb/).
EOF

cd "$OUT"
if [ "$PLATFORM" = linux ]; then
    strip "$STAGE/DragonGL-Plus" "$STAGE/dragongl_server" 2>/dev/null || true
    chmod 755 "$STAGE/DragonGL-Plus" "$STAGE/dragongl_server"
    # desktop entry + icon, for users who want a menu shortcut
    cp "$ROOT/res/dragongl.png" "$STAGE/dragongl.png"
    cat > "$STAGE/install-desktop-shortcut.sh" <<'EOF'
#!/bin/sh
# Adds Dragon GL to the applications menu of the current user.
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/dragongl-plus.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=DragonGL+
Comment=The Long Descent into the Dragon's Maw
Exec="$HERE/DragonGL-Plus"
Path=$HERE
Icon=$HERE/dragongl.png
Categories=Game;RolePlaying;
Terminal=false
EOD
echo "DragonGL+ added to your applications menu."
EOF
    chmod 755 "$STAGE/install-desktop-shortcut.sh"
    tar -czf "$NAME.tar.gz" "$NAME"
    echo "$OUT/$NAME.tar.gz"
else
    x86_64-w64-mingw32-strip "$STAGE/DragonGL-Plus.exe" "$STAGE/dragongl_server.exe" 2>/dev/null || true
    # the C runtime and winpthreads are linked statically: ship their notices
    cp "$T/mingw-w64/COPYING.MinGW-w64-runtime.txt" "$STAGE/licenses/MinGW-w64-runtime.txt"
    # Windows users expect CRLF in text files opened with Notepad
    for f in "$STAGE/README-PLAY.txt" "$STAGE/LICENSE.txt"; do
        sed 's/$/\r/' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
    done
    rm -f "$NAME.zip"
    if command -v zip >/dev/null 2>&1; then
        zip -qr9 "$NAME.zip" "$NAME"
    else
        python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[1])" "$NAME"
    fi
    echo "$OUT/$NAME.zip"
fi
