# DuckTales: The Quest for Gold (Amiga, 1990) – native port

The original 68000 program is statically recompiled to portable C and runs on
an SDL2 runtime that emulates the parts of AmigaOS and the custom chips the
game uses. The same source builds for Windows x64, Linux, macOS and ARM.

The game data (`files/`, `adf/`, `disasm/`) and the generated C (`gen/`) are
derived from Disney's copyrighted game and must not be distributed.

## Quick start

1. Run `build/settings.exe`.
2. Drop the two disk images (`.adf` or `.ipf`) onto its window, or choose
   **Import disk image** and type a path. The files are extracted into the
   game folder as `DT1/` and `DT2/`. **Game folder** turns green when found.
3. Choose **Save and start game**.

Or run the game directly: `build/ducktales.exe ../files`.

## Controls

| Input | Action |
|---|---|
| Arrow keys, Space | Amiga keyboard, as in the game's manual (or the joystick with Keyboard mode = joystick: arrows = directions, Space/Ctrl = fire; other keys stay keys) |
| Game controller | Amiga joystick in port 2 (see below) |
| PgUp / PgDn, keypad + / − | Game speed (fps) ±5, with Shift ±1 |
| Ctrl + PgUp / PgDn | Music rate (Hz) ±5, with Shift ±1 |
| F11 | Toggle fullscreen |
| F12 | Screenshot (`screenshot.bmp`) |

In the office, hold a direction and press fire: up = money bin, left = stock
portfolio, right = security system → world map.

The copy protection screen needs the code wheel chart from the original
manual, unless **Copy protection** is turned off in the settings.

**Game controller** (XInput / any SDL GameController): left stick (with dead
zone) or D-pad = directions; A, B, X, Y or the triggers = fire; LB/RB change
the emulator's fps only. Pure joystick logic: nothing is mapped to keys.
Controllers can be connected and removed while playing. Keyboard and
joystick are separate devices; the game reads both itself.

## Settings

`build/settings.exe` (source `launcher/settings.c`) edits `ducktales.ini` next
to the executables and can start the game.

| Setting | Range | Default |
|---|---|---|
| Game speed (fps) | 1–200 | 25 (50 = original PAL speed) |
| Music rate (Hz) | 1–200 | 50 (original tempo) |
| Sound | on/off | on |
| Volume | 0–100 % | 100 % |
| Fullscreen | yes/no | no |
| Window size | 1x–5x (320x200 … 1600x1000) | 3x |
| Controller dead zone | 5–90 % | 40 % |
| Keyboard mode | keyboard/joystick | keyboard |
| Copy protection | on/off | on |
| Game folder | path | `../files` |

Precedence in the game: defaults < `ducktales.ini` < environment variables <
command line. Command line: `--fps N`, `--music-hz N`, game folder as a plain
argument. Environment: `DT_FPS`, `DT_MUSIC_HZ`, `DT_NOSOUND=1`. A relative
game folder is looked up from the current directory, then from the program's
folder. Shared code: `runtime/settings.c`.

The game's timing counts frames, so fps is the game speed. Audio is generated
in real time, so pitch never changes. The music player (`0x58bc`, called
from the vertical blank server) is gated (`GATED` in `tools/recomp.py`) and
the runtime calls it at the music rate (`music_tick`), so the music tempo is
independent of fps. Vsync is used only up to 60 fps.

### Disk image import

`launcher/diskimage.c`; also `settings.exe --extract <image> <folder>`.
Extracts into `<folder>/<volume name>/` (DT1, DT2), replacing existing files.

- **ADF** is read directly with an AmigaDOS (OFS/FFS) reader.
- **IPF** is decoded with the SPS decoder library `CAPSImg.dll`, loaded at
  run time. The raw MFM tracks are decoded into AmigaDOS sectors here, with
  checksums verified. The CAPS structures are packed (`#pragma pack(1)`).
  `build.sh` copies the DLL and `CAPSImg-LICENCE.txt` into `build/`; the
  licence allows non-commercial use and distribution only. Without the DLL,
  ADF import still works.
- Verified: both ADF and IPF produce files byte-identical to the original
  extraction (DT1 27 files, DT2 12 files).

Fonts: public domain `font8x8_basic.h` (replaces the Amiga ROM font topaz)
and `font8x8_ext_latin.h`.

## Distribution package

