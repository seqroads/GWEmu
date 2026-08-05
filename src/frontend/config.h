/* config.h - persistent settings, stored as a flat key = value file */
#ifndef GW_CONFIG_H
#define GW_CONFIG_H

#include "../core/emu.h"

#define CFG_RECENT_MAX 8
#define CFG_PATH_MAX   512

/* One binding per button: a key, a gamepad button, or a stick or trigger. */
enum { BIND_NONE = 0, BIND_KEY, BIND_PAD, BIND_AXIS };

/* Axis bindings pack the axis and its direction into one code. */
#define AXIS_CODE(axis, positive) ((axis) * 2 + ((positive) ? 1 : 0))
#define AXIS_OF(code)             ((code) / 2)
#define AXIS_POSITIVE(code)       ((code) % 2 != 0)

/* Past this counts as pressed; the higher value keeps stick drift from
   binding itself the moment the dialog starts listening. */
#define AXIS_THRESHOLD      12000
#define AXIS_BIND_THRESHOLD 20000

typedef struct { int type, code; } Binding;

typedef struct {
    char int_path[CFG_PATH_MAX];
    char ext_path[CFG_PATH_MAX];
    char label[64];
} RecentEntry;

typedef struct {
    int  scale;
    bool integer_scale;
    bool linear_filter;
    bool fullscreen;

    bool audio_enabled;
    int  volume;                /* percent */

    int  speed_percent;
    bool rtc_host;

    Binding bind[BTN_COUNT];

    RecentEntry recent[CFG_RECENT_MAX];
    int recent_count;

    char last_dir[CFG_PATH_MAX];  /* where the file browser opens */
} Config;

extern Config config;

void config_defaults(void);
void config_load(void);
void config_save(void);

void config_push_recent(const char *int_path, const char *ext_path, const char *label);
void config_clear_recent(void);

/* Human-readable name for a binding, e.g. "X" or "Left Stick Up". */
const char *config_bind_name(EmuButton b);

#endif /* GW_CONFIG_H */
