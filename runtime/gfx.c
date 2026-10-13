/* graphics.library, layers.library, intuition.library, icon.library,
 * mathffp.library and devices (input.device keyboard handler). */
#include <SDL.h>
#include <math.h>
#include <string.h>

#include "m68k.h"
#include "hle.h"
#include "video.h"
#include "font8x8_basic.h"

uint32_t hle_callback(uint32_t code, uint32_t a0, uint32_t a1);
static void gfx_cost(uint32_t words);

/* ---- planar drawing ---------------------------------------------------------- */

/* Where a RastPort draws: bitmap plus layer offset and clip rectangle. */
typedef struct {
    uint32_t bm;
    int bpr, rows, depth;
    int xo, yo;
    int x0, y0, x1, y1;   /* clip, in bitmap coordinates, inclusive */
} Target;

static Target target_bitmap(uint32_t bm)
{
    Target t = {0};
    t.bm = bm;
    t.bpr = rw(bm);
    t.rows = rw(bm + 2);
    t.depth = rb(bm + 5);
    t.x1 = t.bpr * 8 - 1;
    t.y1 = t.rows - 1;
    return t;
}

static Target target_rp(uint32_t rp)
{
    Target t = target_bitmap(rl(rp + 4));
    uint32_t layer = rl(rp);
    if (layer) {
        t.xo = (int16_t)rw(layer + 16);
        t.yo = (int16_t)rw(layer + 18);
        if (t.xo > t.x0) t.x0 = t.xo;
        if (t.yo > t.y0) t.y0 = t.yo;
        if ((int16_t)rw(layer + 20) < t.x1) t.x1 = (int16_t)rw(layer + 20);
        if ((int16_t)rw(layer + 22) < t.y1) t.y1 = (int16_t)rw(layer + 22);
    }
    return t;
}

static int get_pixel(const Target *t, int x, int y)
{
    if (x < 0 || y < 0 || x >= t->bpr * 8 || y >= t->rows) return 0;
    uint32_t byte = (uint32_t)y * t->bpr + (x >> 3);
    int bit = 7 - (x & 7), c = 0;
    for (int p = 0; p < t->depth; p++)
        c |= ((mem[(rl(t->bm + 8 + 4 * p) + byte) & ADDR_MASK] >> bit) & 1) << p;
    return c;
}

static void put_pixel(const Target *t, int x, int y, int color)
{
    if (x < t->x0 || y < t->y0 || x > t->x1 || y > t->y1) return;
    uint32_t byte = (uint32_t)y * t->bpr + (x >> 3);
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    for (int p = 0; p < t->depth; p++) {
        uint8_t *b = &mem[(rl(t->bm + 8 + 4 * p) + byte) & ADDR_MASK];
        if (color & (1 << p)) *b |= bit;
        else *b &= (uint8_t)~bit;
    }
}

/* Pen drawing in the RastPort's draw mode (JAM1, JAM2, COMPLEMENT). */
static void draw_pixel(uint32_t rp, const Target *t, int x, int y, int fg)
{
    int mode = rb(rp + 28);
    x += t->xo;
    y += t->yo;
    if (mode & 2) {         /* COMPLEMENT */
        if (x < t->x0 || y < t->y0 || x > t->x1 || y > t->y1) return;
        put_pixel(t, x, y, get_pixel(t, x, y) ^ (rb(rp + 24) & 0xFF));
    } else {
        put_pixel(t, x, y, fg ? rb(rp + 25) : rb(rp + 26));
    }
}

static void rect_fill(uint32_t rp, int x0, int y0, int x1, int y1)
{
    Target t = target_rp(rp);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) draw_pixel(rp, &t, x, y, 1);
}

