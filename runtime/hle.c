/* HLE core: memory allocator, library bases, exec.library and dos.library. */
#include <string.h>

#include "m68k.h"
#include "hle.h"
#include "video.h"

static void raw_do_fmt(void);
static void read_cstring(uint32_t addr, char *out, size_t max);

int hle_trace = 0;
const char *hle_gamedir;

static const char *const lib_names[] = {
    "exec.library", "dos.library", "graphics.library", "intuition.library",
    "layers.library", "icon.library", "mathffp.library", "input.device",
    "audio.device", "timer.device", "trackdisk.device", "console.device",
};

/* ---- memory -------------------------------------------------------------- */

/* First-fit allocator over the heap area; blocks are 8-byte aligned. */
typedef struct Block { uint32_t addr, size; struct Block *next; } Block;
static Block *free_list;

static void heap_init(void)
{
    free_list = calloc(1, sizeof *free_list);
    free_list->addr = HLE_HEAP_START;
    free_list->size = HLE_HEAP_END - HLE_HEAP_START;
}

uint32_t hle_alloc(uint32_t size, int clear)
{
    size = (size + 7) & ~7u;
    for (Block **p = &free_list; *p; p = &(*p)->next) {
        Block *b = *p;
        if (b->size >= size) {
            uint32_t addr = b->addr;
            b->addr += size;
            b->size -= size;
            if (b->size == 0) { *p = b->next; free(b); }
            if (clear) memset(mem + addr, 0, size);
            return addr;
        }
    }
    return 0;
}

void hle_free(uint32_t addr, uint32_t size)
{
    size = (size + 7) & ~7u;
    if (!addr || !size) return;
    Block **p = &free_list;
    while (*p && (*p)->addr < addr) p = &(*p)->next;
    Block *b = calloc(1, sizeof *b);
    b->addr = addr;
    b->size = size;
    b->next = *p;
    *p = b;
    /* coalesce with the following and the previous block */
    for (Block *q = free_list; q && q->next; ) {
        if (q->addr + q->size == q->next->addr) {
            Block *n = q->next;
            q->size += n->size;
            q->next = n->next;
            free(n);
        } else {
            q = q->next;
        }
    }
}

static uint32_t avail_mem(void)
{
    uint32_t total = 0;
    for (Block *b = free_list; b; b = b->next) total += b->size;
    return total;
}

uint32_t hle_alloc_string(const char *s)
{
    uint32_t len = (uint32_t)strlen(s) + 1;
    uint32_t a = hle_alloc(len, 1);
    memcpy(mem + a, s, len);
    return a;
}

static void read_cstring(uint32_t addr, char *out, size_t max)
{
    size_t i = 0;
    for (; i + 1 < max && mem[(addr + i) & ADDR_MASK]; i++) out[i] = (char)mem[(addr + i) & ADDR_MASK];
    out[i] = 0;
}

/* ---- libraries ----------------------------------------------------------- */

uint32_t hle_lib_base(LibId id)
{
    /* Vectors extend 0x8000 below the base, structure fields above it. */
    return HLE_LIB_AREA + (uint32_t)id * HLE_LIB_STRIDE + 0x8000u;
}

static LibId lib_from_base(uint32_t base)
{
    if (base < HLE_LIB_AREA) return LIB_UNKNOWN;
    uint32_t i = (base - HLE_LIB_AREA) / HLE_LIB_STRIDE;
    return i < LIB_UNKNOWN ? (LibId)i : LIB_UNKNOWN;
}

int hle_is_library_vector(uint32_t addr)
{
    addr &= ADDR_MASK;
    if (addr < HLE_LIB_AREA || addr >= HLE_LIB_AREA + LIB_UNKNOWN * HLE_LIB_STRIDE) return 0;
    return ((addr - HLE_LIB_AREA) % HLE_LIB_STRIDE) < 0x8000u;
}

void hle_call_vector(uint32_t addr)
{
    LibId id = lib_from_base(addr);
    lib_call(hle_lib_base(id), (int32_t)(addr - hle_lib_base(id)));
}

void input_service(void);
void chips_service(void);

