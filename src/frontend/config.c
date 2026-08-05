/* config.c - reading and writing the settings file */

#include "config.h"
#include "paths.h"
#include <SDL.h>

Config config;

/* Arrows for the D-pad, X and Z for A and B, and the console buttons on 1, 2
   and 3 in the order they are labelled. POWER is deliberately unbound: it is a
   menu item, not something to hit by accident. */
static const struct { EmuButton b; int key; } default_keys[] = {
    { BTN_LEFT,   SDLK_LEFT  },
    { BTN_RIGHT,  SDLK_RIGHT },
    { BTN_UP,     SDLK_UP    },
    { BTN_DOWN,   SDLK_DOWN  },
    { BTN_A,      SDLK_x     },
    { BTN_B,      SDLK_z     },
    { BTN_GAME,   SDLK_1     },
    { BTN_TIME,   SDLK_2     },
    { BTN_PAUSE,  SDLK_3     },
    { BTN_SELECT, SDLK_4     },
    { BTN_START,  SDLK_5     },
};

void config_defaults(void)
{
    memset(&config, 0, sizeof config);
    config.scale         = 2;
    config.integer_scale = true;
    config.audio_enabled = true;
    config.volume        = 100;
    config.speed_percent = 100;

    for (unsigned i = 0; i < SDL_arraysize(default_keys); i++) {
        config.bind[default_keys[i].b].type = BIND_KEY;
        config.bind[default_keys[i].b].code = default_keys[i].key;
    }
}

static const char *config_path(void)
{
    static char p[CFG_PATH_MAX];
    if (!p[0]) user_path(p, sizeof p, "gwemu.cfg");
    return p;
}

