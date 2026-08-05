/* emu.c - emulator session */

#include "emu.h"
#include <stdarg.h>

int  opt_trace = 0;
int  opt_profile = 0;
int  opt_log_periph = 0;
u64  opt_trace_start = 0;
u32  opt_core_hz = 88000000;       /* PLL1P: HSI 64 MHz /4 x11 /2 */
FILE *gw_logfile;

static EmuLogSink log_sink;
static void *log_sink_ctx;

void emu_set_logfile(FILE *f) { gw_logfile = f; }

void emu_set_log_sink(EmuLogSink fn, void *ctx)
{
    log_sink = fn;
    log_sink_ctx = ctx;
}

void gwlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    if (gw_logfile) {
        fputs(buf, gw_logfile);
        fflush(gw_logfile);
    } else if (!log_sink) {
        fputs(buf, stderr);
    }
    if (log_sink) log_sink(log_sink_ctx, buf);
}

void gwfatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (gw_logfile) {
        fprintf(gw_logfile, "FATAL: ");
        va_start(ap, fmt);
        vfprintf(gw_logfile, fmt, ap);
        va_end(ap);
        fprintf(gw_logfile, "\n");
        fflush(gw_logfile);
    }
    cpu.halted = true;
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
/* ------------------------------------------------------------------ */

/* Port 0=A 1=B 2=C 3=D, active low. Pins read out of the firmware's own scan
   routine: 0x0800a688 on Mario, 0x08013506 on Zelda. The two agree on every
   button they share; Zelda scans two more.
 *
 * TIME is PA2. The scan can take it from PC5 on another board revision, but
 * this one reads PA2 and never looks at PC5 at all: PC5 is charger detect
 * here, which is why driving both flipped the charging state. Confirmed by
 * logging the firmware's scan helper - bit 6 reads mask 0x2 on GPIOC, bit 7
 * reads mask 0x4 on GPIOA. */
typedef struct { const char *name; int port, pin; } ButtonWiring;

static const ButtonWiring wiring[BTN_COUNT] = {
    [BTN_LEFT]  = { "Left",  3, 11 },
    [BTN_RIGHT] = { "Right", 3, 15 },
    [BTN_UP]    = { "Up",    3,  0 },
    [BTN_DOWN]  = { "Down",  3, 14 },
    [BTN_A]     = { "A",     3,  9 },
    [BTN_B]     = { "B",     3,  5 },
    [BTN_TIME]  = { "Time",  0,  2 },
    [BTN_GAME]  = { "Game",  2,  1 },
    [BTN_PAUSE] = { "Pause",  2, 13 },
    [BTN_SELECT]= { "Select", 2, 12 },
    [BTN_START] = { "Start",  2, 11 },
    [BTN_POWER] = { "Power",  0,  0 },
};

/* Host input and emulator-driven presses are tracked apart so the frontend
   rewriting button state each frame cannot release an autostart tap. */
static bool button_down[BTN_COUNT];
static bool power_held;
static u64  power_held_until;

/* Emulated time charged so far. Not the same as retired instructions: in
   Standby the CPU is asleep and retires almost none, so anything scheduled
   against cpu.cycles would never come due - including the POWER press that
   is meant to wake it. */
static u64 emu_time;

const char *emu_button_name(EmuButton b)
{
    return (b >= 0 && b < BTN_COUNT) ? wiring[b].name : "?";
}

static void drive_button(EmuButton b)
{
    bool down = button_down[b] || (b == BTN_POWER && power_held);
    gpio_set_button(wiring[b].port, wiring[b].pin, down);
}

void emu_set_button(EmuButton b, bool down)
{
    if (b < 0 || b >= BTN_COUNT) return;
    button_down[b] = down;
    drive_button(b);
}

bool emu_button_state(EmuButton b)
{
    if (b < 0 || b >= BTN_COUNT) return false;
    return button_down[b] || (b == BTN_POWER && power_held);
}

void emu_release_all_buttons(void)
{
    power_held = false;
    for (int i = 0; i < BTN_COUNT; i++) emu_set_button((EmuButton)i, false);
}

void emu_tap_power(void)
{
    power_held = true;
    power_held_until = emu_time + opt_core_hz / 3;
    drive_button(BTN_POWER);
}

static void power_tick(void)
{
    if (power_held && emu_time >= power_held_until) {
        power_held = false;
        drive_button(BTN_POWER);
    }
}

/* ------------------------------------------------------------------ */
/* session                                                             */
/* ------------------------------------------------------------------ */

static bool session_open;

/* Must stay well below the shortest periodic event (SysTick, ~1 ms) or
   interrupts collapse into one another. */
#define SLICE 4096

static bool autostart = true;
static bool autostart_done;
static u64  autostart_delay = 60000000;
static u64  autostart_deadline;