void lib_call(uint32_t base, int32_t offset)
{
    /* Library calls are points where (emulated) time has passed, so
     * keyboard interrupts that became due are delivered here too. */
    chips_service();
    input_service();
    LibId id = lib_from_base(base);
    const char *name = id < LIB_UNKNOWN ? lib_names[id] : "?";
    const char *fn = id < LIB_UNKNOWN ? hle_lvo_name(id, offset) : NULL;
    if (hle_trace)
        fprintf(stderr, "[%s %s(%d)] d0=%x d1=%x a0=%x a1=%x\n", name, fn ? fn : "?", offset,
                R.d[0], R.d[1], R.a[0], R.a[1]);
    int ok;
    switch (id) {
    case LIB_EXEC: ok = hle_exec(offset); break;
    case LIB_DOS: ok = hle_dos(offset); break;
    case LIB_GRAPHICS: ok = hle_graphics(offset); break;
    case LIB_INTUITION: ok = hle_intuition(offset); break;
    case LIB_LAYERS: ok = hle_layers(offset); break;
    case LIB_ICON: ok = hle_icon(offset); break;
    case LIB_MATHFFP: ok = hle_mathffp(offset); break;
    case LIB_UNKNOWN: ok = 0; break;
    default: ok = hle_device(id, offset); break;
    }
    if (!ok)
        rt_fatal("unimplemented library call %s %s (%d), base %06x", name, fn ? fn : "?", offset, base);
}

static uint32_t open_library(const char *name)
{
    for (int i = 0; i < LIB_UNKNOWN; i++)
        if (strcmp(name, lib_names[i]) == 0) return hle_lib_base((LibId)i);
    fprintf(stderr, "OpenLibrary: unknown library %s\n", name);
    return 0;
}

/* Fake process for FindTask(NULL): looks like a CLI process. */
static uint32_t this_task;

void hle_init(const char *gamedir, uint32_t load_end)
{
    (void)load_end;
    hle_gamedir = gamedir;
    hle_trace = getenv("HLE_TRACE") != NULL;
    heap_init();
    video_init();
    chips_init();
    wl(4, hle_lib_base(LIB_EXEC));

    uint32_t exec = hle_lib_base(LIB_EXEC);
    ww(exec + 0x128, 0);              /* AttnFlags: plain 68000 */
    ww(exec + 0x14, 34);              /* lib_Version: Kickstart 1.3 */

    this_task = hle_alloc(0x100, 1);  /* struct Process */
    wb(this_task + 8, 13);            /* ln_Type = NT_PROCESS */
    wl(exec + 0x114, this_task);      /* ThisTask */
    /* DOS device list with both game disks mounted as volumes:
     * DosLibrary.dl_Root @34 -> RootNode.rn_Info @24 (BPTR) -> DosInfo.di_DevInfo
     * @4 (BPTR) -> DeviceList { dl_Next BPTR @0, dl_Type @4, dl_Task @8, dl_Name BSTR @40 } */
    uint32_t dos = hle_lib_base(LIB_DOS);
    uint32_t root = hle_alloc(0x40, 1), info = hle_alloc(0x20, 1);
    wl(dos + 34, root);
    wl(root + 24, info >> 2);
    static const char *const volumes[] = {"DT1", "DT2"};
    uint32_t next = 0;
    for (int i = 1; i >= 0; i--) {
        uint32_t dl = hle_alloc(0x30, 1), name = hle_alloc(8, 1);
        wb(name, (uint32_t)strlen(volumes[i]));
        memcpy(mem + name + 1, volumes[i], strlen(volumes[i]));
        wl(dl, next >> 2);
        wl(dl + 4, 2);                /* DLT_VOLUME */
        wl(dl + 8, 0x00FE0000u);      /* dl_Task: non-zero means mounted */
        wl(dl + 40, name >> 2);
        next = dl;
    }
    wl(info + 4, next >> 2);

    uint32_t cli = hle_alloc(0x40, 1);
    wl(this_task + 0xAC, cli >> 2);   /* pr_CLI (BPTR) */
    wl(this_task + 0x3E, HLE_STACK_TOP - 0x20000); /* tc_SPLower */
    wl(this_task + 0x3A, HLE_STACK_TOP);           /* tc_SPUpper */
}

/* exec Allocate/Deallocate on a caller-owned MemHeader:
 * mh_First @16, mh_Lower @20, mh_Upper @24, mh_Free @28;
 * MemChunk: mc_Next @0, mc_Bytes @4. Sizes round up to 8 bytes. */
static uint32_t mh_allocate(uint32_t mh, uint32_t size)
{
    size = (size + 7) & ~7u;
    if (!size || size > rl(mh + 28)) return 0;
    uint32_t prev = mh + 16;   /* mh_First acts as mc_Next of a pseudo chunk */
    for (uint32_t mc = rl(prev); mc; prev = mc, mc = rl(mc)) {
        uint32_t bytes = rl(mc + 4);
        if (bytes < size) continue;
        if (bytes == size) {
            wl(prev, rl(mc));
        } else {
            uint32_t rest = mc + size;
            wl(rest, rl(mc));
            wl(rest + 4, bytes - size);
            wl(prev, rest);
        }
        wl(mh + 28, rl(mh + 28) - size);
        return mc;
    }
    return 0;
}

