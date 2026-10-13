DuckTales: The Quest for Gold (Amiga, 1990) - native port
=========================================================

This package contains NO part of the game. You need your own copy of the
original Amiga disks as disk images (.ipf or .adf). The game is converted
and compiled on your own computer from your own copy.

Supported: Windows (x64; ARM64 through x64 emulation), Linux (x86_64,
aarch64) and macOS (Intel, Apple Silicon).

Getting started
---------------
Windows:
1. Run settings.exe (included). Drop your two disk images onto its window
   (or choose "Import disk image"). "Game folder" turns green when found.
2. Choose "Build game from disk files". A console window runs install.bat.
3. Choose "Save and start game".

Linux / macOS:
1. Install SDL2 if needed:
     Debian/Ubuntu: sudo apt install libsdl2-2.0-0
     Fedora:        sudo dnf install SDL2
     macOS:         brew install sdl2
   Python 3.8 or newer is also needed (usually installed).
2. In a terminal:  ./install.sh
   This builds the settings program.
3. Run ./settings, import your disk images and choose
   "Build game from disk files" (or run ./install.sh again).
4. Choose "Save and start game".

What the installer does
-----------------------
On the first run it downloads the build tools into the toolchain folder
(about 100 MB) from their official sites and verifies their checksums:
  Zig 0.16.0 (C compiler)       https://ziglang.org
  Capstone 5.0.7                https://pypi.org/project/capstone
  Python 3.12.10 (Windows only) https://www.python.org
It then converts the game's program into C and compiles the game. This
takes a few minutes. Run it again whenever you change the game folder.

Controls
--------
Arrow keys and Space as in the game's manual. A game controller works as the
Amiga joystick. PgUp/PgDn = game speed, Ctrl+PgUp/PgDn = music rate,
F11 = fullscreen, F12 = screenshot. Other options are in the settings program.

Notes
-----
- The game folder, the build_tmp folder and the ducktales program are made
  from your copy of the game. Do not share them.
- IPF images are read with the SPS decoder library: CAPSImg.dll on Windows
  (included); on Linux/macOS install libcapsimage if you need IPF, or use
  ADF images. The library is licensed for non-commercial use only
  (licenses/CAPSImg-LICENCE.txt). This package must not be sold.
- The source code of this project is under the MIT licence (LICENSE). The
  licences of included components are in the licenses folder.
