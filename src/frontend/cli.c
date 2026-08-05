/* cli.c - headless runner: a fixed number of instructions, scripted button
 * presses, and whatever the instrumentation collected. */

#include "options.h"
#include <time.h>

static EmuButton button_by_name(const char *name)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        const char *n = emu_button_name((EmuButton)i);
        size_t j = 0;
        for (; n[j] && name[j]; j++) {
            char a = n[j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (!n[j] && !name[j]) return (EmuButton)i;
    }
    fprintf(stderr, "unknown button: %s\n", name);
    return BTN_COUNT;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    int w = emu_fb_width(), h = emu_fb_height();
    const u32 *fb = emu_framebuffer();
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        u32 c = fb[i];
        fputc((int)((c >> 16) & 0xFF), f);
        fputc((int)((c >> 8) & 0xFF), f);
        fputc((int)(c & 0xFF), f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int cli_run(const AppOptions *o)
{
    struct { EmuButton b; long at, dur; int state; } script[MAX_SCRIPTED_PRESSES];
    int nscript = 0;
    for (int i = 0; i < o->npress; i++) {
        EmuButton b = button_by_name(o->presses[i].name);
        if (b == BTN_COUNT) return 1;
        script[nscript].b     = b;
        script[nscript].at    = o->presses[i].at;
        script[nscript].dur   = o->presses[i].dur;
        script[nscript].state = 0;
        nscript++;
    }

    emu_set_autostart(o->power_at >= 0, (u64)o->power_at * 1000000);

    clock_t t0 = clock();
    u64 target = (u64)o->headless * 1000000;
    u64 last_report = 0;

    while (emu_cycles() < target && !emu_halted()) {
        u64 before = emu_cycles();
        if (!emu_step(65536)) break;
        u64 done = emu_cycles();

        for (int k = 0; k < nscript; k++) {
            u64 at = (u64)script[k].at * 1000000;
            if (script[k].state == 0 && done >= at) {
                gwlog("[cli] pressing %s\n", emu_button_name(script[k].b));
                emu_set_button(script[k].b, true);
                script[k].state = 1;
            } else if (script[k].state == 1 &&
                       done >= at + (u64)script[k].dur * 1000000) {
                gwlog("[cli] releasing %s\n", emu_button_name(script[k].b));
                emu_set_button(script[k].b, false);
                script[k].state = 2;
            }
        }

        if (done / 50000000 != last_report) {
            last_report = done / 50000000;
            gwlog("[t] %4llum insns  pc=%08x sleeping=%d frames=%d  "
                  "audio %llu @%u Hz range %d..%d\n",
                  (unsigned long long)(done / 1000000), cpu.pc, cpu.sleeping,
                  emu_frame_counter(), (unsigned long long)audio_pushed,
                  emu_audio_rate(), audio_min, audio_max);
        }
        (void)before;
    }

    double wall = (double)(clock() - t0) / CLOCKS_PER_SEC;
    u64 done = emu_cycles();
    gwlog("[cli] headless run finished: %llu instructions, halted=%d pc=%08x\n",
          (unsigned long long)done, emu_halted(), cpu.pc);
    if (wall > 0.0)
        printf("throughput: %.1f MIPS executed, %.2fx real time (%.2fs wall)\n",
               (double)done / wall / 1e6,
               (double)done / (double)opt_core_hz / wall, wall);

    if (o->raw_len && o->raw_file) {
        FILE *rf = fopen(o->raw_file, "wb");
        if (rf) {
            for (u32 a = 0; a < o->raw_len; a++) fputc((int)bus_read8(o->raw_addr + a), rf);
            fclose(rf);
            printf("wrote %s (%u bytes from %08x)\n", o->raw_file, o->raw_len, o->raw_addr);
        } else perror(o->raw_file);
    }

    periph_dump_state();
    cpu_dump_exceptions();

    if (o->dump_len) {
        gwlog("---- memory %08x +%u ----\n", o->dump_addr, o->dump_len);
        u32 nonzero = 0;
        for (u32 a = 0; a < o->dump_len; a += 4) if (bus_read32(o->dump_addr + a)) nonzero++;
        gwlog("non-zero words: %u / %u\n", nonzero, o->dump_len / 4);
        for (u32 a = 0; a < o->dump_len && a < 128; a += 16) {
            gwlog("%08x:", o->dump_addr + a);
            for (u32 b = 0; b < 16; b += 4) gwlog(" %08x", bus_read32(o->dump_addr + a + b));
            gwlog("\n");
        }
    }
    if (o->trace)   cpu_dump_history("end of run");
    if (o->profile) { gwlog("---- hottest PCs ----\n"); cpu_dump_profile(25); }

    if (!emu_halted() && !o->no_power_off) emu_power_off_and_wait();

    printf("ran %llu instructions, pc=%08x halted=%d frames=%d\n",
           (unsigned long long)done, cpu.pc, emu_halted(), emu_frame_counter());
    if (o->dump_fb) write_ppm(o->dump_fb);
    return 0;
}
