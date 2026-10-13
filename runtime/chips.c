/* Custom chips and CIAs: blitter, Paula audio, joystick/mouse, beam
 * position, interrupts. Also drives the 50 Hz frame clock. */
#include <SDL.h>
#include <string.h>

#include "m68k.h"
#include "hle.h"
#include "video.h"
#include "settings.h"

#define PAL_CLOCK 3546895.0
#define AUDIO_RATE 44100
/* Frame rate is user-adjustable (1-200, default 50 = PAL). The game's own
 * timing counts frames, so this sets the game speed; audio is generated
 * in real time so its pitch stays correct. */
static int fps = 50;
static double frame_ms = 20.0;
#define FRAME_MS frame_ms

/* Music player rate, independent of the frame rate (1-200 Hz, default
 * 50 = the original tempo). */
static int music_hz = 50;

static void update_title(void)
{
    char title[160];
    snprintf(title, sizeof title,
             "DuckTales: The Quest for Gold  -  %d fps (PgUp/PgDn)  -  music %d Hz (Ctrl+PgUp/PgDn)",
             fps, music_hz);
    video_set_title(title);
}

void chips_set_fps(int value)
{
    if (value < 1) value = 1;
    if (value > 200) value = 200;
    fps = value;
    frame_ms = 1000.0 / value;
    video_set_vsync(value <= 60);
    update_title();
}

void chips_set_music_hz(int value)
{
    if (value < 1) value = 1;
    if (value > 200) value = 200;
    music_hz = value;
    update_title();
}

int chips_get_fps(void) { return fps; }

int poll_count = 1000;
uint32_t poll_pc;
uint32_t frame_counter;

static uint16_t regs[0x100];           /* custom registers, word index */
/* AmigaOS leaves the master enable (INTEN), vertical blank, CIA ports and
 * software interrupts on, and bitplane/copper/blitter DMA running; games
 * only add the bits they need on top. */
static uint16_t dmacon = 0x03F0, intena = 0x402C, intreq;
static int headless;

/* ---- interrupts ------------------------------------------------------------ */

static uint32_t vertb_servers[8];
static int vertb_count;
static uint32_t int_vectors[16];
static int in_interrupt;

void add_int_server(int num, uint32_t is)
{
    if (num == 5 && vertb_count < 8) vertb_servers[vertb_count++] = is;
}

void rem_int_server(int num, uint32_t is)
{
    if (num != 5) return;
    for (int i = 0; i < vertb_count; i++)
        if (vertb_servers[i] == is) {
            vertb_servers[i] = vertb_servers[--vertb_count];
            return;
        }
}

uint32_t set_int_vector(int num, uint32_t is)
{
    uint32_t old = int_vectors[num & 15];
    int_vectors[num & 15] = is;
    return old;
}

/* Run an Interrupt structure's code with the register conventions of exec. */
static void run_interrupt(uint32_t is, uint32_t intbits)
{
    if (!is) return;
    uint32_t code = rl(is + 18);
    if (!code) return;
    CPU saved = R;
    R.a[1] = rl(is + 14);
    R.a[5] = code;
    R.a[0] = 0xDFF000;
    R.a[6] = rl(4);
    R.d[1] = intbits;
    R.a[7] -= 16;       /* keep the interrupted function's stack intact */
    in_interrupt++;
    call_indirect_ret(code, HLE_CALLBACK_ADDR);
    in_interrupt--;
    R = saved;
}

uint32_t hle_callback(uint32_t code, uint32_t a0, uint32_t a1)
{
    CPU saved = R;
    R.a[0] = a0;
    R.a[1] = a1;
    R.a[7] -= 16;
    call_indirect_ret(code, HLE_CALLBACK_ADDR);
    uint32_t d0 = R.d[0];
    R = saved;
    return d0;
}

uint32_t hle_callback_a3(uint32_t code, uint32_t a3)
{
    CPU saved = R;
    R.a[3] = a3;
    R.a[7] -= 16;
    call_indirect_ret(code, HLE_CALLBACK_ADDR);
    a3 = R.a[3];
    R = saved;
    return a3;
}

