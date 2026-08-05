/* state.c - persistence for the parts of the device that survive power-off:
 * the external flash (where the firmware keeps its save data) and the backup
 * domain (backup SRAM plus the battery-backed clock).
 *
 * The user's flash dump is never written to; everything lands in a separate
 * save file.
 */

#include "gw.h"
#include <time.h>

#define STATE_MAGIC   0x57474D45u        /* "EMGW" */
#define STATE_VERSION 2

typedef struct {
    u32 magic;
    u32 version;
    u32 ext_size;
    u32 bkp_size;
    s64 saved_at;                        /* host wall clock, for clock catch-up */
    RtcState rtc;
    u32 bdcr;                            /* RCC_BDCR is in the backup domain */
    u32 tamp[32];                        /* TAMP backup registers            */
} StateHeader;

bool state_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    StateHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 ||
        h.magic != STATE_MAGIC || h.version != STATE_VERSION ||
        h.ext_size != extflash_size || h.bkp_size != BKPSRAM_SIZE) {
        gwlog("[state] %s is not a usable save file, ignoring it\n", path);
        fclose(f);
        return false;
    }

    if (fread(mem_extflash, 1, extflash_size, f) != extflash_size ||
        fread(mem_bkp, 1, BKPSRAM_SIZE, f) != BKPSRAM_SIZE) {
        gwlog("[state] %s is truncated, ignoring it\n", path);
        fclose(f);
        return false;
    }
    fclose(f);

    /* A real unit keeps time while it is switched off. */
    s64 now = (s64)time(NULL);
    s64 elapsed = now - h.saved_at;
    if (elapsed < 0 || elapsed > 366 * 24 * 3600) elapsed = 0;
    rtc_restore_state(&h.rtc, (u32)elapsed);
    /* The backup domain survives power-off on the real unit; without this the
       firmware sees RTCSEL unset, resets the backup domain and wipes the clock. */
    rcc_set_bdcr(h.bdcr);
    memcpy(tamp_bkp, h.tamp, sizeof tamp_bkp);

    gwlog("[state] restored from %s (clock advanced %lld s)\n", path, (long long)elapsed);
    return true;
}

void state_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { gwlog("[state] cannot write %s\n", path); return; }

    StateHeader h = {
        .magic = STATE_MAGIC,
        .version = STATE_VERSION,
        .ext_size = extflash_size,
        .bkp_size = BKPSRAM_SIZE,
        .saved_at = (s64)time(NULL),
    };
    rtc_save_state(&h.rtc);
    h.bdcr = rcc_get_bdcr();
    memcpy(h.tamp, tamp_bkp, sizeof h.tamp);

    bool ok = fwrite(&h, sizeof h, 1, f) == 1 &&
              fwrite(mem_extflash, 1, extflash_size, f) == extflash_size &&
              fwrite(mem_bkp, 1, BKPSRAM_SIZE, f) == BKPSRAM_SIZE;
    fclose(f);
    gwlog(ok ? "[state] saved to %s\n" : "[state] failed writing %s\n", path);
}