static void mh_deallocate(uint32_t mh, uint32_t block, uint32_t size)
{
    size = (size + 7) & ~7u;
    if (!block || !size) return;
    block &= ~7u;
    uint32_t prev = mh + 16, mc = rl(prev);
    while (mc && mc < block) { prev = mc; mc = rl(mc); }
    wl(block, mc);
    wl(block + 4, size);
    wl(prev, block);
    if (mc && block + size == mc) {          /* merge with next */
        wl(block, rl(mc));
        wl(block + 4, size + rl(mc + 4));
    }
    if (prev != mh + 16 && prev + rl(prev + 4) == block) {   /* merge with previous */
        wl(prev, rl(block));
        wl(prev + 4, rl(prev + 4) + rl(block + 4));
    }
    wl(mh + 28, rl(mh + 28) + size);
}

/* ---- exec.library ------------------------------------------------------------ */

int hle_exec(int offset)
{
    char name[128];
    switch (offset) {
    case -30:   /* Supervisor: only used for CPU detection; report a 68000 */
        R.d[0] = 0;
        return 1;
    case -120: case -126: case -132: case -138:  /* Disable/Enable/Forbid/Permit */
        return 1;
    case -198: { /* AllocMem(size d0, flags d1) */
        uint32_t a = hle_alloc(R.d[0], (R.d[1] & 0x10000) != 0);
        if (hle_trace) fprintf(stderr, "  AllocMem(%u) -> %06x\n", R.d[0], a);
        R.d[0] = a;
        return 1;
    }
    case -210:  /* FreeMem(a1, d0) */
        hle_free(R.a[1], R.d[0]);
        return 1;
    case -216:  /* AvailMem */
        R.d[0] = avail_mem();
        return 1;
    case -294:  /* FindTask */
        R.d[0] = R.a[1] ? 0 : this_task;
        return 1;
    case -306:  /* SetSignal: no signals pending */
        R.d[0] = 0;
        return 1;
    case -408:  /* OldOpenLibrary(a1) */
    case -552:  /* OpenLibrary(a1, d0) */
        read_cstring(R.a[1], name, sizeof name);
        R.d[0] = open_library(name);
        if (hle_trace) fprintf(stderr, "  open %s -> %06x\n", name, R.d[0]);
        return 1;
    case -414:  /* CloseLibrary */
        return 1;
    case -108:  /* Alert */
        rt_fatal("Alert(%08x)", R.d[7]);
        return 1;
    case -162:  /* SetIntVector(d0 num, a1 interrupt) */
        R.d[0] = set_int_vector((int)R.d[0], R.a[1]);
        return 1;
    case -168:  /* AddIntServer(d0 num, a1 interrupt) */
        add_int_server((int)R.d[0], R.a[1]);
        return 1;
    case -174:  /* RemIntServer */
        rem_int_server((int)R.d[0], R.a[1]);
        return 1;
    case -186:  /* Allocate(a0 header, d0 size) */
        if (getenv("DT_MEMLOG")) fprintf(stderr, "[mem] alloc %u", R.d[0]);
        R.d[0] = mh_allocate(R.a[0], R.d[0]);
        if (getenv("DT_MEMLOG")) fprintf(stderr, " -> %06x free %u\n", R.d[0], rl(R.a[0] + 28));
        return 1;
    case -192:  /* Deallocate(a0 header, a1 block, d0 size) */
        mh_deallocate(R.a[0], R.a[1], R.d[0]);
        if (getenv("DT_MEMLOG"))
            fprintf(stderr, "[mem] free %06x %u free %u\n", R.a[1], R.d[0], rl(R.a[0] + 28));
        return 1;
    case -246: { /* AddTail(a0 list, a1 node) */
        uint32_t list = R.a[0], node = R.a[1], pred = rl(list + 8);
        wl(node, list + 4);
        wl(node + 4, pred);
        wl(pred, node);
        wl(list + 8, node);
        return 1;
    }
    case -264: { /* RemTail(a0 list) */
        uint32_t list = R.a[0], node = rl(list + 8), pred = rl(node + 4);
        if (!pred) { R.d[0] = 0; return 1; }
        wl(list + 8, pred);
        wl(pred, list + 4);
        R.d[0] = node;
        return 1;
    }
    case -330:  /* AllocSignal */
        R.d[0] = (int32_t)R.d[0] >= 0 ? R.d[0] : 16;
        return 1;
    case -336:  /* FreeSignal */
    case -354: case -360:  /* AddPort, RemPort */
    case -378:  /* ReplyMsg */
        return 1;
    case -372:  /* GetMsg: no messages */
        R.d[0] = 0;
        return 1;
    case -384:  /* WaitPort: nothing ever arrives; let a frame pass */
        hle_wait_frame();
        R.d[0] = 0;
        return 1;
    case -444:  /* OpenDevice(a0 name, d0 unit, a1 ioreq, d1 flags) */
        read_cstring(R.a[0], name, sizeof name);
        R.d[0] = (uint32_t)hle_device_open(name, R.a[1]);
        if (hle_trace) fprintf(stderr, "  OpenDevice %s -> %d\n", name, (int)R.d[0]);
        return 1;
    case -450:  /* CloseDevice */
        return 1;
    case -456: case -462:  /* DoIO, SendIO */
        hle_device_io(R.a[1]);
        R.d[0] = 0;
        return 1;
    case -468:  /* CheckIO: always complete */
        R.d[0] = R.a[1];
        return 1;
    case -474: case -480:  /* WaitIO, AbortIO */
        R.d[0] = 0;
        return 1;
    case -510:  /* RawMayGetChar */
        R.d[0] = (uint32_t)-1;
        return 1;
    case -516:  /* RawPutChar */
        fputc((int)(R.d[0] & 0xFF), stderr);
        return 1;
    case -522:  /* RawDoFmt(a0 fmt, a1 data, a2 putch, a3 putchdata) */
        if (hle_trace) ftrace_dump();
        raw_do_fmt();
        return 1;
    }
    return 0;
}

