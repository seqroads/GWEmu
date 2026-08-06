/* web_main.c - the WebAssembly face of the emulator core.
 *
 * There is no window, no filesystem and no SDL here. The browser frontend owns
 * the frame loop, the audio device and storage; this file is the flat C API it
 * drives, and everything below it is the same portable core the desktop build
 * uses.
 *
 * Buffers are handed over by pointer into the wasm heap: JS allocates with
 * gw_alloc, copies bytes in, and calls. Nothing here keeps a JS reference.
 */

#include <emscripten.h>
#include "emu.h"

#define API EMSCRIPTEN_KEEPALIVE

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

/* The core logs through gwlog; without a sink it writes to stderr, which in a
   worker is console noise nobody sees. Route it to a hook the frontend sets. */
EM_JS(void, js_log, (const char *msg), {
    var s = UTF8ToString(msg);
    if (globalThis.gwOnLog) globalThis.gwOnLog(s);
    else console.log(s);
});

static void log_sink(void *ctx, const char *line)
{
    (void)ctx;
    js_log(line);
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

API void *gw_alloc(u32 n) { return malloc(n ? n : 1); }
API void  gw_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* session                                                             */
/* ------------------------------------------------------------------ */

/* FNV-1a over the internal flash, the same hash the desktop build keys its
   save files with, so the two agree on what "this firmware" means. */
static u64  firmware_hash;
static char firmware_id[17];

static void set_firmware_id(const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    firmware_hash = h;
    for (int i = 0; i < 16; i++)
        firmware_id[i] = "0123456789abcdef"[(h >> (60 - i * 4)) & 0xf];
    firmware_id[16] = 0;
}

/* 128 KiB is the internal flash; 1, 2 or 4 MiB is the external one. Returns
   'i', 'e' or 0, so the page can sort a pair of dropped files by size alone. */
API int gw_classify(u32 len)
{
    if (len == FLASH_SIZE) return 'i';
    for (u32 sz = 1024u * 1024; sz <= EXTFLASH_MAX; sz *= 2)
        if (len == sz) return 'e';
    return 0;
}

/* The clock always follows the host here. A browser tab has no business
   keeping its own battery-backed time across sessions, and the alternative
   needed a setting the user had to flip after every boot. */
API int gw_open(const void *intbuf, u32 intlen, const void *extbuf, u32 extlen,
                u32 core_hz)
{
    emu_set_log_sink(log_sink, NULL);

    EmuFirmware fw;
    memset(&fw, 0, sizeof fw);
    fw.int_data = intbuf; fw.int_len = intlen;
    fw.ext_data = extbuf; fw.ext_len = extlen;
    fw.core_hz  = core_hz;
    fw.rtc_host = true;

    if (!emu_open(&fw)) return 0;

    set_firmware_id(intbuf, intlen);
    emu_set_autostart(true, 0);
    rtc_sync_host();
    return 1;
}

API void gw_close(void) { emu_close(); }
API int  gw_is_open(void) { return emu_is_open() ? 1 : 0; }
API int  gw_halted(void)  { return emu_halted() ? 1 : 0; }

API void gw_reset(void)
{
    emu_reset();
    emu_set_autostart(true, 0);
}

API const char *gw_firmware_id(void) { return firmware_id; }
API u32  gw_core_hz(void) { return opt_core_hz; }

/* ------------------------------------------------------------------ */
/* running                                                             */
/* ------------------------------------------------------------------ */

API u32 gw_run(u32 cycles) { return emu_step(cycles); }
API int gw_frame_counter(void) { return emu_frame_counter(); }
API int gw_fb_width(void)  { return emu_fb_width(); }
API int gw_fb_height(void) { return emu_fb_height(); }
API const u32 *gw_framebuffer(void) { return emu_framebuffer(); }

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

/* One call per frame with a bitmask beats twelve calls; bit n is EmuButton n.
   POWER is left out: it is driven by gw_power_tap and by autostart, and a
   frontend rewriting state every frame would cut those short. */
API void gw_set_buttons(u32 mask)
{
    for (int i = 0; i < BTN_POWER; i++)
        emu_set_button((EmuButton)i, (mask >> i) & 1u);
}

API void gw_power_tap(void) { emu_tap_power(); }
API void gw_release_all(void) { emu_release_all_buttons(); }

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

API void gw_audio_render(s16 *out, u32 frames, u32 rate)
{
    emu_audio_render(out, frames, rate);
}

API u32 gw_audio_available(void) { return emu_audio_available(); }

/* ------------------------------------------------------------------ */
/* storage                                                             */
/* ------------------------------------------------------------------ */

/* Save data: external flash plus the backup domain, the bytes a real unit
   keeps across a power cycle. Not a save state - this is the device's own
   memory, and losing it on a page reload would be losing the game's saves. */
API u32 gw_nvram_size(void) { return state_blob_size(); }
API u32 gw_nvram_save(void *dst, u32 cap) { return state_save_mem(dst, cap); }
API int gw_nvram_load(const void *src, u32 len) { return state_load_mem(src, len) ? 1 : 0; }