static void draw_line(uint32_t rp, int x0, int y0, int x1, int y1)
{
    Target t = target_rp(rp);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        draw_pixel(rp, &t, x0, y0, 1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_text(uint32_t rp, uint32_t str, int count)
{
    Target t = target_rp(rp);
    int x = (int16_t)rw(rp + 36), y = (int16_t)rw(rp + 38) - 6;
    int mode = rb(rp + 28);
    for (int i = 0; i < count; i++) {
        unsigned ch = mem[(str + i) & ADDR_MASK] & 0x7F;
        for (int row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++) {
                int on = (bits >> col) & 1;
                if (on || (mode & 1)) draw_pixel(rp, &t, x + col, y + row, on);
            }
        }
        x += 8;
    }
    ww(rp + 36, (uint32_t)x);
}

/* Blit a rectangle between bitmaps with a graphics.library minterm
 * (B = source, C = destination). mask_plane, if set, limits the blit to its
 * set bits (BltMaskBitMapRastPort). */
static void blit_rect(uint32_t src_bm, int sx, int sy, const Target *dst, int dx, int dy,
                      int w, int h, int minterm, int planemask, uint32_t mask_plane)
{
    Target src = target_bitmap(src_bm);
    if (w <= 0 || h <= 0) return;
    gfx_cost((uint32_t)((w + 15) / 16 + 1) * (uint32_t)h * (uint32_t)dst->depth);
    /* copy through a temporary so overlapping blits behave */
    uint8_t *tmp = malloc((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) tmp[y * w + x] = (uint8_t)get_pixel(&src, sx + x, sy + y);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int px = dx + x, py = dy + y;
            if (px < dst->x0 || py < dst->y0 || px > dst->x1 || py > dst->y1) continue;
            if (mask_plane) {
                uint32_t byte = (uint32_t)(sy + y) * src.bpr + ((sx + x) >> 3);
                if (!((mem[(mask_plane + byte) & ADDR_MASK] >> (7 - ((sx + x) & 7))) & 1)) continue;
            }
            int s = tmp[y * w + x], d = get_pixel(dst, px, py), out = 0;
            for (int p = 0; p < dst->depth; p++) {
                int b = (s >> p) & 1, c = (d >> p) & 1, r;
                if (!(planemask & (1 << p))) r = c;
                /* BltMaskBitMapRastPort only uses the A terms: 0xEx means
                 * copy the source through the mask (the game passes 0xE2),
                 * 0x2x inverts it. */
                else if (mask_plane && (minterm & 0xF0) == 0xE0) r = b;
                else if (mask_plane && (minterm & 0xF0) == 0x20) r = !b;
                else r = (minterm >> (4 | (b << 1) | c)) & 1;
                out |= r << p;
            }
            put_pixel(dst, px, py, out);
        }
    }
    free(tmp);
}

/* ---- copper lists ---------------------------------------------------------------- */

/* MrgCop builds a real copper list in View.LOFCprList. The game relies on
 * that: it keeps two lists (one per bitmap) and flips the display by
 * pointing LOFCprList at one of them before LoadView, and it patches the
 * COLORxx moves in them directly. So the display is rendered from the
 * copper list in effect at LoadView (video.c), like the hardware does.
 *
 * Per ViewPort: WAIT for its first line, BPLxPT moves for each plane,
 * then 32 consecutive colour MOVEs. An existing list is reused in place. */
#define MAX_COPPER_LISTS 32
static struct { uint32_t vp, colors; } copper_colors[MAX_COPPER_LISTS];
static int copper_color_count;

static void remember_colors(uint32_t vp, uint32_t colors)
{
    for (int i = 0; i < copper_color_count; i++)
        if (copper_colors[i].colors == colors) { copper_colors[i].vp = vp; return; }
    if (copper_color_count < MAX_COPPER_LISTS) {
        copper_colors[copper_color_count].vp = vp;
        copper_colors[copper_color_count++].colors = colors;
    }
}

