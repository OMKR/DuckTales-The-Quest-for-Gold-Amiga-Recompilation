#!/bin/sh
# Linux / macOS installer: runs src/tools/install.py with the system Python 3.
# Usage: ./install.sh [game folder containing DT1 and DT2]
# Needs: Python 3.8+ and SDL2 (Linux: e.g. libsdl2-2.0-0, macOS: brew install sdl2).
cd "$(dirname "$0")" || exit 1
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then
    echo "Python 3 is needed (e.g. sudo apt install python3, or brew install python)." >&2
    exit 1
fi
exec "$PY" src/tools/install.py "$@"