static void raise_irq(int bit)
{
    intreq |= (uint16_t)(1 << bit);
    if (in_interrupt) return;
    if (!(intena & 0x4000) || !(intena & (1 << bit))) return;
    run_interrupt(int_vectors[bit], 1u << bit);
}

/* ---- Paula audio ------------------------------------------------------------ */

typedef struct {
    uint32_t lc, pt;        /* latched start and current pointer */
    uint32_t len, left;     /* length in words */
    uint16_t per, vol;
    double pos;             /* fractional sample position within the word */
    int8_t cur[2];
    int on;
} Channel;

static Channel ch[4];
static SDL_AudioDeviceID audio_dev;
static double audio_debt;

static int start_irq_pending;

static void channel_start(int i)
{
    Channel *c = &ch[i];
    c->lc = ((uint32_t)regs[(0xA0 + 16 * i) / 2] << 16 | regs[(0xA2 + 16 * i) / 2]) & ADDR_MASK;
    c->len = regs[(0xA4 + 16 * i) / 2];
    if (!c->len) c->len = 0x10000;
    c->pt = c->lc;
    c->left = c->len;
    c->pos = 0;
    c->on = 1;
    /* Paula raises "block started" only after DMA fetched the first word,
     * i.e. after the CPU has gone on to clear INTREQ and enable INTENA.
     * Raising it here would be lost, the player would not queue the next
     * block, and the sample would play twice. Deliver it a moment later. */
    start_irq_pending |= 1 << i;
}

/* Deliver deferred block-start interrupts (called at safe points). */
void chips_service(void)
{
    if (!start_irq_pending || in_interrupt) return;
    int pending = start_irq_pending;
    start_irq_pending = 0;
    for (int i = 0; i < 4; i++)
        if (pending & (1 << i)) raise_irq(7 + i);
}

static void audio_generate(int frames)
{
    chips_service();
    int16_t buf[2 * 2048];
    while (frames > 0) {
        int n = frames > 2048 ? 2048 : frames;
        for (int s = 0; s < n; s++) {
            int mix[2] = {0, 0};
            for (int i = 0; i < 4; i++) {
                Channel *c = &ch[i];
                if (!c->on || !(dmacon & 0x200) || !(dmacon & (1 << i))) continue;
                uint32_t word_addr = c->pt + ((uint32_t)c->pos & ~1u);
                int8_t smp = (int8_t)mem[(word_addr + ((uint32_t)c->pos & 1)) & ADDR_MASK];
                int vol = c->vol > 64 ? 64 : c->vol;
                mix[(i == 0 || i == 3) ? 0 : 1] += smp * vol;
                int per = c->per < 113 ? 113 : c->per;
                c->pos += PAL_CLOCK / per / AUDIO_RATE;
                while (c->pos >= 2.0) {
                    c->pos -= 2.0;
                    c->pt += 2;
                    if (--c->left == 0) {
                        c->pt = ((uint32_t)regs[(0xA0 + 16 * i) / 2] << 16 | regs[(0xA2 + 16 * i) / 2]) & ADDR_MASK;
                        c->len = regs[(0xA4 + 16 * i) / 2];
                        if (!c->len) c->len = 0x10000;
                        c->left = c->len;
                        raise_irq(7 + i);
                    }
                }
            }
            int l = (mix[0] * 2 + mix[1] / 2) * settings.volume / 100;
            int r = (mix[1] * 2 + mix[0] / 2) * settings.volume / 100;
            buf[2 * s] = (int16_t)l;
            buf[2 * s + 1] = (int16_t)r;
        }
        if (audio_dev) SDL_QueueAudio(audio_dev, buf, (Uint32)(n * 4));
        frames -= n;
    }
}

/* ---- blitter ------------------------------------------------------------------ */

static uint32_t reg_ptr(int off)
{
    return ((uint32_t)regs[off / 2] << 16 | regs[off / 2 + 1]) & ADDR_MASK & ~1u;
}

static void set_reg_ptr(int off, uint32_t v)
{
    regs[off / 2] = (uint16_t)(v >> 16);
    regs[off / 2 + 1] = (uint16_t)v;
}

