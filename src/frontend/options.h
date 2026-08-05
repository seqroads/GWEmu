/* options.h - command line options shared by the GUI and headless frontends */
#ifndef GW_OPTIONS_H
#define GW_OPTIONS_H

#include "../core/emu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SCRIPTED_PRESSES 8

typedef struct {
    /* firmware and storage */
    const char *int_path;       /* NULL: fall back to the last-used pair */
    const char *ext_path;
    const char *save_path;      /* NULL: derived from the firmware hash */
    const char *log_path;
    bool no_save;
    bool no_power_off;
    bool rtc_host;
    u32  core_hz;

    /* window */
    int  scale;
    bool fullscreen;

    /* headless and instrumentation */
    long headless;              /* million instructions; 0 runs the GUI */
    long power_at;              /* million instructions, -1 disables autostart */
    const char *dump_fb;
    bool trace, profile, log_periph;
    u32  watch_lo, watch_hi;
    u32  dump_addr, dump_len;
    u32  raw_addr, raw_len;
    const char *raw_file;
    u32  battery_raw;

    struct { const char *name; long at, dur; int state; } presses[MAX_SCRIPTED_PRESSES];
    int npress;

    bool help;                  /* usage was asked for; not an error */
} AppOptions;

void app_options_defaults(AppOptions *o);
/* Returns false and prints usage on a bad argument. */
bool app_options_parse(AppOptions *o, int argc, char **argv);
void app_options_usage(const char *prog);

/* Applies the instrumentation options to the core's globals. */
void app_options_apply(const AppOptions *o);

/* Headless / scripted runner. Returns a process exit code. */
int cli_run(const AppOptions *o);

#ifdef __cplusplus
}
#endif

#endif /* GW_OPTIONS_H */