static void mrg_cop(uint32_t view)
{
    int nvp = 0;
    for (uint32_t vp = rl(view); vp && nvp < 8; vp = rl(vp)) nvp++;
    uint32_t words = (uint32_t)nvp * (2 + 4 * 8 + 64) + 2;
    uint32_t cpr = rl(view + 4);
    if (!cpr || rw(cpr + 8) < words) {
        cpr = hle_alloc(12, 1);
        wl(cpr + 4, hle_alloc(words * 2, 1));
        ww(cpr + 8, words);
        wl(view + 4, cpr);
    }
    uint32_t p = rl(cpr + 4);
    for (uint32_t vp = rl(view); vp && nvp-- > 0; vp = rl(vp)) {
        if (rw(vp + 32) & 0x2000) continue;            /* VP_HIDE */
        int line = (int16_t)rw(vp + 30) + 0x2C;
        ww(p, (uint32_t)((line & 0xFF) << 8) | 1); ww(p + 2, 0xFFFE); p += 4;  /* WAIT */
        /* Colours right after the WAIT, as in the ROM's lists: the game's
         * palette cycling (0x61a0, fire flicker etc.) writes colour n at
         * list start + 6 + 4n without searching. */
        uint32_t cm = rl(vp + 4);
        int count = cm ? rw(cm + 2) : 0;
        remember_colors(vp, p);
        for (int c = 0; c < 32; c++) {
            ww(p, 0x180 + 2 * c);
            ww(p + 2, c < count ? rw(rl(cm + 4) + 2 * c) : 0);
            p += 4;
        }
        uint32_t ri = rl(vp + 36), bm = ri ? rl(ri + 4) : 0;
        if (bm) {
            int depth = rb(bm + 5) > 8 ? 8 : rb(bm + 5);
            uint32_t offset = (uint32_t)((int16_t)rw(ri + 10) * rw(bm) + ((int16_t)rw(ri + 8) >> 3));
            for (int i = 0; i < depth; i++) {
                uint32_t pt = rl(bm + 8 + 4 * i) + offset;
                ww(p, 0x0E0 + 4 * i); ww(p + 2, pt >> 16); p += 4;
                ww(p, 0x0E2 + 4 * i); ww(p + 2, pt & 0xFFFF); p += 4;
            }
        }
    }
    ww(p, 0xFFFF);
    ww(p + 2, 0xFFFE);
}

/* LoadRGB4/SetRGB4 change the ColorMap and every copper list showing it. */
static void set_vp_color(uint32_t vp, int n, uint16_t rgb)
{
    uint32_t cm = rl(vp + 4);
    if (cm && n < (int)rw(cm + 2)) ww(rl(cm + 4) + 2 * n, rgb);
    if (n >= 32) return;
    for (int i = 0; i < copper_color_count; i++)
        if (copper_colors[i].vp == vp) ww(copper_colors[i].colors + 4 * n + 2, rgb);
}

/* ---- graphics.library ----------------------------------------------------------- */

static uint32_t topaz;   /* fake TextFont */

static uint32_t open_font(void)
{
    if (!topaz) {
        topaz = hle_alloc(52, 1);
        ww(topaz + 20, 8);     /* tf_YSize */
        ww(topaz + 24, 8);     /* tf_XSize */
        ww(topaz + 26, 6);     /* tf_Baseline */
        wb(topaz + 32, 32);
        wb(topaz + 33, 127);
    }
    return topaz;
}