static void blit(void)
{
    uint16_t con0 = regs[0x40 / 2], con1 = regs[0x42 / 2];
    uint16_t size = regs[0x58 / 2];
    int h = size >> 6, w = size & 63;
    if (!h) h = 1024;
    if (!w) w = 64;
    if (con1 & 1) {     /* line mode */
        static int warned;
        if (!warned++) fprintf(stderr, "warning: blitter line mode not emulated\n");
        return;
    }
    int useA = con0 & 0x800, useB = con0 & 0x400, useC = con0 & 0x200, useD = con0 & 0x100;
    int minterm = con0 & 0xFF, ash = con0 >> 12, bsh = con1 >> 12;
    int desc = con1 & 2;
    int fill = con1 & 0x18, fci = (con1 >> 2) & 1;
    uint32_t apt = reg_ptr(0x50), bpt = reg_ptr(0x4C), cpt = reg_ptr(0x48), dpt = reg_ptr(0x54);
    int16_t amod = (int16_t)regs[0x64 / 2], bmod = (int16_t)regs[0x62 / 2];
    int16_t cmod = (int16_t)regs[0x60 / 2], dmod = (int16_t)regs[0x66 / 2];
    uint16_t adat = regs[0x74 / 2], bdat = regs[0x72 / 2], cdat = regs[0x70 / 2];
    uint16_t fwm = regs[0x44 / 2], lwm = regs[0x46 / 2];
    int step = desc ? -2 : 2;
    if (hle_trace)
        fprintf(stderr, "  blit %dx%d con0 %04x con1 %04x A %06x B %06x C %06x D %06x mods %d %d %d %d fwm %04x lwm %04x\n",
                w, h, con0, con1, apt, bpt, cpt, dpt, amod, bmod, cmod, dmod, fwm, lwm);

    for (int y = 0; y < h; y++) {
        uint32_t prevA = 0, prevB = 0;
        int carry = fci;
        for (int x = 0; x < w; x++) {
            uint32_t a = useA ? rw(apt) : adat;
            if (useA) apt += step;
            if (x == 0) a &= fwm;
            if (x == w - 1) a &= lwm;
            uint32_t b = bdat;
            if (useB) { b = rw(bpt); bpt += step; }
            uint32_t c = cdat;
            if (useC) { c = rw(cpt); cpt += step; }
            uint32_t as, bs;
            if (!desc) {
                as = ((prevA << 16) | a) >> ash;
                bs = ((prevB << 16) | b) >> bsh;
            } else {
                as = ((a << 16) | prevA) >> (16 - ash);
                bs = ((b << 16) | prevB) >> (16 - bsh);
            }
            as &= 0xFFFF;
            bs &= 0xFFFF;
            prevA = a;
            prevB = b;
            uint32_t d = 0;
            for (int t = 0; t < 8; t++) {
                if (!(minterm & (1 << t))) continue;
                d |= ((t & 4) ? as : ~as) & ((t & 2) ? bs : ~bs) & ((t & 1) ? c : ~c);
            }
            d &= 0xFFFF;
            if (fill) {
                uint32_t out = 0;
                for (int bit = 0; bit < 16; bit++) {
                    int v = (d >> bit) & 1;
                    if (fill & 0x08) {          /* inclusive */
                        if (v) carry ^= 1;
                        out |= (uint32_t)(carry | v) << bit;
                    } else {                    /* exclusive */
                        if (v) carry ^= 1;
                        out |= (uint32_t)carry << bit;
                    }
                }
                d = out;
            }
            if (useD) { ww(dpt, d); dpt += step; }
        }
        if (useA) apt += desc ? -amod : amod;
        if (useB) bpt += desc ? -bmod : bmod;
        if (useC) cpt += desc ? -cmod : cmod;
        if (useD) dpt += desc ? -dmod : dmod;
    }
    set_reg_ptr(0x50, apt);
    set_reg_ptr(0x4C, bpt);
    set_reg_ptr(0x48, cpt);
    set_reg_ptr(0x54, dpt);
    raise_irq(6);
}

