#!/bin/sh
# Creates dist/DuckTalesPort: a package without any of the game's code or
# data. The user imports their own disk images and builds the game locally
# (settings -> "Build game from disk files", or install.bat / install.sh).
set -e
cd "$(dirname "$0")"
./build.sh >/dev/null                      # up-to-date settings.exe
OUT=dist/DuckTalesPort
SDL=tools/sdl/SDL2-2.30.9/x86_64-w64-mingw32
rm -rf "$OUT"
mkdir -p "$OUT/src/tools" "$OUT/src/runtime" "$OUT/src/launcher" "$OUT/src/sdl/lib" "$OUT/licenses" "$OUT/files"

# programs and libraries that contain no game material
cp build/settings.exe build/SDL2.dll build/CAPSImg.dll "$OUT/"
cp dist_src/install.bat dist_src/install.sh LICENSE "$OUT/"
chmod +x "$OUT/install.sh"
cp dist_src/README.txt "$OUT/"
printf "# DuckTales settings
gamedir = files
" > "$OUT/ducktales.ini"

# recompiler and runtime sources
cp tools/recomp.py tools/analyze.py tools/disasm.py tools/m68k.py "$OUT/src/tools/"
cp dist_src/install.py "$OUT/src/tools/"
cp launcher/settings.c launcher/diskimage.c launcher/diskimage.h "$OUT/src/launcher/"
cp runtime/*.c runtime/*.h "$OUT/src/runtime/"
cp -r $SDL/include "$OUT/src/sdl/"
cp $SDL/lib/libSDL2.dll.a "$OUT/src/sdl/lib/"

# licences
cp launcher/CAPSImg-LICENCE.txt "$OUT/licenses/CAPSImg-LICENCE.txt"
cp tools/sdl/SDL2-2.30.9/LICENSE.txt "$OUT/licenses/SDL2-LICENSE.txt"
echo "font8x8_basic.h / font8x8_ext_latin.h: public domain (Daniel Hepper, IBM VGA fonts)" \
    > "$OUT/licenses/fonts.txt"

# Safety check: nothing derived from the game may be in the package.
if grep -rlE "f00_0[0-9a-f]{4}\(void\)|DuckTales.*Disney|Scrooge" "$OUT/src" >/dev/null 2>&1 ||
   find "$OUT" \( -name "*.arc" -o -name "*.adf" -o -name "*.ipf" -o -name "DuckTales" \
        -o -name "ducktales.exe" -o -name "node*.c" -o -name "analysis.json" \) | grep -q .; then
    echo "ERROR: game material found in $OUT" >&2
    exit 1
fi
echo "Package: $OUT ($(du -sh "$OUT" | cut -f1))"