int hle_graphics(int off)
{
    uint32_t *d = R.d, *a = R.a;
    switch (off) {
    case -30: { /* BltBitMap(a0 src, d0 sx, d1 sy, a1 dst, d2 dx, d3 dy, d4 w, d5 h, d6 minterm, d7 mask) */
        Target t = target_bitmap(a[1]);
        blit_rect(a[0], (int16_t)d[0], (int16_t)d[1], &t, (int16_t)d[2], (int16_t)d[3],
                  (int16_t)d[4], (int16_t)d[5], d[6] & 0xFF, d[7] & 0xFF, 0);
        d[0] = rb(a[0] + 5);
        return 1;
    }
    case -606: { /* BltBitMapRastPort(a0 src, d0, d1, a1 rp, d2, d3, d4, d5, d6 minterm) */
        Target t = target_rp(a[1]);
        blit_rect(a[0], (int16_t)d[0], (int16_t)d[1], &t, (int16_t)d[2] + t.xo, (int16_t)d[3] + t.yo,
                  (int16_t)d[4], (int16_t)d[5], d[6] & 0xFF, 0xFF, 0);
        return 1;
    }
    case -636: { /* BltMaskBitMapRastPort(..., d6 minterm, a2 mask) */
        Target t = target_rp(a[1]);
        if (hle_trace)
            fprintf(stderr, "  mask blit src bm %06x depth %d bpr %d rows %d (%d,%d) -> (%d,%d) %dx%d "
                    "minterm %02x mask %06x dst depth %d\n", a[0], rb(a[0] + 5), rw(a[0]), rw(a[0] + 2),
                    (int16_t)d[0], (int16_t)d[1], (int16_t)d[2], (int16_t)d[3], (int16_t)d[4],
                    (int16_t)d[5], d[6] & 0xFF, a[2], t.depth);
        blit_rect(a[0], (int16_t)d[0], (int16_t)d[1], &t, (int16_t)d[2] + t.xo, (int16_t)d[3] + t.yo,
                  (int16_t)d[4], (int16_t)d[5], d[6] & 0xFF, 0xFF, a[2]);
        return 1;
    }
    case -60:   /* Text(a1 rp, a0 str, d0 count) */
        draw_text(a[1], a[0], (int)(d[0] & 0xFFFF));
        return 1;
    case -54:   /* TextLength */
        d[0] = (d[0] & 0xFFFF) * 8;
        return 1;
    case -66:   /* SetFont(a1 rp, a0 font) */
        wl(a[1] + 52, a[0]);
        ww(a[1] + 58, 8);
        ww(a[1] + 60, 8);
        ww(a[1] + 62, 6);
        return 1;
    case -72:   /* OpenFont(a0 textattr) */
        d[0] = open_font();
        return 1;
    case -78:   /* CloseFont */
        return 1;
    case -192: { /* LoadRGB4(a0 vp, a1 colors, d0 count) */
        int n = (int)(d[0] & 0xFFFF);
        for (int i = 0; i < n && i < 32; i++) set_vp_color(a[0], i, (uint16_t)rw(a[1] + 2 * i));
        return 1;
    }
    case -288: { /* SetRGB4(a0 vp, d0 n, d1 r, d2 g, d3 b) */
        set_vp_color(a[0], (int)(d[0] & 0xFFFF),
                     (uint16_t)(((d[1] & 15) << 8) | ((d[2] & 15) << 4) | (d[3] & 15)));
        return 1;
    }
    case -570: { /* GetColorMap(d0 entries) */
        uint32_t n = d[0] & 0xFFFF;
        uint32_t cm = hle_alloc(8 + 2 * n, 1);
        ww(cm + 2, n);
        wl(cm + 4, cm + 8);
        /* Kickstart 1.3 default colours */
        static const uint16_t defaults[32] = {
            0x000, 0xF00, 0x0F0, 0xFF0, 0x00F, 0xF0F, 0x0FF, 0xFFF,
            0x620, 0xE50, 0x9F1, 0xEB0, 0x55F, 0x92F, 0x0F8, 0xCCC,
            0x000, 0xD22, 0x000, 0xFCA, 0x444, 0x555, 0x666, 0x777,
            0x888, 0x999, 0xAAA, 0xBBB, 0xCCC, 0xDDD, 0xEEE, 0xFFF,
        };
        for (uint32_t i = 0; i < n && i < 32; i++) ww(cm + 8 + 2 * i, defaults[i]);
        d[0] = cm;
        return 1;
    }
    case -210:  /* MrgCop(a1 view) */
        mrg_cop(a[1]);
        return 1;
    case -576: case -540: case -564:  /* FreeColorMap, FreeVPortCopLists, FreeCprList */
    case -216:                         /* MakeVPort */
    case -414:                         /* FreeSprite */
    case -456: case -462:              /* OwnBlitter, DisownBlitter */
    case -228:                         /* WaitBlit */
        return 1;
    case -408:  /* GetSprite: no hardware sprites available */
        d[0] = (uint32_t)-1;
        return 1;
    case -204:  /* InitVPort(a0) */
        memset(mem + a[0], 0, 40);
        return 1;
    case -360:  /* InitView(a1) */
        memset(mem + a[1], 0, 18);
        return 1;
    case -198:  /* InitRastPort(a1) */
        memset(mem + a[1], 0, 100);
        wb(a[1] + 24, 0xFF);
        wb(a[1] + 25, 1);
        wb(a[1] + 28, 1);
        ww(a[1] + 34, 0xFFFF);
        return 1;
    case -222:  /* LoadView(a1) */
        video_load_view(a[1]);
        return 1;
    case -270: case -402:  /* WaitTOF, WaitBOVP */
        hle_wait_frame();
        return 1;
    case -390:  /* InitBitMap(a0 bm, d0 depth, d1 width, d2 height) */
        ww(a[0], ((d[1] + 15) >> 4) << 1);
        ww(a[0] + 2, d[2]);
        wb(a[0] + 4, 0);
        wb(a[0] + 5, d[0]);
        return 1;
    case -234: { /* SetRast(a1 rp, d0 pen) */
        Target t = target_rp(a[1]);
        for (int y = t.y0; y <= t.y1; y++)
            for (int x = t.x0; x <= t.x1; x++) put_pixel(&t, x, y, (int)d[0]);
        return 1;
    }
    case -240:  /* Move(a1 rp, d0 x, d1 y) */
        ww(a[1] + 36, d[0]);
        ww(a[1] + 38, d[1]);
        return 1;
    case -246:  /* Draw(a1 rp, d0 x, d1 y) */
        draw_line(a[1], (int16_t)rw(a[1] + 36), (int16_t)rw(a[1] + 38), (int16_t)d[0], (int16_t)d[1]);
        ww(a[1] + 36, d[0]);
        ww(a[1] + 38, d[1]);
        return 1;
    case -306:  /* RectFill(a1 rp, d0 xmin, d1 ymin, d2 xmax, d3 ymax) */
        rect_fill(a[1], (int16_t)d[0], (int16_t)d[1], (int16_t)d[2], (int16_t)d[3]);
        return 1;
    case -324: { /* WritePixel(a1 rp, d0 x, d1 y) */
        Target t = target_rp(a[1]);
        draw_pixel(a[1], &t, (int16_t)d[0], (int16_t)d[1], 1);
        d[0] = 0;
        return 1;
    }
    case -318: { /* ReadPixel */
        Target t = target_rp(a[1]);
        d[0] = (uint32_t)get_pixel(&t, (int16_t)d[0] + t.xo, (int16_t)d[1] + t.yo);
        return 1;
    }
    case -342: wb(a[1] + 25, d[0]); return 1;   /* SetAPen */
    case -348: wb(a[1] + 26, d[0]); return 1;   /* SetBPen */
    case -354: wb(a[1] + 28, d[0]); return 1;   /* SetDrMd */
    case -300: { /* BltClear(a1 mem, d0 count, d1 flags) */
        uint32_t n = d[0];
        if (d[1] & 2) n = (d[0] >> 16) * (d[0] & 0xFFFF);
        memset(mem + (a[1] & ADDR_MASK), 0, n);
        return 1;
    }
    case -492: { /* AllocRaster(d0 w, d1 h) */
        d[0] = hle_alloc(((d[0] + 15) >> 3 & ~1u) * d[1], 1);
        return 1;
    }
    case -498:  /* FreeRaster(a0, d0 w, d1 h) */
        hle_free(a[0], ((d[0] + 15) >> 3 & ~1u) * d[1]);
        return 1;
    }
    return 0;
}

