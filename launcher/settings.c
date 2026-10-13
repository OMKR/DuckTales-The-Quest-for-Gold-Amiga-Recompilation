/* DuckTales: The Quest for Gold - settings program.
 *
 * A small stand-alone window that edits ducktales.ini (see
 * runtime/settings.h) and can start the game. Keyboard, mouse and game
 * controller all work: up/down choose, left/right change, Enter selects.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "settings.h"
#include "diskimage.h"
#include "font8x8_basic.h"
#include "font8x8_ext_latin.h"

#define W 640
#define H 440   /* menu rows end at ~382; help line below them */
#define CHAR 16           /* 8x8 font drawn at 2x */
#define LINE 21
#define TOP 60
#define LEFT 40

static SDL_Renderer *ren;
static Settings cfg;
static char status[128];
static int editing_dir;        /* 1 = game folder, 2 = disk image path */
static char dir_edit[512];
static int status_ok;          /* status line shown in green */

enum {
    I_FPS, I_MUSIC, I_SOUND, I_VOLUME, I_FULL, I_SCALE, I_DEAD, I_PAD, I_PROT, I_DIR,
    I_IMPORT, I_BUILD, I_DEFAULTS, I_START, I_SAVE, I_QUIT, I_COUNT
};

static const char *const labels[I_COUNT] = {
    "Game speed (fps)", "Music rate (Hz)", "Sound", "Volume",
    "Fullscreen", "Window size", "Controller dead zone", "Keyboard mode", "Copy protection", "Game folder",
    "Import disk image (.adf/.ipf)", "Build game from disk files",
    "Restore defaults", "Save and start game", "Save and exit",
    "Exit without saving",
};

/* ---- text ----------------------------------------------------------------------- */

static const char *glyph(unsigned cp)
{
    if (cp < 128) return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF) return font8x8_ext_latin[cp - 0xA0];
    return font8x8_basic['?'];
}

static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp = *p++;
    if (cp >= 0xC0 && cp < 0xE0 && (*p & 0xC0) == 0x80) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}

static int text_width(const char *s)
{
    int n = 0;
    while (*s) { next_cp(&s); n++; }
    return n * CHAR;
}

/* px = size of one font pixel (2 = normal text, 1 = small help line) */
static void draw_text_px(int x, int y, const char *s, SDL_Color c, int px)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    while (*s) {
        const char *g = glyph(next_cp(&s));
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((g[row] >> col) & 1) {
                    SDL_Rect r = {x + col * px, y + row * px, px, px};
                    SDL_RenderFillRect(ren, &r);
                }
        x += 8 * px;
    }
}

static void draw_text(int x, int y, const char *s, SDL_Color c)
{
    draw_text_px(x, y, s, c, 2);
}

/* ---- settings ---------------------------------------------------------------------- */

static char base_dir[1024];

/* Resolve the game folder like the game does (relative to the program). */
static int gamedir_ok(const char *dir)
{
    char path[1600];
    struct stat st;
    snprintf(path, sizeof path, "%s/DT1/DuckTales", dir);
    if (stat(path, &st) == 0) return 1;
    snprintf(path, sizeof path, "%s%s/DT1/DuckTales", base_dir, dir);
    return stat(path, &st) == 0;
}

static int game_built(void);

static void value_text(int item, char *out, size_t n)
{
    out[0] = 0;
    switch (item) {
    case I_FPS: snprintf(out, n, "< %d >", cfg.fps); break;
    case I_MUSIC: snprintf(out, n, "< %d >", cfg.music_hz); break;
    case I_SOUND: snprintf(out, n, "< %s >", cfg.sound ? "on" : "off"); break;
    case I_VOLUME: snprintf(out, n, "< %d %% >", cfg.volume); break;
    case I_FULL: snprintf(out, n, "< %s >", cfg.fullscreen ? "yes" : "no"); break;
    case I_SCALE: snprintf(out, n, "< %dx %dx%d >", cfg.scale, 320 * cfg.scale, 200 * cfg.scale); break;
    case I_DEAD: snprintf(out, n, "< %d %% >", cfg.deadzone); break;
    case I_PROT: snprintf(out, n, "< %s >", cfg.copy_protection ? "on" : "off"); break;
    case I_PAD: snprintf(out, n, "< %s >", cfg.keyboard_mode ? "joystick" : "keyboard"); break;
    default: break;
    }
}