`./make_dist.sh` creates `dist/DuckTalesPort/` (~6 MB) **without any of the
game's code or data**: `settings.exe`, `SDL2.dll`, `CAPSImg.dll`,
`install.bat`, `install.sh`, `README.txt`, `LICENSE`, `ducktales.ini`
(`gamedir = files`), the recompiler, runtime and launcher sources (`src/`),
SDL2 headers/import library and `licenses/`. The script refuses to finish if
game material is found.

**Installer** (`dist_src/install.py`, run by `install.bat` on Windows or
`install.sh` on Linux/macOS) – works on Windows x64/ARM64, Linux
x86_64/aarch64, macOS Intel/Apple Silicon:
1. Downloads Zig 0.16.0 for the host (pinned SHA-256 per platform) and the
   matching Capstone 5.0.7 wheel (SHA-256 from PyPI) into `toolchain/`.
   `install.bat` first fetches embeddable Python 3.12.10 (pinned SHA-256);
   Linux/macOS use the system Python 3.
2. Finds SDL2: bundled on Windows; `pkg-config sdl2`, Homebrew, or the
   installed `libSDL2-2.0.so.0` with the bundled headers (`SDL2_LIBDIR`
   overrides).
3. Builds the settings program, then – if `<game folder>/DT1/DuckTales`
   exists – recompiles it into `build_tmp/gen` and builds the game.
The settings program's **Build game from disk files** runs the installer.

Tested in clean copies: Windows (IPF import → install 153 s incl. downloads
→ game reaches the flight) and Linux x86_64 in WSL Ubuntu 24.04 (SDL2 unpacked
locally; same route, same result). macOS and ARM were not tested.

Determinism: Capstone builds differ for invalid words (Windows returns
nothing, Linux a `dc.w` pseudo instruction); `analyze.py` treats both as
undecodable, so every platform finds the same 515 functions.

The project's own code is MIT licensed (`LICENSE`, which also states that
the game and anything made from it are not covered). The copy protection
setting is still included; consider removing it before publishing.

## Building

    ./build.sh                       # Windows x64 (zig cc + SDL2), output in build/
    ./build.sh x86_64-linux-gnu      # other targets need SDL2 for the target

`build.sh` regenerates the C from the original executable, then builds
`ducktales.exe` and `settings.exe`. Toolchain: `zig cc` (pip `ziglang`), SDL2
2.30 MinGW package in `tools/sdl/`.

Pipeline: `tools/analyze.py` (functions) → `tools/recomp.py` (C) → `zig cc`.

## Directory layout

- `adf/` – ADF images converted from the IPF originals (Greaseweazle + CAPSImg)
- `files/DT1`, `files/DT2` – the disks' files
- `disasm/` – disassembly, one `.s` per code hunk (`tools/disasm.py`)
- `gen/` – generated C (do not edit)
- `runtime/` – CPU state, memory, loader, OS and chip emulation (HLE)
- `launcher/` – settings program and disk image import
- `tools/` – recompiler scripts and downloaded tools

## The original program

- Two disks, standard AmigaDOS OFS. Startup: `s/startup-sequence` → `DuckTales`.
- Data: `*.arc` and `dt1.as`/`dt2.as`. `.arc` is little-endian (`u16 count`,
  `u32 offset[count]`, data); the content formats are not decoded.
- `DuckTales` (193 004 bytes): hunk executable with an **overlay**
  (HUNK_OVERLAY): root node + 13 overlay nodes.
- Compiler: Aztec C, A5 frame pointer, small data model (globals at
  `$xxxx(a4)`, A4 = data hunk + $7FFE).
- The data hunk starts with a table of 97 `jmp` entries, then 26 overlay stubs
  (`bsr loader; dc.b node,0; dc.w offset`). The recompiler turns them into
  direct calls; every overlay node is loaded at its own address
  (`gen/layout.h`).
- The data hunk allocates 46 KB (12 KB in the file + BSS), so the layout uses
  the allocation sizes from the hunk headers.
- About 70 library vectors (exec, dos, graphics, intuition, layers, icon,
  mathffp, input.device) plus direct blitter, Paula, CIA and joystick access.

## Recompiler

- Own 68000 decoder `tools/m68k.py`, checked against Capstone (44 545
  instructions).
- 515 functions, 61 jump tables (switch), about 196 000 lines of C.
- Backward branches call `POLL()`, giving the runtime a chance to run the
  frame clock, interrupts, audio and input.
- `REPLACED` (functions replaced by a return): Aztec C overlay preload
  (`0x7978`, uses LoadSeg); all nodes are resident.
