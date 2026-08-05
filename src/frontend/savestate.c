/* savestate.c - whole-machine snapshots
 *
 * The core hands out its state as tagged, pointer-free blocks. Tags mean a
 * snapshot from a build with different struct sizes is rejected, not misread.
 */

#include "savestate.h"
#include "session.h"
#include "paths.h"
#include <SDL.h>

#define STATE_MAGIC   0x54535747u        /* "GWST" */
#define STATE_VERSION 1
#define TAG_LEN       16

typedef struct {
    u32 magic, version;
    u64 firmware_hash;
    u32 core_hz, block_count;
} StateHeader;

static int current_slot;

int  savestate_slot(void) { return current_slot; }
void savestate_set_slot(int n) { if (n >= 0 && n < SAVESTATE_SLOTS) current_slot = n; }

static bool slot_path(char *out, size_t cap, int n)
{
    if (!session.open || n < 0 || n >= SAVESTATE_SLOTS) return false;
    char name[64];
    SDL_snprintf(name, sizeof name, "%016llx.st%d", (unsigned long long)session.hash, n);
    user_path(out, cap, name);
    return true;
}

bool savestate_exists(int n)
{
    char p[SESSION_PATH_MAX];
    return slot_path(p, sizeof p, n) && file_size(p) > 0;
}

/* ---- writing ---- */

typedef struct { u8 *buf; size_t len, cap; u32 count; bool ok; } WriteCtx;

static void wr(WriteCtx *w, const void *data, size_t n)
{
    if (!w->ok) return;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : (1u << 20);
        while (cap < w->len + n) cap *= 2;
        u8 *p = (u8 *)realloc(w->buf, cap);
        if (!p) { w->ok = false; return; }
        w->buf = p;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void write_block(void *ctx, const char *tag, void *data, u32 size)
{
    WriteCtx *w = (WriteCtx *)ctx;
    char name[TAG_LEN];
    memset(name, 0, sizeof name);
    SDL_strlcpy(name, tag, sizeof name);
    wr(w, name, TAG_LEN);
    wr(w, &size, sizeof size);
    wr(w, data, size);
    w->count++;
}

/* ---- reading ---- */

typedef struct { const u8 *base; size_t len; u32 restored, skipped; } ReadCtx;

static void read_block(void *ctx, const char *tag, void *data, u32 size)
{
    ReadCtx *r = (ReadCtx *)ctx;
    size_t off = sizeof(StateHeader);

    while (off + TAG_LEN + 4 <= r->len) {
        const char *name = (const char *)(r->base + off);
        u32 bs;
        memcpy(&bs, r->base + off + TAG_LEN, sizeof bs);
        size_t payload = off + TAG_LEN + 4;
        if (payload + bs > r->len) break;

        if (!strncmp(name, tag, TAG_LEN)) {
            if (bs == size) { memcpy(data, r->base + payload, size); r->restored++; }
            else {
                gwlog("[state] block '%s' is %u bytes in the file but %u here\n", tag, bs, size);
                r->skipped++;
            }
            return;
        }
        off = payload + bs;
    }
    gwlog("[state] block '%s' missing from the snapshot\n", tag);
    r->skipped++;
}

static void visit_all(StateBlockFn fn, void *ctx)
{
    fn(ctx, "cpu", &cpu, sizeof cpu);
    bus_state_blocks(fn, ctx);
    periph_state_blocks(fn, ctx);
    octospi_state_blocks(fn, ctx);
}

bool savestate_save(int n)
{
    char path[SESSION_PATH_MAX];
    if (!slot_path(path, sizeof path, n)) return false;

    StateHeader h;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.firmware_hash = session.hash;
    h.core_hz = opt_core_hz;

    WriteCtx w;
    memset(&w, 0, sizeof w);
    w.ok = true;
    wr(&w, &h, sizeof h);
    visit_all(write_block, &w);

    bool ok = false;
    if (w.ok) {
        h.block_count = w.count;
        memcpy(w.buf, &h, sizeof h);
        ok = file_write(path, w.buf, (u32)w.len);
    }
    free(w.buf);

    if (ok) gwlog("[state] wrote slot %d (%u blocks)\n", n, w.count);
    return ok;
}

bool savestate_load(int n)
{
    char path[SESSION_PATH_MAX];
    if (!slot_path(path, sizeof path, n)) return false;

    u32 len = 0;
    void *buf = file_load(path, &len);
    if (!buf || len < sizeof(StateHeader)) { free(buf); return false; }

    StateHeader h;
    memcpy(&h, buf, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION) {
        gwlog("[state] slot %d was not written by this build\n", n);
        free(buf);
        return false;
    }
    if (h.firmware_hash != session.hash) {
        gwlog("[state] slot %d belongs to a different firmware image\n", n);
        free(buf);
        return false;
    }

    ReadCtx r;
    memset(&r, 0, sizeof r);
    r.base = (const u8 *)buf;
    r.len = len;
    visit_all(read_block, &r);
    free(buf);

    periph_state_fixup();
    if (h.core_hz) opt_core_hz = h.core_hz;

    /* A half-applied snapshot is worse than none. */
    if (r.skipped) {
        gwlog("[state] %u block(s) could not be applied\n", r.skipped);
        return false;
    }
    gwlog("[state] restored slot %d (%u blocks)\n", n, r.restored);
    return true;
}