static const char *help_text(int item)
{
    switch (item) {
    case I_FPS: return "25 = recommended, 50 = original Amiga speed";
    case I_MUSIC: return "50 = original tempo (independent of game speed)";
    case I_VOLUME: return "Shift + arrow: step 1";
    case I_SCALE: return "window size in pixels (the game screen is 320x200)";
    case I_DEAD: return "how far the stick must tilt before it moves";
    case I_PROT: return "off = skip the code wheel check (security system)";
    case I_PAD: return "joystick = arrows move the joystick, space/Ctrl = fire";
    case I_DIR: return editing_dir ? "type the path, Enter = done, Esc = cancel"
                                   : "Enter = edit (folder containing DT1 and DT2)";
    case I_BUILD: return game_built() ? "built - rebuild after changing the game folder"
                                      : "needed once: converts your game into ducktales.exe";
    case I_IMPORT: return editing_dir ? "type the image path, Enter = extract, Esc = cancel"
                                      : "Enter = type a path, or drop .adf/.ipf files on this window";
    default: return "";
    }
}

static void change(int item, int dir, int big)
{
    int step = big ? 10 : 1;
    switch (item) {
    case I_FPS: cfg.fps += dir * step; break;
    case I_MUSIC: cfg.music_hz += dir * step; break;
    case I_SOUND: cfg.sound = !cfg.sound; break;
    case I_VOLUME: cfg.volume += dir * (big ? 1 : 5); break;
    case I_FULL: cfg.fullscreen = !cfg.fullscreen; break;
    case I_PROT: cfg.copy_protection = !cfg.copy_protection; break;
    case I_PAD: cfg.keyboard_mode = !cfg.keyboard_mode; break;
    case I_SCALE: cfg.scale += dir; break;
    case I_DEAD: cfg.deadzone += dir * (big ? 1 : 5); break;
    default: return;
    }
    settings_clamp(&cfg);
    status[0] = 0;
}

/* Extract a disk image into the game folder (resolved like the game does). */
static void import_image(const char *image)
{
    static unsigned char adf[ADF_SIZE];
    char err[160], vol[64], out[1200];
    const char *dir = cfg.gamedir;
    int absolute = dir[0] == '/' || dir[0] == '\\' || (dir[0] && dir[1] == ':');
    snprintf(out, sizeof out, "%s%s", absolute ? "" : base_dir, dir);
    status_ok = 0;
    if (!disk_read_image(image, adf, err, sizeof err)) {
        snprintf(status, sizeof status, "Import failed: %s", err);
        return;
    }
    int n = disk_extract(adf, out, vol, sizeof vol, err, sizeof err);
    if (n < 0) {
        snprintf(status, sizeof status, "Import failed: %s", err);
        return;
    }
    snprintf(status, sizeof status, "Disk %s: %d files extracted", vol, n);
    status_ok = 1;
}

/* ducktales(.exe) next to this program (built by the installer). */
static int game_built(void)
{
    char path[1200];
    struct stat st;
#ifdef _WIN32
    snprintf(path, sizeof path, "%sducktales.exe", base_dir);
#else
    snprintf(path, sizeof path, "%sducktales", base_dir);
#endif
    return stat(path, &st) == 0;
}

/* Run the installer (install.bat in its own console window, install.sh in
 * the background): it downloads the build
 * tools on first use, recompiles the user's copy of the game and compiles
 * ducktales.exe. */
static void build_game(void)
{
    char bat[1200], dir[1200], cmd[2600];
    struct stat st;
    status_ok = 0;
#ifdef _WIN32
    snprintf(bat, sizeof bat, "%sinstall.bat", base_dir);
#else
    snprintf(bat, sizeof bat, "%sinstall.sh", base_dir);
#endif
    if (stat(bat, &st) != 0) {
        snprintf(status, sizeof status, "installer not found (development tree: use build.sh)");
        return;
    }
    if (!gamedir_ok(cfg.gamedir)) {
        snprintf(status, sizeof status, "Import the disk images first");
        return;
    }
    const char *d = cfg.gamedir;
    int absolute = d[0] == '/' || d[0] == '\\' || (d[0] && d[1] == ':');
    snprintf(dir, sizeof dir, "%s%s", absolute ? "" : base_dir, d);
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "cmd.exe /c \"\"%s\" \"%s\"\"", bat, dir);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                        base_dir[0] ? base_dir : NULL, &si, &pi)) {
        snprintf(status, sizeof status, "Could not start install.bat");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    snprintf(status, sizeof status, "Building in the console window...");
    status_ok = 1;
#else
    /* runs in the background; its output goes to the terminal that started
     * this program */
    (void)cmd;
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", bat, dir, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        snprintf(status, sizeof status, "Could not start install.sh");
        return;
    }
    snprintf(status, sizeof status, "Building - see the terminal; ducktales appears when done");
    status_ok = 1;