- `GATED` (functions that run only when the runtime allows): the music player.
- `PROLOGUE` (code inserted at a function's start): the copy protection check
  (node 11 `0x942`, returns d0 = 1 for a correct answer) returns 1 at once
  when `copy_protection = 0`.
- The game's memory pool constant (266 000 bytes) is enlarged to 512 KiB
  (`POOL_SIZE_*`), see below.

## Emulation notes (HLE)

- **Display**: the game builds a View/ViewPort itself and double-buffers with
  two copper lists (`MrgCop` twice, kept at `$53e0/$53e4`), switching by
  setting `View.LOFCprList` before `LoadView`. It also patches the COLOR
  moves in the copper lists directly. So `MrgCop` writes a real copper list
  (BPLxPT and COLOR moves) and the display is taken from the copper list in
  effect at `LoadView` (planes latched, colours read live).
  Layout per ViewPort: WAIT, COLOR00–31, then BPLxPT moves. The colour order
  matters: the palette cycling in the vertical blank server (`0x61a0`, e.g.
  the camp fire, and effects left on after finding Bombasium) writes colour n
  at list start + 6 + 4n without searching; with the BPL moves first it
  overwrote the bitplane pointers and the whole screen flickered.
- **Disks**: AmigaDOS device list with both volumes DT1/DT2 always present
  (`RootNode → DosInfo → DevInfo`). `DT1:main.arc` → `<game folder>/DT1/main.arc`.
  File handles are never reused.
- **Memory pool**: the game's own pool (AllocMem + exec Allocate/Deallocate
  on a MemHeader). The game's `free` checks `mh_Lower..mh_Upper`. A user got
  "alloc … FAILED", so the pool was enlarged; no leak was found (allocations
  and frees match), the pool was already ~80 % used in the office.
- **Interrupts**: vertical blank server (`AddIntServer` 5) and Paula
  interrupts (`SetIntVector` 7–10). INTENA/DMACON start as AmigaOS leaves
  them (INTEN, VERTB …); without INTEN the sound engine never finished a
  sound and the flight scene hung. Paula's "block started" interrupt is
  delayed (`chips_service`): the game does DMACON → INTREQ clear → INTENA,
  so an immediate interrupt was lost and every sample played twice.
- **Blitter**: full emulation except line mode (a warning is printed if used).
  `BltMaskBitMapRastPort` uses only the minterm's A terms (game uses `0xE2`).
- **CIA**: addresses decoded like the hardware (the game reads the fire
  button as a word from `$bfe0fe`).
- **mathffp**: `SPCmp` result and flags compare d0 to d1 (verified against
  the flight speed limiter; wrong order made the plane fly backwards).
- **Keyboard timing**: menus do `flush buffer (0x435e) → draw → read
  (0x4376)`, so only keys arriving while drawing count. Graphics calls add
  estimated blitter time (~1 µs per word) to the emulated time inside the
  frame (`emu_time_us`); key events are delivered when due, at `POLL` and
  library calls; a host key press is spread to a random point in the frame.
  All keys auto-repeat (also space): 160 ms delay, 27 ms rate (not a
  multiple of the 20 ms frame, so it does not lock to the game's loop).
- Joystick read from `JOY1DAT` / CIA-A PRA; keys through the input.device
  handler (RAWKEY only, IEQUALIFIER_REPEAT on repeats).

## Diagnostics (environment variables)

- `HLE_TRACE=1` – library and device calls to stderr
- `DT_HEADLESS=1` – no window or sound; one frame per 50 polls
- `DT_MAX_FRAMES=n` – stop at frame n (saves `shot.bmp`)
- `DT_SCRIPT="700:left+,760:left-,800:fire+,806:fire-,900:shot"` – scripted
  input; also `k<rawkey hex>+/-` (Amiga keys), `lmb/rmb`, `mx<n>/my<n>`,
  `dump` (whole memory to `mem_<frame>.bin`)
- `DT_BREAK=<address hex>` – print function history when it is entered
- `DT_DEBUG=1` – on exit, last loop address and function history
- `DT_WATCH=address,length` – report functions writing there
- `DT_LOGFN=address,...` – log calls with the key buffer state, plus `[key]`
  and `[poll]` lines with emulated time
- `DT_MEMLOG=1` – game memory pool allocations
- `DT_SHOT=file.bmp` (settings.exe) – save the first frame and exit

## Next

- Play through and fix differences (platform sections, sounds).
- Blitter line mode.
- Make the generated C readable (flag liveness analysis, naming).