/* ---- layers.library: one level of smart refresh (save/restore under) ------------- */

int hle_layers(int off)
{
    uint32_t *d = R.d, *a = R.a;
    switch (off) {
    case -144:  /* NewLayerInfo */
        d[0] = hle_alloc(0x66, 1);
        return 1;
    case -150:  /* DisposeLayerInfo */
        return 1;
    case -36: case -42: { /* CreateUpfrontLayer(a0 li, a1 bm, d0 x0, d1 y0, d2 x1, d3 y1, d4 flags) */
        uint32_t layer = hle_alloc(0xA0, 1), rp = hle_alloc(100, 1);
        int x0 = (int16_t)d[0], y0 = (int16_t)d[1], x1 = (int16_t)d[2], y1 = (int16_t)d[3];
        ww(layer + 16, x0); ww(layer + 18, y0); ww(layer + 20, x1); ww(layer + 22, y1);
        wl(layer + 12, rp);
        wl(rp, layer);
        wl(rp + 4, a[1]);
        wb(rp + 24, 0xFF); wb(rp + 25, 1); wb(rp + 28, 1);
        wl(rp + 52, open_font());
        ww(rp + 58, 8); ww(rp + 60, 8); ww(rp + 62, 6);
        /* save what is underneath, restored by DeleteLayer */
        Target t = target_bitmap(a[1]);
        int w = x1 - x0 + 1, h = y1 - y0 + 1;
        uint32_t save = hle_alloc((uint32_t)(w * h), 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) mem[save + y * w + x] = (uint8_t)get_pixel(&t, x0 + x, y0 + y);
        wl(layer + 0x90, save);
        wl(layer + 0x94, a[1]);
        d[0] = layer;
        return 1;
    }
    case -90: { /* DeleteLayer(a0 dummy, a1 layer) */
        uint32_t layer = a[1], save = rl(layer + 0x90);
        int x0 = (int16_t)rw(layer + 16), y0 = (int16_t)rw(layer + 18);
        int w = (int16_t)rw(layer + 20) - x0 + 1, h = (int16_t)rw(layer + 22) - y0 + 1;
        Target t = target_bitmap(rl(layer + 0x94));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) put_pixel(&t, x0 + x, y0 + y, mem[save + y * w + x]);
        hle_free(save, (uint32_t)(w * h));
        d[0] = 1;
        return 1;
    }
    case -96: case -102: case -108: case -114: case -120: case -138:  /* locking */
        return 1;
    }
    return 0;
}