/* ---- input --------------------------------------------------------------------- */

static int joy_up, joy_down, joy_left, joy_right, joy_fire;   /* test scripts */
static int mouse_x, mouse_y, mouse_left, mouse_right;

/* Game controller (XInput on Windows, via SDL's GameController API) in
 * joystick port 2, read fresh every frame. The keyboard stays separate. */
static SDL_GameController *pad;
static int pad_up, pad_down, pad_left, pad_right, pad_fire;
/* Keyboard mode "joystick": cursor keys and space/Ctrl drive the joystick. */
static int kb_up, kb_down, kb_left, kb_right, kb_fire;
#define STICK_DEADZONE (settings.deadzone * 32767 / 100)
#define TRIGGER_FIRE   16000

static void pad_open_first(void)
{
    for (int i = 0; !pad && i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) fprintf(stderr, "controller: %s\n", SDL_GameControllerName(pad));
        }
}

void input_held_event(int rawkey, int down);

static void pad_poll(void)
{
    pad_up = pad_down = pad_left = pad_right = pad_fire = 0;
    if (!pad) return;
    SDL_GameController *c = pad;
    int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    pad_left = x < -STICK_DEADZONE || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    pad_right = x > STICK_DEADZONE || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad_up = y < -STICK_DEADZONE || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);
    pad_down = y > STICK_DEADZONE || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad_fire = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ||
               SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ||
               SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ||
               SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ||
               SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_FIRE ||
               SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_FIRE;
}

void input_key_event(int scancode, int down, int repeat);  /* input.device, gfx.c */
void input_held_event(int rawkey, int down);

static uint16_t joy1dat(void)
{
    uint16_t v = 0;
    int up = joy_up || pad_up || kb_up, down = joy_down || pad_down || kb_down;
    if (joy_right || pad_right || kb_right) v |= 0x0002;
    if (joy_left || pad_left || kb_left) v |= 0x0200;
    if (((v >> 1) & 1) ^ down) v |= 0x0001;
    if (((v >> 9) & 1) ^ up) v |= 0x0100;
    return v;
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            exit(0);
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int down = e.type == SDL_KEYDOWN;
            /* Held keys auto-repeat like input.device does (the game moves
             * on repeated key events); repeats carry IEQUALIFIER_REPEAT. */
            /* The keyboard is only the Amiga keyboard (the game's manual
             * uses cursor keys and space); the joystick is a separate device. */
            if (e.key.keysym.scancode == SDL_SCANCODE_F11) {
                if (down) video_toggle_fullscreen();
                break;
            }
            if (e.key.keysym.scancode == SDL_SCANCODE_F12) {
                if (down) video_save_bmp("screenshot.bmp");
                break;
            }
            /* Frame rate: keypad +/- or PgUp/PgDn, steps of 5 (1 with Shift) */
            {
                SDL_Scancode sc = e.key.keysym.scancode;
                int step = (e.key.keysym.mod & KMOD_SHIFT) ? 1 : 5;
                int music = (e.key.keysym.mod & KMOD_CTRL) != 0;   /* Ctrl: music rate */
                if (sc == SDL_SCANCODE_KP_PLUS || sc == SDL_SCANCODE_PAGEUP) {
                    if (down) { if (music) chips_set_music_hz(music_hz + step); else chips_set_fps(fps + step); }
                    break;
                }
                if (sc == SDL_SCANCODE_KP_MINUS || sc == SDL_SCANCODE_PAGEDOWN) {
                    if (down) { if (music) chips_set_music_hz(music_hz - step); else chips_set_fps(fps - step); }
                    break;
                }
            }
            if (settings.keyboard_mode == 1) {
                int handled = 1;
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP: kb_up = down; break;
                case SDL_SCANCODE_DOWN: kb_down = down; break;
                case SDL_SCANCODE_LEFT: kb_left = down; break;
                case SDL_SCANCODE_RIGHT: kb_right = down; break;
                case SDL_SCANCODE_SPACE: case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL:
                    kb_fire = down; break;
                default: handled = 0; break;
                }
                if (handled) break;   /* other keys still reach the game as keys */
            }
            input_key_event(e.key.keysym.scancode, down, e.key.repeat != 0);
            break;
        }
        case SDL_MOUSEMOTION:
            mouse_x += e.motion.xrel;
            mouse_y += e.motion.yrel;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) mouse_left = e.type == SDL_MOUSEBUTTONDOWN;
            if (e.button.button == SDL_BUTTON_RIGHT) mouse_right = e.type == SDL_MOUSEBUTTONDOWN;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            pad_open_first();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == e.cdevice.which) {
                SDL_GameControllerClose(pad);
                pad = NULL;
                pad_open_first();          /* another one may still be attached */
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            /* The controller is purely the Amiga joystick: directions and
             * fire are polled in pad_poll and nothing is sent as keys.
             * The shoulder buttons only adjust the emulator's frame rate. */
            int down = e.type == SDL_CONTROLLERBUTTONDOWN;
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (down) chips_set_fps(fps - 5); break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) chips_set_fps(fps + 5); break;
            default: break;
            }
            break;
        }
        }
    }
}