/* The clock is a running total a reset does not clear, so the deadline is
   recomputed against the reset rather than stored absolute. */
static void arm_autostart(void)
{
    autostart_done = false;
    autostart_deadline = emu_time + autostart_delay;
}

void emu_set_autostart(bool on, u64 delay_cycles)
{
    autostart = on;
    if (delay_cycles) autostart_delay = delay_cycles;
    arm_autostart();
}

bool emu_is_open(void) { return session_open; }
bool emu_halted(void)  { return cpu.halted; }
u64  emu_cycles(void)  { return cpu.cycles; }

bool emu_open(const EmuFirmware *fw)
{
    if (!fw || !fw->int_data || !fw->ext_data) return false;
    if (session_open) emu_close();

    if (fw->core_hz) opt_core_hz = fw->core_hz;
    opt_rtc_host = fw->rtc_host;

    bus_init_mem(fw->int_data, fw->int_len, fw->ext_data, fw->ext_len);
    if (opt_profile) cpu_profile_init();
    periph_init();
    cpu_reset();

    for (int i = 0; i < BTN_COUNT; i++) button_down[i] = false;
    power_held = false;
    emu_time = 0;
    arm_autostart();
    session_open = true;
    return !cpu.halted;
}

void emu_close(void)
{
    if (!session_open) return;
    bus_free();
    session_open = false;
}

void emu_reset(void)
{
    if (!session_open) return;
    /* Cold, not warm: a warm reset keeps the PWR flags of the running system,
       so the firmware comes back already on and the autostart tap that follows
       reads as a request to switch off. */
    system_cold_reset();
    emu_release_all_buttons();
    arm_autostart();
}

u32 emu_step(u32 cycles)
{
    if (!session_open || cpu.halted) return 0;

    u32 done = 0;
    while (done < cycles && !cpu.halted) {
        u32 step = cycles - done < SLICE ? cycles - done : SLICE;
        cpu_run(step);
        periph_tick(step);      /* full slice: WFI must still advance time */
        done += step;
        emu_time += step;

        power_tick();
        if (autostart && !autostart_done && emu_time >= autostart_deadline) {
            gwlog("[emu] autostart: pressing POWER\n");
            emu_tap_power();
            autostart_done = true;
        }
    }
    return done;
}

bool emu_power_off_and_wait(void)
{
    if (!session_open || cpu.halted) return false;

    u64 limit = (u64)opt_core_hz * 8;
    u64 spent = 0;

    gwlog("[emu] pressing POWER to shut down\n");
    emu_tap_power();
    while (spent < limit && !cpu.halted) {
        cpu_run(SLICE);
        periph_tick(SLICE);
        emu_time += SLICE;
        power_tick();
        spent += SLICE;
        if (!power_held && pwr_in_standby()) {
            gwlog("[emu] reached Standby after %.1f emulated seconds\n",
                  (double)spent / (double)opt_core_hz);
            return true;
        }
    }
    gwlog("[emu] shutdown did not reach Standby in time\n");
    return false;
}

bool emu_load_nvram(const char *path) { return session_open && state_load(path); }
void emu_save_nvram(const char *path) { if (session_open) state_save(path); }

/* ------------------------------------------------------------------ */
/* frame and audio                                                     */
/* ------------------------------------------------------------------ */

const u32 *emu_framebuffer(void) { return ltdc_framebuffer; }
int  emu_fb_width(void)          { return ltdc_width; }
int  emu_fb_height(void)         { return ltdc_height; }
int  emu_frame_counter(void)     { return ltdc_frame_counter; }
u32  emu_audio_rate(void)        { return opt_audio_hz; }

u32 emu_audio_available(void)
{
    return (audio_wr + AUDIO_RING - audio_rd) % AUDIO_RING;
}

void emu_audio_render(s16 *out, u32 frames, u32 out_hz)
{
    static double pos;
    static s16 last;

    if (!out_hz) out_hz = 48000;
    double step = (double)opt_audio_hz / (double)out_hz;
    u32 avail = emu_audio_available();
    if (avail > AUDIO_RING / 2)  step *= 1.01;
    else if (avail < 1024)       step *= 0.99;

    for (u32 i = 0; i < frames; i++) {
        if (emu_audio_available() < 2) { out[i] = last; continue; }
        u32 i0 = audio_rd, i1 = (audio_rd + 1) % AUDIO_RING;
        s32 a = audio_ring[i0], b = audio_ring[i1];
        out[i] = last = (s16)(a + (s32)((b - a) * pos));
        pos += step;
        while (pos >= 1.0 && emu_audio_available() >= 2) {
            audio_rd = (audio_rd + 1) % AUDIO_RING;
            pos -= 1.0;
        }
        if (pos >= 1.0) pos = 0.0;
    }
}