/* ---- intuition.library ----------------------------------------------------------------- */

static void print_itext(uint32_t itext)
{
    for (; itext; itext = rl(itext + 16)) {
        char buf[128];
        uint32_t s = rl(itext + 12);
        int i = 0;
        while (s && i < 127 && mem[(s + i) & ADDR_MASK]) { buf[i] = (char)mem[(s + i) & ADDR_MASK]; i++; }
        buf[i] = 0;
        fprintf(stderr, "  | %s\n", buf);
    }
}

int hle_intuition(int off)
{
    switch (off) {
    case -78:   /* CloseWorkBench */
    case -210:  /* OpenWorkBench */
        R.d[0] = 1;
        return 1;
    case -132:  /* GetPrefs(a0 buf, d0 size) */
        memset(mem + R.a[0], 0, R.d[0] & 0xFFFF);
        R.d[0] = R.a[0];
        return 1;
    case -324:  /* SetPrefs */
        R.d[0] = R.a[0];
        return 1;
    case -348:  /* AutoRequest(a0 win, a1 body, a2 pos, a3 neg, ...): answer "positive" */
        fprintf(stderr, "AutoRequest:\n");
        print_itext(R.a[1]);
        R.d[0] = 1;
        return 1;
    case -96:   /* DisplayBeep */
        return 1;
    }
    return 0;
}

/* ---- icon.library: no icons ------------------------------------------------------------- */

int hle_icon(int off)
{
    switch (off) {
    case -78: case -96:  /* GetDiskObject, FindToolType */
        R.d[0] = 0;
        return 1;
    case -90:  /* FreeDiskObject */
        return 1;
    }
    return 0;
}

/* ---- mathffp.library: Motorola fast floating point ------------------------------------- */

static double ffp_to_double(uint32_t v)
{
    uint32_t mant = v >> 8;
    if (!mant) return 0.0;
    double r = ldexp((double)mant, (int)(v & 0x7F) - 64 - 24);
    return (v & 0x80) ? -r : r;
}

static uint32_t double_to_ffp(double x)
{
    if (x == 0.0 || x != x) return 0;
    uint32_t sign = x < 0 ? 0x80 : 0;
    int e;
    double m = frexp(fabs(x), &e);       /* m in [0.5, 1) */
    uint32_t mant = (uint32_t)(m * 16777216.0);
    if (mant > 0xFFFFFF) { mant >>= 1; e++; }
    if (e + 64 < 0) return 0;
    if (e + 64 > 127) return 0xFFFFFF7Fu | sign;
    return (mant << 8) | sign | (uint32_t)(e + 64);
}

static void ffp_result(uint32_t r)
{
    R.d[0] = r;
    R.n = (r & 0x80) != 0 && (r >> 8) != 0;
    R.z = (r >> 8) == 0;
    R.v = R.c = 0;
}

int hle_mathffp(int off)
{
    double x = ffp_to_double(R.d[0]), y = ffp_to_double(R.d[1]);
    switch (off) {
    case -30: { /* SPFix(d0) -> int */
        int32_t i = (int32_t)x;
        R.d[0] = (uint32_t)i;
        R.n = i < 0; R.z = i == 0; R.v = R.c = 0;
        return 1;
    }
    case -36: ffp_result(double_to_ffp((double)(int32_t)R.d[0])); return 1;      /* SPFlt */
    case -42: { /* SPCmp(d1, d0): result and flags as for d0 compared to d1,
                 * i.e. bge after the call means d0 >= d1 (verified against the
                 * game's speed limiter: d1 = 7.0, d0 = speed) */
        int r = x > y ? 1 : (x < y ? -1 : 0);
        R.d[0] = (uint32_t)r;
        R.n = r < 0; R.z = r == 0; R.v = R.c = 0;
        return 1;
    }
    case -48: { /* SPTst(d1) */
        int r = y > 0 ? 1 : (y < 0 ? -1 : 0);
        R.d[0] = (uint32_t)r;
        R.n = r < 0; R.z = r == 0; R.v = R.c = 0;
        return 1;
    }
    case -54: ffp_result(double_to_ffp(fabs(x))); return 1;   /* SPAbs */
    case -60: ffp_result(double_to_ffp(-x)); return 1;        /* SPNeg */
    case -66: ffp_result(double_to_ffp(y + x)); return 1;     /* SPAdd */
    case -72: ffp_result(double_to_ffp(x - y)); return 1;     /* SPSub: d0 - d1 */
    case -78: ffp_result(double_to_ffp(y * x)); return 1;     /* SPMul */
    case -84: ffp_result(double_to_ffp(y != 0 ? x / y : 0)); return 1;  /* SPDiv: d0 / d1 */
    }
    return 0;
}