/* ---- timing ------------------------------------------------------------------------ */

static Uint64 perf_freq, frame_start;

static int headless_polls;          /* polls in the current headless frame */
uint64_t input_frame_cost_us(void);
void input_service(void);

/* Emulated time: frames of 20 ms plus the time inside the frame, which is
 * wall time (or poll count when headless) plus the estimated blitter work
 * the graphics library did this frame. Capped to stay inside the frame. */
uint64_t emu_time_us(void)
{
    uint64_t inside;
    if (headless)
        inside = (uint64_t)headless_polls * 400;
    else
        inside = (uint64_t)((double)(SDL_GetPerformanceCounter() - frame_start) / perf_freq
                            * 1e6 * 20.0 / frame_ms);   /* scaled to 20 ms frames */
    inside += input_frame_cost_us();
    if (inside > 19999) inside = 19999;
    return (uint64_t)frame_counter * 20000 + inside;
}

static double frame_fraction(void)
{
    if (headless) return 0.5;
    double f = (double)(SDL_GetPerformanceCounter() - frame_start) / perf_freq * 1000.0 / FRAME_MS;
    return f < 0 ? 0 : (f > 0.999 ? 0.999 : f);
}

/* Scripted joystick input for testing: DT_SCRIPT="300:fire+,305:fire-,400:right+"
 * (frame:button with + for press, - for release; also shot to save a BMP). */
static void run_script(void)
{
    const char *s = getenv("DT_SCRIPT");
    if (!s) return;
    while (*s) {
        unsigned f = (unsigned)strtoul(s, (char **)&s, 10);
        if (*s++ != ':') return;
        char name[16] = {0};
        int n = 0;
        while (*s && *s != '+' && *s != '-' && *s != ',' && n < 15) name[n++] = *s++;
        int down = *s == '+';
        if (*s == '+' || *s == '-') s++;
        if (*s == ',') s++;
        if (f != frame_counter) continue;
        if (!strcmp(name, "up")) joy_up = down;
        else if (!strcmp(name, "down")) joy_down = down;
        else if (!strcmp(name, "left")) joy_left = down;
        else if (!strcmp(name, "right")) joy_right = down;
        else if (!strcmp(name, "fire")) joy_fire = down;
        else if (name[0] == 'k' && name[1])          /* k<raw hex>: Amiga key, e.g. k4e+ */
            input_held_event((int)strtol(name + 1, NULL, 16), down);
        else if (!strcmp(name, "lmb")) mouse_left = down;
        else if (!strcmp(name, "rmb")) mouse_right = down;
        /* mouse moves: mx40 = right 40 counts, mxn40 = left 40 */
        else if (!strncmp(name, "mx", 2))
            mouse_x += name[2] == 'n' ? -atoi(name + 3) : atoi(name + 2);
        else if (!strncmp(name, "my", 2))
            mouse_y += name[2] == 'n' ? -atoi(name + 3) : atoi(name + 2);
        else if (!strcmp(name, "dump")) {   /* whole 16 MB address space */
            char path[32];
            snprintf(path, sizeof path, "mem_%u.bin", f);
            FILE *out = fopen(path, "wb");
            if (out) { fwrite(mem, 1, MEM_SIZE, out); fclose(out); }
        }
        else if (!strcmp(name, "shot")) {
            char path[32];
            snprintf(path, sizeof path, "shot_%u.bmp", f);
            video_save_bmp(path);
        }
    }
}