/* exec RawDoFmt: format into a host buffer, then feed it to the putch
 * routine one character at a time (it usually stores to (a3)+). */
static void raw_do_fmt(void)
{
    uint32_t fmt = R.a[0], data = R.a[1], putch = R.a[2];
    char out[1024];
    size_t n = 0;
    while (n < sizeof out - 1) {
        char c = (char)mem[fmt++ & ADDR_MASK];
        if (!c) break;
        if (c != '%') { out[n++] = c; continue; }
        char spec[16] = "%";
        int k = 1, is_long = 0;
        while ((c = (char)mem[fmt & ADDR_MASK]) && strchr("-0123456789.", c) && k < 12) { spec[k++] = c; fmt++; }
        if (mem[fmt & ADDR_MASK] == 'l') { is_long = 1; fmt++; }
        c = (char)mem[fmt++ & ADDR_MASK];
        char tmp[256];
        if (c == 'd' || c == 'u' || c == 'x' || c == 'X' || c == 'c') {
            uint32_t v = is_long ? rl(data) : rw(data);
            data += is_long ? 4 : 2;
            spec[k++] = c;
            spec[k] = 0;
            if (c == 'd') snprintf(tmp, sizeof tmp, spec, is_long ? (int32_t)v : (int16_t)v);
            else snprintf(tmp, sizeof tmp, spec, v);
        } else if (c == 's' || c == 'b') {
            uint32_t p = rl(data);
            data += 4;
            char s[200];
            if (c == 'b') {   /* BSTR */
                p <<= 2;
                int len = mem[p & ADDR_MASK];
                for (int i = 0; i < len && i < 199; i++) s[i] = (char)mem[(p + 1 + i) & ADDR_MASK];
                s[len < 199 ? len : 199] = 0;
            } else {
                read_cstring(p, s, sizeof s);
            }
            spec[k++] = 's';
            spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, s);
        } else {
            tmp[0] = c;
            tmp[1] = 0;
        }
        for (char *t = tmp; *t && n < sizeof out - 1; t++) out[n++] = *t;
    }
    out[n++] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t a3 = R.a[3];
        if (rw(putch) == 0x16C0 && rw(putch + 2) == 0x4E75) {  /* move.b d0,(a3)+; rts */
            wb(a3, (uint8_t)out[i]);
            R.a[3] = a3 + 1;
        } else {
            R.d[0] = (uint8_t)out[i];
            R.a[3] = hle_callback_a3(putch, a3);
        }
    }
    R.d[0] = data;
}

/* ---- dos.library ------------------------------------------------------------- */

/* File handles are never reused: AmigaDOS handles are BPTRs to fresh
 * structures, and the game closes a stale handle after the file it named
 * was replaced, which must not hit the replacement. */