/* ---- devices -------------------------------------------------------------------------------- */

static uint32_t input_handler;

int hle_device(LibId id, int off)
{
    (void)id;
    (void)off;
    return 0;
}

/* OpenDevice/CloseDevice/DoIO/SendIO/... from exec, by IORequest. */
int hle_device_open(const char *name, uint32_t ioreq)
{
    LibId id = LIB_UNKNOWN;
    if (strcmp(name, "input.device") == 0) id = LIB_DEV_INPUT;
    else if (strcmp(name, "audio.device") == 0) id = LIB_DEV_AUDIO;
    else if (strcmp(name, "timer.device") == 0) id = LIB_DEV_TIMER;
    else if (strcmp(name, "trackdisk.device") == 0) id = LIB_DEV_TRACKDISK;
    else if (strcmp(name, "console.device") == 0) id = LIB_DEV_CONSOLE;
    if (id == LIB_UNKNOWN) return -1;
    wl(ioreq + 20, hle_lib_base(id));
    wb(ioreq + 31, 0);
    return 0;
}

void hle_device_io(uint32_t ioreq)
{
    LibId id = (LibId)((rl(ioreq + 20) - HLE_LIB_AREA) / HLE_LIB_STRIDE);
    uint16_t cmd = (uint16_t)rw(ioreq + 28);
    wb(ioreq + 31, 0);
    if (id == LIB_DEV_INPUT) {
        if (cmd == 9) input_handler = rl(ioreq + 40);        /* IND_ADDHANDLER */
        else if (cmd == 10) input_handler = 0;               /* IND_REMHANDLER */
        return;
    }
    if (hle_trace) fprintf(stderr, "  device %d command %d ignored\n", id, cmd);
}

static int amiga_rawkey(int sc)
{
    static const struct { int sdl, amiga; } map[] = {
        {SDL_SCANCODE_ESCAPE, 0x45}, {SDL_SCANCODE_SPACE, 0x40}, {SDL_SCANCODE_RETURN, 0x44},
        {SDL_SCANCODE_KP_ENTER, 0x43}, {SDL_SCANCODE_BACKSPACE, 0x41}, {SDL_SCANCODE_TAB, 0x42},
        {SDL_SCANCODE_DELETE, 0x46}, {SDL_SCANCODE_UP, 0x4C}, {SDL_SCANCODE_DOWN, 0x4D},
        {SDL_SCANCODE_RIGHT, 0x4E}, {SDL_SCANCODE_LEFT, 0x4F}, {SDL_SCANCODE_LSHIFT, 0x60},
        {SDL_SCANCODE_RSHIFT, 0x61}, {SDL_SCANCODE_LCTRL, 0x63}, {SDL_SCANCODE_LALT, 0x64},
        {SDL_SCANCODE_RALT, 0x65}, {SDL_SCANCODE_MINUS, 0x0B}, {SDL_SCANCODE_EQUALS, 0x0C},
        {SDL_SCANCODE_COMMA, 0x38}, {SDL_SCANCODE_PERIOD, 0x39}, {SDL_SCANCODE_SLASH, 0x3A},
        {SDL_SCANCODE_0, 0x0A}, {SDL_SCANCODE_HOME, 0x5F},
    };
    static const char rows[][11] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const int row_base[] = {0x10, 0x20, 0x31};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].sdl == sc) return map[i].amiga;
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x01 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x50 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char c = (char)('a' + (sc - SDL_SCANCODE_A));
        for (int r = 0; r < 3; r++) {
            const char *p = strchr(rows[r], c);
            if (p) return row_base[r] + (int)(p - rows[r]);
        }
    }
    return -1;
}

void input_raw_event(int code, int down, int repeat);

