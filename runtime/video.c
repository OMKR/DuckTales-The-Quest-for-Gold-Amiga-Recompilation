/* Display output: renders the ViewPorts of the active View through SDL. */
#include <SDL.h>
#include <string.h>

#include "m68k.h"
#include "hle.h"
#include "video.h"
#include "settings.h"

#define CANVAS_W 640
#define CANVAS_H 512

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t pixels[CANVAS_W * CANVAS_H];

/* Amiga lines actually shown. The game's screens are 320x200, so the full
 * 256-line PAL area would leave a black band at the bottom; the shown area
 * grows (up to 256) only if a screen uses more. */
static int visible_lines = 200;

static void apply_visible_lines(void)
{
    if (renderer) SDL_RenderSetLogicalSize(renderer, CANVAS_W, visible_lines * 2);
}

/* Snapshot of the display taken at LoadView, as the copper list would hold. */
typedef struct {
    uint32_t vp;            /* ViewPort */
    uint32_t colors;        /* first COLOR00 move of its copper list segment, or 0 */
    uint32_t planes[8];
    int depth, bpr, rows;
    int width, height, x, y, rx, ry;
    uint16_t modes;
} Display;

static Display displays[4];
static int display_count;
static int headless;

void video_init(void)
{
    headless = getenv("DT_HEADLESS") != NULL;
    if (headless) return;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    window = SDL_CreateWindow("DuckTales: The Quest for Gold", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, 320 * settings.scale, 200 * settings.scale,
                              SDL_WINDOW_RESIZABLE |
                              (settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    apply_visible_lines();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                CANVAS_W, CANVAS_H);
}

/* Fill bitplane pointers and the colour location of display `index` from
 * its segment of the copper list (segments start with a WAIT). The planes
 * are latched now; colours are read live because the game patches them. */
static void apply_copper(uint32_t cpr)
{
    if (!cpr) return;
    uint32_t p = rl(cpr + 4);
    int seg = -1;
    for (int n = 0; p && n < 4096; n++, p += 4) {
        uint16_t w1 = (uint16_t)rw(p), w2 = (uint16_t)rw(p + 2);
        if (w1 == 0xFFFF && w2 == 0xFFFE) break;
        if (w1 & 1) { seg++; continue; }                      /* WAIT / SKIP */
        if (seg < 0 || seg >= display_count) continue;
        Display *d = &displays[seg];
        if (w1 >= 0x0E0 && w1 < 0x100) {                     /* BPLxPTH/L */
            int i = (w1 - 0x0E0) / 4;
            if (w1 & 2) d->planes[i] = (d->planes[i] & 0xFFFF0000u) | w2;
            else d->planes[i] = (d->planes[i] & 0xFFFFu) | ((uint32_t)w2 << 16);
            d->rx = d->ry = 0;          /* the scroll offset is in the pointers */
        } else if (w1 == 0x180 && !d->colors) {
            d->colors = p;
        }
    }
}

void video_toggle_fullscreen(void)
{
    if (!window) return;
    int full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void video_set_title(const char *title)
{
    if (window) SDL_SetWindowTitle(window, title);
}

/* Vsync caps presentation at the monitor rate, so it is only used up to
 * 60 fps; above that frames are paced by the emulation clock alone. */
void video_set_vsync(int on)
{
    if (renderer) SDL_RenderSetVSync(renderer, on);
}

void video_load_view(uint32_t view)
{
    display_count = 0;
    if (!view) return;
    for (uint32_t vp = rl(view); vp && display_count < 4; vp = rl(vp)) {
        if (rw(vp + 32) & 0x2000) continue;            /* VP_HIDE */
        uint32_t ri = rl(vp + 36);
        uint32_t bm = ri ? rl(ri + 4) : 0;
        if (!bm) continue;
        Display *d = &displays[display_count++];
        d->vp = vp;
        d->bpr = rw(bm);
        d->rows = rw(bm + 2);
        d->depth = rb(bm + 5);
        if (d->depth > 8) d->depth = 8;
        for (int i = 0; i < d->depth; i++) d->planes[i] = rl(bm + 8 + 4 * i);
        d->width = (int16_t)rw(vp + 24);
        d->height = (int16_t)rw(vp + 26);
        d->x = (int16_t)rw(vp + 28);
        d->y = (int16_t)rw(vp + 30);
        d->modes = rw(vp + 32);
        if (hle_trace) {
            static int last_w, last_h, last_y;
            if (d->width != last_w || d->height != last_h || d->y != last_y)
                fprintf(stderr, "[viewport] %dx%d at y=%d modes %04x\n", d->width, d->height, d->y, d->modes);
            last_w = d->width; last_h = d->height; last_y = d->y;
        }
        {
            int lace = (d->modes & 0x0004) != 0;
            int bottom = (d->y < 0 ? 0 : d->y) + (lace ? d->height / 2 : d->height);
            if (bottom > 256) bottom = 256;
            if (bottom > visible_lines) { visible_lines = bottom; apply_visible_lines(); }
        }
        d->rx = (int16_t)rw(ri + 8);
        d->ry = (int16_t)rw(ri + 10);
        d->colors = 0;
    }
    apply_copper(rl(view + 4));        /* View.LOFCprList */
}

static uint32_t rgb4_to_argb(uint16_t c)
{
    uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return 0xFF000000u | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

static void render(void)
{
    memset(pixels, 0, sizeof pixels);
    for (int i = 0; i < display_count; i++) {
        Display *d = &displays[i];
        uint32_t palette[64] = {0};
        uint32_t cm = rl(d->vp + 4);
        uint32_t copper = d->colors;
        if (copper) {
            /* colours as the copper list sets them (the game patches it) */
            for (int c = 0; c < 32; c++) palette[c] = rgb4_to_argb((uint16_t)rw(copper + 4 * c + 2));
        } else if (cm) {
            uint32_t table = rl(cm + 4);
            int count = rw(cm + 2);
            for (int c = 0; c < count && c < 32; c++) palette[c] = rgb4_to_argb((uint16_t)rw(table + 2 * c));
        }
        for (int c = 0; c < 32; c++) {   /* extra half-brite */
            uint32_t p = palette[c];
            palette[c + 32] = 0xFF000000u | ((p >> 1) & 0x7F7F7F);
        }
        int hires = (d->modes & 0x8000) != 0;
        int lace = (d->modes & 0x0004) != 0;
        int xscale = hires ? 1 : 2, yscale = lace ? 1 : 2;
        int top = (d->y < 0 ? 0 : d->y) * yscale;
        for (int y = 0; y < d->height; y++) {
            int sy = y + d->ry;
            if (sy < 0 || sy >= d->rows) continue;
            int oy = top + y * yscale;
            if (oy >= CANVAS_H) break;
            for (int x = 0; x < d->width; x++) {
                int sx = x + d->rx;
                if (sx < 0 || sx >= d->bpr * 8) continue;
                uint32_t byte = (uint32_t)sy * d->bpr + (sx >> 3);
                int bit = 7 - (sx & 7), color = 0;
                for (int p = 0; p < d->depth; p++)
                    color |= ((mem[(d->planes[p] + byte) & ADDR_MASK] >> bit) & 1) << p;
                uint32_t argb = palette[color & 63];
                int ox = x * xscale;
                if (ox >= CANVAS_W) break;
                for (int dy = 0; dy < yscale && oy + dy < CANVAS_H; dy++)
                    for (int dx = 0; dx < xscale; dx++)
                        pixels[(oy + dy) * CANVAS_W + ox + dx] = argb;
            }
        }
    }
}

void video_frame(void)
{
    if (headless) return;
    render();
    SDL_UpdateTexture(texture, NULL, pixels, CANVAS_W * 4);
    SDL_RenderClear(renderer);
    SDL_Rect src = {0, 0, CANVAS_W, visible_lines * 2};
    SDL_RenderCopy(renderer, texture, &src, NULL);
    SDL_RenderPresent(renderer);
}

int video_save_bmp(const char *path)
{
    render();
    if (hle_trace) fprintf(stderr, "displays: %d\n", display_count);
    for (int i = 0; hle_trace && i < display_count; i++) {
        Display *d = &displays[i];
        uint32_t nonzero = 0;
        for (int p = 0; p < d->depth; p++)
            for (int b = 0; b < d->bpr * d->rows; b++) nonzero += mem[(d->planes[p] + b) & ADDR_MASK] != 0;
        uint32_t cm = rl(d->vp + 4);
        fprintf(stderr, "  vp %06x %dx%d at %d,%d modes %04x depth %d bpr %d rows %d planes %06x.. nonzero %u cm %06x:",
                d->vp, d->width, d->height, d->x, d->y, d->modes, d->depth, d->bpr, d->rows,
                d->planes[0], nonzero, cm);
        for (int c = 0; cm && c < 8; c++) fprintf(stderr, " %03x", rw(rl(cm + 4) + 2 * c));
        fprintf(stderr, "    rx %d ry %d (RasInfo %d,%d)\n", d->rx, d->ry,
                (int16_t)rw(rl(d->vp + 36) + 8), (int16_t)rw(rl(d->vp + 36) + 10));
        uint32_t cop = d->colors;
        fprintf(stderr, "\n    copper %06x:", cop);
        for (int c = 0; cop && c < 8; c++) fprintf(stderr, " %03x", rw(cop + 4 * c + 2));
        fprintf(stderr, "\n");
    }
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t size = 54 + CANVAS_W * CANVAS_H * 4;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    int32_t w = CANVAS_W, h = -CANVAS_H;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    fwrite(pixels, 4, CANVAS_W * CANVAS_H, f);
    fclose(f);
    return 1;
}
