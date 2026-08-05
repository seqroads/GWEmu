/* emu.h - emulator session API, no host dependencies */
#ifndef GW_EMU_H
#define GW_EMU_H

#include "gw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SELECT and START are only scanned by titles that have them - Zelda does,
   Mario does not - so they are always present and simply do nothing on a
   firmware that never reads their pins. */
typedef enum {
    BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN,
    BTN_A, BTN_B, BTN_TIME, BTN_GAME, BTN_PAUSE,
    BTN_SELECT, BTN_START, BTN_POWER,
    BTN_COUNT
} EmuButton;

const char *emu_button_name(EmuButton b);
void        emu_set_button(EmuButton b, bool down);
bool        emu_button_state(EmuButton b);
void        emu_release_all_buttons(void);

/* Held by the emulator for a realistic time, so a frontend that rewrites
   button state every frame cannot cut it short. */
void emu_tap_power(void);

typedef struct {
    const void *int_data;   /* internal flash, 128 KiB */
    u32         int_len;
    const void *ext_data;   /* external flash, 1 MiB */
    u32         ext_len;
    u32         core_hz;    /* 0 = stock 88 MHz */
    bool        rtc_host;
} EmuFirmware;

bool emu_open(const EmuFirmware *fw);   /* closes any previous session */
void emu_close(void);
bool emu_is_open(void);

void emu_reset(void);                   /* cold reset; backup domain survives */
u32  emu_step(u32 cycles);
bool emu_halted(void);
u64  emu_cycles(void);

/* The unit boots into Standby, so POWER must be pressed for anything to
   appear. delay_cycles is measured from the reset, 0 keeps the current value. */
void emu_set_autostart(bool on, u64 delay_cycles);

/* Hold POWER until the firmware reaches Standby. True if it got there. */
bool emu_power_off_and_wait(void);

/* External flash writes plus the backup domain. */
bool emu_load_nvram(const char *path);
void emu_save_nvram(const char *path);

const u32 *emu_framebuffer(void);       /* ARGB8888 */
int  emu_fb_width(void);
int  emu_fb_height(void);
int  emu_frame_counter(void);

/* Resamples from the SAI rate to out_hz, nudging to keep the ring half full.
   Called from the host audio thread. */
void emu_audio_render(s16 *out, u32 frames, u32 out_hz);
u32  emu_audio_available(void);
u32  emu_audio_rate(void);

void emu_set_logfile(FILE *f);

typedef void (*EmuLogSink)(void *ctx, const char *line);
void emu_set_log_sink(EmuLogSink fn, void *ctx);

extern u32 opt_core_hz;

#ifdef __cplusplus
}
#endif

#endif /* GW_EMU_H */
