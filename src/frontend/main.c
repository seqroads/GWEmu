/* main.c - entry point: the window, or the headless runner */

#include "options.h"
#include "session.h"
#include "app.h"
#include <SDL.h>

#ifdef _WIN32
#include <windows.h>

/* A GUI subsystem binary does not flash a console when double-clicked, which
   leaves --headless and --help with nowhere to print. Adopt the console of
   whatever launched us, when there is one. */
static void attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    attach_parent_console();
#endif

    AppOptions opt;
    app_options_defaults(&opt);
    if (!app_options_parse(&opt, argc, argv)) return opt.help ? 0 : 1;
    app_options_apply(&opt);

    if (opt.headless) {
        FILE *logf = fopen(opt.log_path ? opt.log_path : "gw.log", "w");
        if (!logf) { perror(opt.log_path ? opt.log_path : "gw.log"); return 1; }
        emu_set_logfile(logf);

        if (SDL_Init(0) != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        const char *err = session_load(opt.int_path, opt.ext_path, &opt);
        if (err) { fprintf(stderr, "%s\n", err); return 1; }

        int rc = cli_run(&opt);
        session_close(false);       /* cli_run already powered off */
        emu_set_logfile(NULL);
        fclose(logf);
        SDL_Quit();
        return rc;
    }

    /* The window has nowhere to show the log, so it only goes to a file when
       one was asked for. */
    FILE *logf = NULL;
    if (opt.log_path) {
        logf = fopen(opt.log_path, "w");
        if (!logf) { perror(opt.log_path); return 1; }
        emu_set_logfile(logf);
    }

    int rc = app_run(&opt);

    if (logf) { emu_set_logfile(NULL); fclose(logf); }
    return rc;
}