static const char *button_key(EmuButton b)
{
    static char k[32];
    SDL_strlcpy(k, emu_button_name(b), sizeof k);
    for (char *c = k; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    return k;
}

void config_save(void)
{
    FILE *f = fopen(config_path(), "w");
    if (!f) { gwlog("[config] cannot write %s\n", config_path()); return; }

    fprintf(f, "# gwemu settings\n\n");
    fprintf(f, "scale = %d\n",         config.scale);
    fprintf(f, "integer_scale = %d\n", config.integer_scale);
    fprintf(f, "linear_filter = %d\n", config.linear_filter);
    fprintf(f, "fullscreen = %d\n",    config.fullscreen);
    fprintf(f, "audio = %d\n",         config.audio_enabled);
    fprintf(f, "volume = %d\n",        config.volume);
    fprintf(f, "speed = %d\n",         config.speed_percent);
    fprintf(f, "rtc_host = %d\n",      config.rtc_host);
    fprintf(f, "last_dir = %s\n",      config.last_dir);

    fprintf(f, "\n# input: type 1 = key, 2 = pad button, 3 = axis\n");
    for (int i = 0; i < BTN_COUNT; i++)
        fprintf(f, "bind_%s = %d %d\n", button_key((EmuButton)i),
                config.bind[i].type, config.bind[i].code);

    fprintf(f, "\n");
    for (int i = 0; i < config.recent_count; i++)
        fprintf(f, "recent = %s|%s|%s\n", config.recent[i].int_path,
                config.recent[i].ext_path, config.recent[i].label);

    fclose(f);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

void config_load(void)
{
    config_defaults();

    FILE *f = fopen(config_path(), "r");
    if (!f) return;

    char line[CFG_PATH_MAX * 3];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        int iv = atoi(val);

        if      (!strcmp(key, "scale"))         config.scale = iv < 1 ? 1 : (iv > 8 ? 8 : iv);
        else if (!strcmp(key, "integer_scale")) config.integer_scale = iv != 0;
        else if (!strcmp(key, "linear_filter")) config.linear_filter = iv != 0;
        else if (!strcmp(key, "fullscreen"))    config.fullscreen = iv != 0;
        else if (!strcmp(key, "audio"))         config.audio_enabled = iv != 0;
        else if (!strcmp(key, "volume"))        config.volume = iv < 0 ? 0 : (iv > 200 ? 200 : iv);
        else if (!strcmp(key, "speed"))         config.speed_percent = iv < 5 ? 5 : (iv > 800 ? 800 : iv);
        else if (!strcmp(key, "rtc_host"))      config.rtc_host = iv != 0;
        else if (!strcmp(key, "last_dir"))      SDL_strlcpy(config.last_dir, val, CFG_PATH_MAX);
        else if (!strncmp(key, "bind_", 5)) {
            for (int i = 0; i < BTN_COUNT; i++)
                if (!strcmp(key + 5, button_key((EmuButton)i))) {
                    int t = 0, c = 0;
                    if (sscanf(val, "%d %d", &t, &c) == 2) {
                        config.bind[i].type = t;
                        config.bind[i].code = c;
                    }
                    break;
                }
        }
        else if (!strcmp(key, "recent") && config.recent_count < CFG_RECENT_MAX) {
            char *a = val, *b = strchr(a, '|');
            if (!b) continue;
            *b++ = 0;
            char *c = strchr(b, '|');
            if (!c) continue;
            *c++ = 0;
            RecentEntry *e = &config.recent[config.recent_count++];
            SDL_strlcpy(e->int_path, a, CFG_PATH_MAX);
            SDL_strlcpy(e->ext_path, b, CFG_PATH_MAX);
            SDL_strlcpy(e->label, c, sizeof e->label);
        }
    }
    fclose(f);
}

void config_push_recent(const char *int_path, const char *ext_path, const char *label)
{
    if (!int_path || !*int_path) return;

    /* Drop any existing entry for the same image, then push to the front. */
    int n = 0;
    for (int i = 0; i < config.recent_count; i++)
        if (strcmp(config.recent[i].int_path, int_path))
            config.recent[n++] = config.recent[i];
    config.recent_count = n < CFG_RECENT_MAX ? n : CFG_RECENT_MAX - 1;

    for (int i = config.recent_count; i > 0; i--)
        config.recent[i] = config.recent[i - 1];

    RecentEntry *e = &config.recent[0];
    SDL_strlcpy(e->int_path, int_path, CFG_PATH_MAX);
    SDL_strlcpy(e->ext_path, ext_path ? ext_path : "", CFG_PATH_MAX);
    SDL_strlcpy(e->label, label ? label : path_basename(int_path), sizeof e->label);
    if (config.recent_count < CFG_RECENT_MAX) config.recent_count++;
}

void config_clear_recent(void)
{
    config.recent_count = 0;
    memset(config.recent, 0, sizeof config.recent);
}

const char *config_bind_name(EmuButton b)
{
    static char buf[48];
    if (b < 0 || b >= BTN_COUNT) return "";
    const Binding *bind = &config.bind[b];

    switch (bind->type) {
    case BIND_KEY: {
        const char *n = SDL_GetKeyName((SDL_Keycode)bind->code);
        return (n && *n) ? n : "(none)";
    }
    case BIND_PAD:
        switch (bind->code) {
        case SDL_CONTROLLER_BUTTON_A:             return "A";
        case SDL_CONTROLLER_BUTTON_B:             return "B";
        case SDL_CONTROLLER_BUTTON_X:             return "X";
        case SDL_CONTROLLER_BUTTON_Y:             return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:          return "Back";
        case SDL_CONTROLLER_BUTTON_GUIDE:         return "Guide";
        case SDL_CONTROLLER_BUTTON_START:         return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "Left Stick Click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "Right Stick Click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "Left Bumper";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Right Bumper";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "D-Pad Right";
        default: break;
        }
        SDL_snprintf(buf, sizeof buf, "Button %d", bind->code);
        return buf;
    case BIND_AXIS: {
        bool pos = AXIS_POSITIVE(bind->code);
        switch (AXIS_OF(bind->code)) {
        case SDL_CONTROLLER_AXIS_LEFTX:  return pos ? "Left Stick Right"  : "Left Stick Left";
        case SDL_CONTROLLER_AXIS_LEFTY:  return pos ? "Left Stick Down"   : "Left Stick Up";
        case SDL_CONTROLLER_AXIS_RIGHTX: return pos ? "Right Stick Right" : "Right Stick Left";
        case SDL_CONTROLLER_AXIS_RIGHTY: return pos ? "Right Stick Down"  : "Right Stick Up";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return "Left Trigger";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Right Trigger";
        default: break;
        }
        return "Axis";
    }
    default:
        return "(none)";
    }
}
