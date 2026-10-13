/* CPU state, memory, program loader and indirect call dispatch. */
#include <stdarg.h>
#include <string.h>

#include "m68k.h"
#include "dispatch.h"
#include "functions.h"
#include "layout.h"
#include "hle.h"
#include "video.h"
#include "settings.h"
#include <sys/stat.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

CPU R;
uint8_t *mem;

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ftrace_dump();
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n  d: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            R.d[0], R.d[1], R.d[2], R.d[3], R.d[4], R.d[5], R.d[6], R.d[7]);
    fprintf(stderr, "  a: %08x %08x %08x %08x %08x %08x %08x %08x\n",
            R.a[0], R.a[1], R.a[2], R.a[3], R.a[4], R.a[5], R.a[6], R.a[7]);
    va_end(ap);
    exit(1);
}

void (*find_function(uint32_t addr))(void)
{
    int lo = 0, hi = func_table_len - 1;
    addr &= ADDR_MASK;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (func_table[mid].addr == addr) return func_table[mid].fn;
        if (func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* jsr <computed>: the callee pops the return address with rts. */
void call_indirect_ret(uint32_t addr, uint32_t ret)
{
    if (hle_is_library_vector(addr)) {
        hle_call_vector(addr);
        return;
    }
    void (*fn)(void) = find_function(addr);
    if (!fn) rt_fatal("indirect call to unknown address %06x", addr);
    push32(ret);
    fn();
}

/* jmp <computed>: the return address of our caller is already on the stack. */
void call_indirect_tail(uint32_t addr)
{
    if (hle_is_library_vector(addr)) {
        hle_call_vector(addr);
        R.a[7] += 4;
        return;
    }
    void (*fn)(void) = find_function(addr);
    if (!fn) rt_fatal("indirect jump to unknown address %06x", addr);
    fn();
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Load the hunk executable (root and overlay nodes) at the layout addresses
 * the recompiler assumed, applying 32-bit relocations. */
static void load_program(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) rt_fatal("cannot read %s", path);
    fclose(f);

    long pos = 0;
    int hunk = 0;
    int cur = -1;
    while (pos + 4 <= len) {
        uint32_t type = be32(buf + pos) & 0x3FFFFFFF;
        pos += 4;
        switch (type) {
        case 0x3F3: { /* HUNK_HEADER */
            while (be32(buf + pos)) pos += 4 + be32(buf + pos) * 4;
            pos += 4;
            uint32_t first = be32(buf + pos + 4), last = be32(buf + pos + 8);
            pos += 12 + (last - first + 1) * 4;
            hunk = first;
            break;
        }
        case 0x3E9: case 0x3EA: { /* HUNK_CODE, HUNK_DATA */
            uint32_t n = be32(buf + pos) * 4;
            if (hunk >= NUM_HUNKS) rt_fatal("unexpected hunk %d", hunk);
            memcpy(mem + hunk_base[hunk], buf + pos + 4, n);
            cur = hunk++;
            pos += 4 + n;
            break;
        }
        case 0x3EB: /* HUNK_BSS: memory is already zero */
            cur = hunk++;
            pos += 4;
            break;
        case 0x3EC: /* HUNK_RELOC32 */
            while (be32(buf + pos)) {
                uint32_t n = be32(buf + pos), target = be32(buf + pos + 4);
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t at = hunk_base[cur] + be32(buf + pos + 8 + 4 * i);
                    wl(at, rl(at) + hunk_base[target]);
                }
                pos += 8 + n * 4;
            }
            pos += 4;
            break;
        case 0x3F0: /* HUNK_SYMBOL */
            while (be32(buf + pos)) pos += 4 + be32(buf + pos) * 4 + 4;
            pos += 4;
            break;
        case 0x3F1: /* HUNK_DEBUG */
            pos += 4 + be32(buf + pos) * 4;
            break;
        case 0x3F2: /* HUNK_END */
        case 0x3F6: /* HUNK_BREAK */
            break;
        case 0x3F5: /* HUNK_OVERLAY: skip the overlay table */
            pos += 4 + (be32(buf + pos) + 1) * 4;
            break;
        default:
            rt_fatal("unknown hunk type %x at %lx", type, pos - 4);
        }
    }
    free(buf);
}

uint32_t ftrace_ring[64];
unsigned ftrace_pos;
uint32_t ftrace_break;
int logfn_count;
static uint32_t logfn_addrs[8];
extern uint32_t frame_counter;

void logfn_check(uint32_t addr)
{
    for (int i = 0; i < logfn_count; i++) {
        if (logfn_addrs[i] != addr) continue;
        /* the game's key ring: write index $b28c, read index $b28e (data hunk) */
        uint32_t data = hunk_base[1];
        extern uint64_t emu_time_us(void);
        fprintf(stderr, "[fn %06x] frame %u t=%u keyring w=%u r=%u\n", addr, frame_counter,
                (unsigned)(emu_time_us() % 20000), rw(data + 0xB28C), rw(data + 0xB28E));
    }
}

void ftrace_dump(void)
{
    fprintf(stderr, "recent functions:");
    for (unsigned i = ftrace_pos - 40; i != ftrace_pos; i++) {
        uint32_t a = ftrace_ring[i & 63];
        int h = 0;
        while (h + 1 < NUM_HUNKS && hunk_base[h + 1] <= a) h++;
        int node = h == 0 ? 0 : h - 2;
        fprintf(stderr, " %02d:%05x", node, a - hunk_base[h]);
    }
    fprintf(stderr, "\n");
}

uint32_t watch_lo, watch_len;
extern uint32_t frame_counter;

void watch_hit(uint32_t addr)
{
    static uint32_t seen[256];
    static int nseen;
    uint32_t fn = ftrace_ring[(ftrace_pos - 1) & 63];
    for (int i = 0; i < nseen; i++) if (seen[i] == fn) return;
    if (nseen < 256) seen[nseen++] = fn;
    int h = 0;
    while (h + 1 < NUM_HUNKS && hunk_base[h + 1] <= fn) h++;
    fprintf(stderr, "[watch] frame %u: %06x written by %02d:%05x\n", frame_counter, addr,
            h == 0 ? 0 : h - 2, fn - hunk_base[h]);
}

/* Music runs at a fixed 50 Hz regardless of the frame rate: the game's
 * vertical blank server calls the player (0x58bc), which is gated, and the
 * runtime calls it here with the gate open. */
static int music_open;

int copy_protection_off(void)
{
    return !settings.copy_protection;
}

int music_gate(void)
{
    return music_open;
}

void music_tick(void)
{
    CPU saved = R;
    R.a[4] = rl(hunk_base[1] + 0x2B4A);   /* as the vblank server sets it */
    R.a[7] -= 16;
    music_open = 1;
    call_indirect_ret(hunk_base[0] + 0x58BC, 0x00FFFFF4u);
    music_open = 0;
    R = saved;
}

static void at_exit_report(void)
{
    ftrace_dump();
    fprintf(stderr, "[exit] last loop %06x  a7=%08x d0=%08x\n", poll_pc, R.a[7], R.d[0]);
}

/* A relative game folder is tried from the current directory first, then
 * from the program's own folder (so double-clicking the .exe works). */
static const char *resolve_gamedir(const char *dir)
{
    static char path[1600];
    struct stat st;
    snprintf(path, sizeof path, "%s/DT1/DuckTales", dir);
    if (stat(path, &st) == 0) return dir;
    char *base = SDL_GetBasePath();
    snprintf(path, sizeof path, "%s%s", base ? base : "", dir);
    SDL_free(base);
    char probe[1700];
    snprintf(probe, sizeof probe, "%s/DT1/DuckTales", path);
    return stat(probe, &st) == 0 ? path : dir;
}

int main(int argc, char **argv)
{
    if (getenv("DT_DEBUG")) atexit(at_exit_report);
    if (getenv("DT_WATCH")) sscanf(getenv("DT_WATCH"), "%x,%x", &watch_lo, &watch_len);
    if (getenv("DT_LOGFN")) {
        const char *s = getenv("DT_LOGFN");
        while (*s && logfn_count < 8) {
            logfn_addrs[logfn_count++] = (uint32_t)strtoul(s, (char **)&s, 16);
            if (*s == ',') s++;
        }
    }
    if (getenv("DT_BREAK")) ftrace_break = (uint32_t)strtoul(getenv("DT_BREAK"), NULL, 16);
    /* gamedir holds the disk contents as DT1/ and DT2/ subdirectories. */
    /* defaults < ducktales.ini (settings.exe) < environment < command line */
    settings_defaults(&settings);
    settings_load(&settings, settings_path());
    if (getenv("DT_FPS")) settings.fps = atoi(getenv("DT_FPS"));
    if (getenv("DT_MUSIC_HZ")) settings.music_hz = atoi(getenv("DT_MUSIC_HZ"));
    if (getenv("DT_NOSOUND")) settings.sound = 0;
    const char *gamedir = settings.gamedir;
    int fps = settings.fps;
    int music_hz = settings.music_hz;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--fps=", 6)) fps = atoi(argv[i] + 6);
        else if (!strcmp(argv[i], "--music-hz") && i + 1 < argc) music_hz = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--music-hz=", 11)) music_hz = atoi(argv[i] + 11);
        else gamedir = argv[i];
    }
    settings.fps = fps;
    settings.music_hz = music_hz;
    settings_clamp(&settings);
    gamedir = resolve_gamedir(gamedir);
    char path[1024];
    mem = calloc(1, MEM_SIZE);
    if (!mem) rt_fatal("out of memory");

    snprintf(path, sizeof path, "%s/DT1/DuckTales", gamedir);
    load_program(path);
    hle_init(gamedir, LOAD_END);
    chips_set_music_hz(music_hz);
    chips_set_fps(fps);

    /* AmigaDOS CLI start: stack with return address, A0/D0 = argument line. */
    R.a[7] = HLE_STACK_TOP;
    R.a[4] = 0;
    R.a[0] = hle_alloc_string("\n");
    R.d[0] = 1;
    push32(HLE_EXIT_ADDR);
    ENTRY_FUNC();
    rt_program_exit();
    return 0;
}

void rt_program_exit(void)
{
    if (hle_trace) fprintf(stderr, "program exited, d0=%u\n", R.d[0]);
    exit((int)R.d[0]);
}