static void vblank(void)
{
    frame_counter++;
    run_script();
    video_frame();
    if (!headless) { pump_events(); pad_poll(); }
    for (int i = 0; i < vertb_count; i++) run_interrupt(vertb_servers[i], 1u << 5);
    raise_irq(5);
    if (getenv("DT_SHOT_FRAME") && frame_counter == (uint32_t)atoi(getenv("DT_SHOT_FRAME"))) {
        video_save_bmp("shot.bmp");
        fprintf(stderr, "saved shot.bmp at frame %u\n", frame_counter);
    }
    if (getenv("DT_MAX_FRAMES") && frame_counter >= (uint32_t)atoi(getenv("DT_MAX_FRAMES"))) {
        fprintf(stderr, "stopping at frame %u, last loop at %06x\n", frame_counter, poll_pc);
        video_save_bmp("shot.bmp");
        exit(0);
    }
}

/* Called from backward branches in recompiled code and from waits. */
void hle_poll(void)
{
    poll_count = 2000;
    if (logfn_count) fprintf(stderr, "[poll] frame %u at %06x\n", frame_counter, poll_pc);
    if (in_interrupt) return;
    chips_service();
    input_service();
    if (headless) {
        if (++headless_polls >= 50) {
            headless_polls = 0;
            frame_start++;
            audio_generate(AUDIO_RATE / 50);   /* advance Paula: block-end interrupts */
            vblank();
            if (vertb_count) music_tick();
        }
        return;
    }
    /* Music player at its own rate of real time. */
    {
        static Uint64 music_next;
        Uint64 t = SDL_GetPerformanceCounter(), period = perf_freq / (Uint64)music_hz;
        if (!music_next || (t > music_next && t - music_next > 10 * period)) music_next = t;
        while (t >= music_next) {
            music_next += period;
            if (vertb_count) music_tick();
        }
    }
    Uint64 now = SDL_GetPerformanceCounter();
    double ms = (double)(now - frame_start) / perf_freq * 1000.0;
    if (ms >= FRAME_MS) {
        frame_start = ms > 5 * FRAME_MS ? now : frame_start + (Uint64)(perf_freq * FRAME_MS / 1000.0);
        audio_debt += AUDIO_RATE * FRAME_MS / 1000.0;
        int frames = (int)audio_debt;
        audio_debt -= frames;
        /* Keep about 60 ms queued; skip generating if far ahead. */
        if (!audio_dev || SDL_GetQueuedAudioSize(audio_dev) < AUDIO_RATE * 4 / 8) audio_generate(frames);
        else audio_generate(frames / 2);
        vblank();
    }
}

/* WaitTOF and friends: block until the next vertical blank. */
void hle_wait_frame(void)
{
    uint32_t f = frame_counter;
    while (frame_counter == f) {
        hle_poll();
        if (!headless && frame_counter == f) SDL_Delay(1);
    }
}

void chips_init(void)
{
    headless = getenv("DT_HEADLESS") != NULL;
    perf_freq = SDL_GetPerformanceFrequency();
    frame_start = SDL_GetPerformanceCounter();
    if (!headless && settings.sound) {
        SDL_AudioSpec want = {0}, have;
        want.freq = AUDIO_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
    }
}

/* ---- register access ---------------------------------------------------------------- */