/* Keyboard timing, modelled on a real Amiga.
 *
 * The game's menus flush the key buffer, draw the screen, then read one
 * key event; only keys arriving while it draws count. On the Amiga the
 * drawing takes real time (blitter work) and keyboard interrupts land in
 * that window. Here every graphics call is instantaneous, so emulated
 * time inside a frame is advanced by an estimate of the blitter work
 * (gfx_cost) and key events are delivered when the emulated clock passes
 * their due time, at any safe point (POLL or library call).
 *
 * Held keys auto-repeat like input.device (initial delay, then a fixed
 * rate). That includes space: the game's "fire held" flag is set when a
 * space event is read and cleared by the flush, so it needs the repeats. */
#define KEY_REPEAT_DELAY_US 160000
#define KEY_REPEAT_US        27000   /* not a multiple of the 20 ms frame */

typedef struct { int code, down, repeat; uint64_t due; } KeyEvent;
static KeyEvent key_queue[32];
static int key_queue_len;
static struct { int code; uint64_t next; } held[8];
static int held_count;
static uint64_t frame_cost_us;      /* blitter time spent this frame */
static uint32_t cost_frame;

uint64_t emu_time_us(void);         /* chips.c: frame * 20000 + time inside frame */


/* Called from the graphics functions: `words` 16-bit words of blitter work. */
static void gfx_cost(uint32_t words)
{
    extern uint32_t frame_counter;
    if (cost_frame != frame_counter) { cost_frame = frame_counter; frame_cost_us = 0; }
    frame_cost_us += words;          /* about 1 us per word for A+B+C+D */
}

uint64_t input_frame_cost_us(void)
{
    extern uint32_t frame_counter;
    return cost_frame == frame_counter ? frame_cost_us : 0;
}

static void queue_key(int code, int down, int repeat, uint64_t due)
{
    if (key_queue_len < 32)
        key_queue[key_queue_len++] = (KeyEvent){code, down, repeat, due};
}

void input_held_event(int code, int down);

void input_key_event(int scancode, int down, int repeat)
{
    int code = amiga_rawkey(scancode);
    if (code >= 0 && !repeat) input_held_event(code, down);
}

void input_held_event(int code, int down)
{
    /* A host key event is only seen at the frame boundary; on the Amiga it
     * would have arrived somewhere within the frame, so spread it over it. */
    static uint32_t spread = 7919;
    spread = spread * 1103515245u + 12345u;
    uint64_t due = emu_time_us() + (spread >> 8) % 20000;
    for (int i = 0; i < held_count; i++)
        if (held[i].code == code) held[i--] = held[--held_count];
    if (down && held_count < 8) {
        held[held_count].code = code;
        held[held_count++].next = due + KEY_REPEAT_DELAY_US;
    }
    queue_key(code, down, 0, due);
}

/* Deliver due key events; called at every safe point. */
void input_service(void)
{
    uint64_t now = emu_time_us();
    for (int i = 0; i < held_count; i++)
        while (held[i].next <= now) {
            queue_key(held[i].code, 1, 1, held[i].next);
            held[i].next += KEY_REPEAT_US;
        }
    for (int i = 0; i < key_queue_len; ) {
        if (key_queue[i].due <= now) {
            KeyEvent e = key_queue[i];
            memmove(&key_queue[i], &key_queue[i + 1], (size_t)(--key_queue_len - i) * sizeof e);
            input_raw_event(e.code, e.down, e.repeat);
        } else {
            i++;
        }
    }
}

void input_raw_event(int code, int down, int repeat)
{
    if (!input_handler) return;
    if (logfn_count) {
        extern uint32_t frame_counter;
        fprintf(stderr, "[key %02x%s%s] frame %u t=%u\n", code, down ? "" : " up", repeat ? " rep" : "",
                frame_counter, (unsigned)(emu_time_us() % 20000));
    }
    static uint32_t event;
    if (!event) event = hle_alloc(22, 1);
    memset(mem + event, 0, 22);
    wb(event + 4, 1);                       /* IECLASS_RAWKEY */
    ww(event + 6, (uint32_t)(code | (down ? 0 : 0x80)));
    if (repeat) ww(event + 8, 0x0200);      /* ie_Qualifier: IEQUALIFIER_REPEAT */
    uint32_t code_ptr = rl(input_handler + 18), data = rl(input_handler + 14);
    hle_callback(code_ptr, event, data);
}
