#!/bin/sh
# Regenerate C from the original executable and build.
# Usage: ./build.sh [zig target]   e.g. x86_64-windows-gnu (default), x86_64-linux-gnu, aarch64-linux-gnu
set -e
set -o pipefail 2>/dev/null || true
cd "$(dirname "$0")"
TARGET=${1:-x86_64-windows-gnu}
SDL=tools/sdl/SDL2-2.30.9/x86_64-w64-mingw32
python -I tools/recomp.py files/DT1/DuckTales gen
mkdir -p build
case "$TARGET" in
  *windows*)
    python -m ziglang cc -target "$TARGET" -O2 -w -Iruntime -Igen -I$SDL/include/SDL2 \
      runtime/*.c gen/*.c $SDL/lib/libSDL2.dll.a -o build/ducktales.exe
    cp $SDL/bin/SDL2.dll build/
    # settings program (stand-alone, writes ducktales.ini)
    python -m ziglang cc -target "$TARGET" -O2 -w -Iruntime -I$SDL/include/SDL2 -mwindows       launcher/settings.c launcher/diskimage.c runtime/settings.c $SDL/lib/libSDL2.dll.a -o build/settings.exe
    cp tools/gw/greaseweazle-1.23/CAPSImg.dll build/
    cp launcher/CAPSImg-LICENCE.txt build/ ;;
  *)
    # needs SDL2 headers/libs for the target (e.g. via sdl2-config on that system)
    python -m ziglang cc -target "$TARGET" -O2 -w -Iruntime -Igen $(sdl2-config --cflags) \
      runtime/*.c gen/*.c $(sdl2-config --libs) -lm -o build/ducktales
    python -m ziglang cc -target "$TARGET" -O2 -w -Iruntime $(sdl2-config --cflags)       launcher/settings.c launcher/diskimage.c runtime/settings.c $(sdl2-config --libs) -ldl -o build/settings ;;
esac