static uint16_t custom_read(uint32_t off)
{
    switch (off) {
    case 0x002: return dmacon & 0x07FF;                 /* DMACONR, blitter never busy */
    case 0x004: {                                       /* VPOSR */
        int line = (int)(frame_fraction() * 312);
        hle_poll();
        return (uint16_t)(0x2000 | (line >> 8));        /* PAL Agnus id */
    }
    case 0x006: {                                       /* VHPOSR */
        double f = frame_fraction() * 312;
        int line = (int)f, hpos = (int)((f - line) * 227);
        hle_poll();
        return (uint16_t)(((line & 0xFF) << 8) | hpos);
    }
    case 0x00A: return (uint16_t)(((mouse_y & 0xFF) << 8) | (mouse_x & 0xFF));  /* JOY0DAT */
    case 0x00C: return joy1dat();
    case 0x016: return (uint16_t)(0xFF00 & ~(mouse_right ? 0x0400 : 0));       /* POTGOR */
    case 0x01C: return intena;
    case 0x01E: return intreq;
    }
    return regs[(off & 0x1FF) / 2];
}

static void custom_write(uint32_t off, uint16_t v)
{
    regs[(off & 0x1FF) / 2] = v;
    switch (off) {
    case 0x096: {                                       /* DMACON */
        uint16_t old = dmacon;
        if (v & 0x8000) dmacon |= v & 0x7FFF;
        else dmacon &= ~v;
        for (int i = 0; i < 4; i++) {
            int was = (old & 0x200) && (old & (1 << i));
            int now = (dmacon & 0x200) && (dmacon & (1 << i));
            if (now && !was) channel_start(i);
            if (!now) ch[i].on = 0;
        }
        break;
    }
    case 0x09A:                                         /* INTENA */
        if (v & 0x8000) intena |= v & 0x7FFF;
        else intena &= ~v;
        break;
    case 0x09C:                                         /* INTREQ */
        if (v & 0x8000) intreq |= v & 0x7FFF;
        else intreq &= ~v;
        break;
    case 0x058:                                         /* BLTSIZE */
        blit();
        break;
    default:
        if (off >= 0xA0 && off < 0xE0) {
            int i = (off - 0xA0) / 16;
            if ((off & 15) == 6) ch[i].per = v;
            if ((off & 15) == 8) ch[i].vol = v & 0x7F;
        }
    }
}

uint32_t hw_read(uint32_t addr, int size)
{
    if (addr >= 0xDFF000 && addr < 0xDFF200) {
        uint32_t off = addr - 0xDFF000;
        if (size == 4) return ((uint32_t)custom_read(off) << 16) | custom_read(off + 2);
        uint16_t w = custom_read(off & ~1u);
        if (size == 1) return (off & 1) ? (w & 0xFF) : (w >> 8);
        return w;
    }
    if (addr >= 0xBF0000 && addr < 0xC00000) {
        /* CIAs decode only some address lines: CIA-A sits on odd bytes
         * (A12 = 0), CIA-B on even bytes (A13 = 0), register = A8-A11.
         * The game reads the fire button as a word from $bfe0fe. */
        if (size == 4)
            return (hw_read(addr, 2) << 16) | hw_read(addr + 2, 2);
        if (size == 2)
            return (hw_read(addr, 1) << 8) | hw_read(addr + 1, 1);
        if ((addr & 1) && !(addr & 0x1000) && ((addr >> 8) & 15) == 0) {   /* CIA-A PRA */
            uint32_t v = 0xFF;
            if (joy_fire || pad_fire || kb_fire) v &= ~0x80u;
            if (mouse_left) v &= ~0x40u;
            return v;
        }
        return 0xFF;
    }
    if (size == 1) return 0xFF;
    return 0;
}

void hw_write(uint32_t addr, uint32_t value, int size)
{
    if (addr >= 0xDFF000 && addr < 0xDFF200) {
        uint32_t off = addr - 0xDFF000;
        if (size == 4) {
            custom_write(off, (uint16_t)(value >> 16));
            custom_write(off + 2, (uint16_t)value);
        } else if (size == 2) {
            custom_write(off & ~1u, (uint16_t)value);
        } else {
            uint16_t w = regs[(off & 0x1FE) / 2];
            w = (off & 1) ? (uint16_t)((w & 0xFF00) | value) : (uint16_t)((w & 0xFF) | (value << 8));
            custom_write(off & ~1u, w);
        }
    }
    /* CIA writes (LED filter, timers) are ignored. */
}
