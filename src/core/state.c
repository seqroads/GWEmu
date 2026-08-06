/* state.c - persistence for the parts of the device that survive power-off:
 * the external flash (where the firmware keeps its save data) and the backup
 * domain (backup SRAM plus the battery-backed clock).
 *
 * The user's flash dump is never written to; everything lands in a separate
 * save file. The blob is built in memory and the file layer is a wrapper, so
 * a frontend without a filesystem - the browser one - stores the same bytes.
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

u32 state_blob_size(void)
{
    return (u32)sizeof(StateHeader) + extflash_size + BKPSRAM_SIZE;
}

u32 state_save_mem(void *dst, u32 cap)
{
    u32 need = state_blob_size();
    if (!dst || cap < need) return need;

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

    u8 *p = (u8 *)dst;
    memcpy(p, &h, sizeof h);                          p += sizeof h;
    memcpy(p, mem_extflash, extflash_size);           p += extflash_size;
    memcpy(p, mem_bkp, BKPSRAM_SIZE);
    return need;
}

bool state_load_mem(const void *src, u32 len)
{
    if (!src || len < sizeof(StateHeader)) return false;

    StateHeader h;
    memcpy(&h, src, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION ||
        h.ext_size != extflash_size || h.bkp_size != BKPSRAM_SIZE) {
        gwlog("[state] not a usable save for this firmware, ignoring it\n");
        return false;
    }
    if (len < state_blob_size()) {
        gwlog("[state] save data is truncated, ignoring it\n");
        return false;
    }

    const u8 *p = (const u8 *)src + sizeof h;
    memcpy(mem_extflash, p, extflash_size);           p += extflash_size;
    memcpy(mem_bkp, p, BKPSRAM_SIZE);

    /* A real unit keeps time while it is switched off. */
    s64 elapsed = (s64)time(NULL) - h.saved_at;
    if (elapsed < 0 || elapsed > 366 * 24 * 3600) elapsed = 0;
    rtc_restore_state(&h.rtc, (u32)elapsed);
    /* The backup domain survives power-off on the real unit; without this the
       firmware sees RTCSEL unset, resets the backup domain and wipes the clock. */
    rcc_set_bdcr(h.bdcr);
    memcpy(tamp_bkp, h.tamp, sizeof tamp_bkp);

    gwlog("[state] restored save data (clock advanced %lld s)\n", (long long)elapsed);
    return true;
}

bool state_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    u32 need = state_blob_size();
    u8 *buf = (u8 *)malloc(need);
    if (!buf) { fclose(f); return false; }

    size_t got = fread(buf, 1, need, f);
    fclose(f);

    bool ok = state_load_mem(buf, (u32)got);
    free(buf);
    if (ok) gwlog("[state] read %s\n", path);
    return ok;
}

void state_save(const char *path)
{
    u32 need = state_blob_size();
    u8 *buf = (u8 *)malloc(need);
    if (!buf) return;
    state_save_mem(buf, need);

    FILE *f = fopen(path, "wb");
    if (!f) {
        gwlog("[state] cannot write %s\n", path);
        free(buf);
        return;
    }
    bool ok = fwrite(buf, 1, need, f) == need;
    fclose(f);
    free(buf);
    gwlog(ok ? "[state] saved to %s\n" : "[state] failed writing %s\n", path);
}