#endif
}

static int start_game(void)
{
#ifdef _WIN32
    char exe[1200], cmd[1300];
    snprintf(exe, sizeof exe, "%sducktales.exe", base_dir);
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, base_dir[0] ? base_dir : NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#else
    char exe[1200];
    snprintf(exe, sizeof exe, "%sducktales", base_dir);
    pid_t pid = fork();
    if (pid == 0) {
        if (base_dir[0] && chdir(base_dir) != 0) _exit(1);
        execl(exe, exe, (char *)NULL);
        _exit(1);
    }
    return pid > 0;
#endif
}

/* Returns 1 to keep running, 0 to quit. */
static int activate(int item)
{
    switch (item) {
    case I_SOUND: case I_FULL: case I_PROT: case I_PAD: change(item, 1, 0); return 1;
    case I_DIR:
        editing_dir = 1;
        snprintf(dir_edit, sizeof dir_edit, "%s", cfg.gamedir);
        SDL_StartTextInput();
        return 1;
    case I_IMPORT:
        editing_dir = 2;
        dir_edit[0] = 0;
        SDL_StartTextInput();
        return 1;
    case I_DEFAULTS:
        settings_defaults(&cfg);
        snprintf(status, sizeof status, "Defaults restored (not saved yet)");
        return 1;
    case I_BUILD:
        if (!settings_save(&cfg, settings_path())) {
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        build_game();
        return 1;
    case I_START:
        if (!game_built()) {
            status_ok = 0;
            snprintf(status, sizeof status, "Build the game first (Build game from disk files)");
            return 1;
        }
        if (!gamedir_ok(cfg.gamedir)) {
            snprintf(status, sizeof status, "Game folder not found!");
            return 1;
        }
        if (!settings_save(&cfg, settings_path())) {
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        if (!start_game()) {
            snprintf(status, sizeof status, "Could not start the game!");
            return 1;
        }
        return 0;
    case I_SAVE:
        if (!settings_save(&cfg, settings_path())) {
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        return 0;
    case I_QUIT:
        return 0;
    }
    return 1;
}

static void draw(int sel)
{
    const SDL_Color white = {255, 255, 255, 255}, grey = {150, 150, 170, 255};
    const SDL_Color gold = {255, 204, 51, 255}, red = {255, 110, 90, 255}, green = {120, 220, 120, 255};
    SDL_SetRenderDrawColor(ren, 16, 24, 64, 255);
    SDL_RenderClear(ren);
    const char *title = "DuckTales - Settings";
    draw_text((W - text_width(title)) / 2, 20, title, gold);

    for (int i = 0; i < I_COUNT; i++) {
        int y = TOP + i * LINE + (i >= I_DEFAULTS ? 12 : 0);
        if (i == sel) {
            SDL_SetRenderDrawColor(ren, 60, 80, 160, 255);
            SDL_Rect r = {LEFT - 10, y - 3, W - 2 * LEFT + 20, LINE - 1};   /* text is 16 px tall */
            SDL_RenderFillRect(ren, &r);
        }
        draw_text(LEFT, y, labels[i], i == sel ? white : grey);
        char v[64];
        value_text(i, v, sizeof v);
        if (v[0]) draw_text(W - LEFT - text_width(v), y, v, i == sel ? gold : white);
        if (i == I_IMPORT && editing_dir == 2) {
            size_t len = strlen(dir_edit);
            char shown[24];
            snprintf(shown, sizeof shown, "%s%s_", len > 14 ? "..." : "", len > 14 ? dir_edit + len - 14 : dir_edit);
            draw_text(W - LEFT - text_width(shown), y, shown, gold);
        }
        if (i == I_DIR) {
            const char *d = editing_dir == 1 ? dir_edit : cfg.gamedir;
            char shown[24];
            size_t len = strlen(d);
            snprintf(shown, sizeof shown, "%s%s", len > 14 ? "..." : "", len > 14 ? d + len - 14 : d);
            if (editing_dir == 1) strcat(shown, "_");
            draw_text(W - LEFT - text_width(shown), y, shown,
                      editing_dir == 1 ? gold : gamedir_ok(d) ? green : red);
        }
    }
    const char *help = status[0] ? status : help_text(sel);
    /* help text at 1x size to fit */
    /* below the last menu row, small font (no render scale change: it
     * would break the logical-size scaling and misplace the text) */
    draw_text_px(LEFT - 10, H - 30, help, status[0] ? (status_ok ? green : red) : grey, 1);
    SDL_RenderPresent(ren);
}

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    /* --extract <image> <folder>: command line import (also used for tests) */
    if (argc == 4 && !strcmp(argv[1], "--extract")) {
        static unsigned char adf[ADF_SIZE];
        char err[160], vol[64];
        if (!disk_read_image(argv[2], adf, err, sizeof err)) { printf("error: %s\n", err); return 1; }
        int n = disk_extract(adf, argv[3], vol, sizeof vol, err, sizeof err);
        if (n < 0) { printf("error: %s\n", err); return 1; }
        printf("%s: %d files\n", vol, n);
        return 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    char *b = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", b ? b : "");
    SDL_free(b);
    settings_defaults(&cfg);
    if (!settings_load(&cfg, settings_path()))
        snprintf(status, sizeof status, "No saved settings - using defaults");

    SDL_Window *win = SDL_CreateWindow("DuckTales - Settings", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W * 3 / 2, H * 3 / 2,
                                       SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_GameController *pad = NULL;

    int sel = I_START, running = 1;
    if (getenv("DT_SEL")) sel = atoi(getenv("DT_SEL"));   /* testing */
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_DROPFILE) {
                import_image(e.drop.file);
                SDL_free(e.drop.file);
                sel = I_IMPORT;
            }
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (editing_dir && e.type == SDL_TEXTINPUT) {
                size_t len = strlen(dir_edit);
                snprintf(dir_edit + len, sizeof dir_edit - len, "%s", e.text.text);
            } else if (editing_dir && e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_BACKSPACE && dir_edit[0]) {
                    size_t len = strlen(dir_edit);
                    do len--; while (len > 0 && (dir_edit[len] & 0xC0) == 0x80);
                    dir_edit[len] = 0;
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) {
                    if (k != SDLK_ESCAPE && dir_edit[0]) {
                        if (editing_dir == 1) snprintf(cfg.gamedir, sizeof cfg.gamedir, "%s", dir_edit);
                        else import_image(dir_edit);
                    }
                    editing_dir = 0;
                    SDL_StopTextInput();
                }
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN) {
                int big = (SDL_GetModState() & KMOD_SHIFT) != 0;
                int key = e.type == SDL_KEYDOWN ? e.key.keysym.sym : -1;
                int btn = e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.button : -1;
                if (key == SDLK_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_UP)
                    sel = (sel + I_COUNT - 1) % I_COUNT;
                else if (key == SDLK_DOWN || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                    sel = (sel + 1) % I_COUNT;
                else if (key == SDLK_LEFT || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) change(sel, -1, big);
                else if (key == SDLK_RIGHT || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) change(sel, 1, big);
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                         btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START)
                    running = activate(sel);
                else if (key == SDLK_ESCAPE || btn == SDL_CONTROLLER_BUTTON_B) running = 0;
            } else if (e.type == SDL_MOUSEMOTION && !editing_dir) {
                for (int i = 0; i < I_COUNT; i++) {
                    int y = TOP + i * LINE + (i >= I_DEFAULTS ? 12 : 0);
                    if (e.motion.y >= y - 3 && e.motion.y < y - 3 + LINE) sel = i;
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && !editing_dir) {
                /* left half of a value: decrease, right half: increase */
                char v[64];
                value_text(sel, v, sizeof v);
                if (v[0] && e.button.x > W / 2)
                    change(sel, e.button.x < W - LEFT - text_width(v) / 2 ? -1 : 1, 0);
                else
                    running = activate(sel);
            } else if (e.type == SDL_MOUSEWHEEL && !editing_dir) {
                change(sel, e.wheel.y > 0 ? 1 : -1, 0);
            }
        }
        draw(sel);
        /* DT_SHOT=file.bmp: save the first frame and exit (for testing) */
        if (getenv("DT_SHOT")) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W * 3 / 2, H * 3 / 2, 32,
                                                            SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, getenv("DT_SHOT"));
            SDL_FreeSurface(s);
            running = 0;
        }
    }
    SDL_Quit();
    return 0;
}
