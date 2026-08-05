/* options.c - command line parsing shared by both frontends */

#include "options.h"

void app_options_defaults(AppOptions *o)
{
    memset(o, 0, sizeof *o);
    o->scale    = 3;
    o->power_at = 60;          /* million instructions after reset */
    o->core_hz  = 88000000;
    o->watch_lo = 1;           /* lo > hi disables the watchpoint */
    o->watch_hi = 0;
}

void app_options_usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [firmware.bin] [options]\n"
        "\n"
        "Runs a graphical emulator by default. With no firmware given it opens\n"
        "empty and the last-used images can be reloaded from the File menu.\n"
        "\n"
        "firmware\n"
        "  --int PATH        internal flash dump (128 KiB)\n"
        "  --ext PATH        external flash dump (1 MiB)\n"
        "  --save PATH       save file for external flash + backup domain\n"
        "  --no-save         do not load or write a save file\n"
        "  --rtc host        start the clock from host time instead of 12:00\n"
        "  --hz N            emulated core clock (default 88000000)\n"
        "\n"
        "window\n"
        "  --scale N         window scale factor (default 3)\n"
        "  --fullscreen      start fullscreen\n"
        "\n"
        "headless and debugging\n"
        "  --headless N      run N million instructions with no window, then exit\n"
        "  --power-at N      auto-press POWER after N million instructions (-1 = never)\n"
        "  --hold NAME AT D  hold a button at AT for D million instructions\n"
        "  --press NAME AT   tap a button at AT million instructions\n"
        "  --no-power-off    exit immediately instead of holding POWER to shut down\n"
        "  --log FILE        write the emulator log here (default gw.log)\n"
        "  --log-periph      log accesses to unmapped peripheral registers\n"
        "  --trace           keep an instruction history and dump it on faults\n"
        "  --profile         collect an exact PC histogram\n"
        "  --logcall ADDR    log calls to an address with their arguments\n"
        "  --watch LO HI     log writes into an address range\n"
        "  --battery RAW     raw ADC value for the battery level\n"
        "  --dump-fb FILE    write the final framebuffer as a PPM\n"
        "  --dump-mem A N    log a memory region at exit\n"
        "  --dump-raw A N F  dump emulated memory to a file\n"
        "\n"
        "keys: arrows = D-pad, X = A, Z = B, 1 = TIME, 2 = GAME, Enter = PAUSE, P = POWER\n",
        prog);
}

#define NEED(n) (i + (n) < argc)

bool app_options_parse(AppOptions *o, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--int")   && NEED(1)) o->int_path  = argv[++i];
        else if (!strcmp(a, "--ext")   && NEED(1)) o->ext_path  = argv[++i];
        else if (!strcmp(a, "--save")  && NEED(1)) o->save_path = argv[++i];
        else if (!strcmp(a, "--log")   && NEED(1)) o->log_path  = argv[++i];
        else if (!strcmp(a, "--scale") && NEED(1)) o->scale     = atoi(argv[++i]);
        else if (!strcmp(a, "--hz")    && NEED(1)) o->core_hz   = (u32)atol(argv[++i]);
        else if (!strcmp(a, "--fullscreen"))       o->fullscreen = true;
        else if (!strcmp(a, "--no-save"))          o->no_save = true;
        else if (!strcmp(a, "--no-power-off"))     o->no_power_off = true;
        else if (!strcmp(a, "--log-periph"))       o->log_periph = true;
        else if (!strcmp(a, "--trace"))            o->trace = true;
        else if (!strcmp(a, "--profile"))          o->profile = true;
        else if (!strcmp(a, "--rtc") && NEED(1))   o->rtc_host = !strcmp(argv[++i], "host");
        else if (!strcmp(a, "--headless") && NEED(1)) o->headless = atol(argv[++i]);
        else if (!strcmp(a, "--power-at") && NEED(1)) o->power_at = atol(argv[++i]);
        else if (!strcmp(a, "--dump-fb")  && NEED(1)) o->dump_fb  = argv[++i];
        else if (!strcmp(a, "--battery")  && NEED(1)) o->battery_raw = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--logcall")  && NEED(1)) cpu_add_logcall((u32)strtoul(argv[++i], NULL, 0));
        else if (!strcmp(a, "--watch") && NEED(2)) {
            o->watch_lo = (u32)strtoul(argv[++i], NULL, 0);
            o->watch_hi = (u32)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(a, "--dump-mem") && NEED(2)) {
            o->dump_addr = (u32)strtoul(argv[++i], NULL, 0);
            o->dump_len  = (u32)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(a, "--dump-raw") && NEED(3)) {
            o->raw_addr = (u32)strtoul(argv[++i], NULL, 0);
            o->raw_len  = (u32)strtoul(argv[++i], NULL, 0);
            o->raw_file = argv[++i];
        }
        else if (!strcmp(a, "--hold") && NEED(3)) {
            if (o->npress < MAX_SCRIPTED_PRESSES) {
                o->presses[o->npress].name = argv[++i];
                o->presses[o->npress].at   = atol(argv[++i]);
                o->presses[o->npress].dur  = atol(argv[++i]);
                o->npress++;
            } else i += 3;
        }
        else if (!strcmp(a, "--press") && NEED(2)) {
            if (o->npress < MAX_SCRIPTED_PRESSES) {
                o->presses[o->npress].name = argv[++i];
                o->presses[o->npress].at   = atol(argv[++i]);
                o->presses[o->npress].dur  = 20;
                o->npress++;
            } else i += 2;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            app_options_usage(argv[0]);
            o->help = true;
            return false;
        }
        /* A bare path is the internal flash, so that "gwemu firmware.bin"
           and drag-and-drop onto the binary both work. */
        else if (a[0] != '-' && !o->int_path) o->int_path = a;
        else {
            fprintf(stderr, "unknown option: %s\n\n", a);
            app_options_usage(argv[0]);
            return false;
        }
    }
    return true;
}

void app_options_apply(const AppOptions *o)
{
    opt_trace       = o->trace;
    opt_profile     = o->profile;
    opt_log_periph  = o->log_periph;
    opt_watch_lo    = o->watch_lo;
    opt_watch_hi    = o->watch_hi;
    if (o->core_hz)     opt_core_hz     = o->core_hz;
    if (o->battery_raw) opt_battery_raw = o->battery_raw;
}