#define MAX_FILES 32
static struct { uint32_t handle; FILE *f; } files[MAX_FILES];
static uint32_t next_handle = 0x100;

static FILE *file_of(uint32_t handle)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].f && files[i].handle == handle) return files[i].f;
    return NULL;
}

/* Map an AmigaDOS path ("DT1:main.arc", "dt2:dt2.as") to the host. */
static void host_path(const char *amiga, char *out, size_t max)
{
    const char *colon = strchr(amiga, ':');
    if (colon) {
        char vol[16] = {0};
        size_t n = (size_t)(colon - amiga);
        for (size_t i = 0; i < n && i < sizeof vol - 1; i++)
            vol[i] = (char)((amiga[i] >= 'a' && amiga[i] <= 'z') ? amiga[i] - 32 : amiga[i]);
        snprintf(out, max, "%s/%s/%s", hle_gamedir, vol, colon + 1);
    } else {
        snprintf(out, max, "%s/DT1/%s", hle_gamedir, amiga);
    }
}

int hle_dos(int offset)
{
    char name[256], path[512];
    switch (offset) {
    case -30: { /* Open(d1 name, d2 mode) */
        read_cstring(R.d[1], name, sizeof name);
        if (strcmp(name, "*") == 0 || strncmp(name, "CON:", 4) == 0) {
            R.d[0] = 0x7FFF;  /* console */
            return 1;
        }
        host_path(name, path, sizeof path);
        const char *mode = R.d[2] == 1006 ? "wb" : (R.d[2] == 1004 ? "r+b" : "rb");
        int slot = 0;
        while (slot < MAX_FILES && files[slot].f) slot++;
        FILE *f = slot < MAX_FILES ? fopen(path, mode) : NULL;
        if (!f && R.d[2] == 1004) f = fopen(path, "w+b");
        if (hle_trace || !f) fprintf(stderr, "  Open(%s -> %s) = %s\n", name, path, f ? "ok" : "FAIL");
        R.d[0] = 0;
        if (f) {
            files[slot].f = f;
            files[slot].handle = next_handle++;
            R.d[0] = files[slot].handle;
        }
        return 1;
    }
    case -36:   /* Close(d1) */
        for (int i = 0; i < MAX_FILES; i++)
            if (files[i].f && files[i].handle == R.d[1]) { fclose(files[i].f); files[i].f = NULL; }
        R.d[0] = 1;
        return 1;
    case -42: { /* Read(d1 fh, d2 buf, d3 len) */
        FILE *f = file_of(R.d[1]);
        R.d[0] = f ? (uint32_t)fread(mem + (R.d[2] & ADDR_MASK), 1, R.d[3], f) : (uint32_t)-1;
        return 1;
    }
    case -48: { /* Write(d1 fh, d2 buf, d3 len) */
        FILE *f = file_of(R.d[1]);
        if (R.d[1] == 0x7FFF) f = stdout;
        R.d[0] = f ? (uint32_t)fwrite(mem + (R.d[2] & ADDR_MASK), 1, R.d[3], f) : (uint32_t)-1;
        return 1;
    }
    case -54: case -60:  /* Input/Output: a pseudo handle for the console */
        R.d[0] = 0x7FFF;
        return 1;
    case -66: { /* Seek(d1 fh, d2 pos, d3 mode) -> old position */
        FILE *f = file_of(R.d[1]);
        if (!f) { R.d[0] = (uint32_t)-1; return 1; }
        long old = ftell(f);
        int whence = (int32_t)R.d[3] == -1 ? SEEK_SET : (R.d[3] == 1 ? SEEK_END : SEEK_CUR);
        R.d[0] = fseek(f, (int32_t)R.d[2], whence) == 0 ? (uint32_t)old : (uint32_t)-1;
        return 1;
    }
    case -84:   /* Lock: succeed with a dummy lock */
        R.d[0] = 1;
        return 1;
    case -90:   /* UnLock */
    case -126:  /* CurrentDir */
        R.d[0] = 0;
        return 1;
    case -132:  /* IoErr */
        R.d[0] = 0;
        return 1;
    case -216:  /* IsInteractive(d1): only the console is */
        R.d[0] = R.d[1] == 0x7FFF ? (uint32_t)-1 : 0;
        return 1;
    case -198:  /* Delay(d1 ticks), 50 ticks per second like the frame rate */
        for (uint32_t i = 0; i < R.d[1] && i < 500; i++) hle_wait_frame();
        return 1;
    }
    return 0;
}
