/* octospi.c - OCTOSPI1 controller, MX25U8035F flash model, OTFDEC1 decryption */

#include "gw.h"
#include "aes.h"

extern u8 *extflash_plain;

/* ================================================================== */
/* OTFDEC1                                                             */
/* ================================================================== */

typedef struct {
    u32 cfgr, start, end;
    u32 nonce[2];
    u32 key[4];
    u8  keycrc;                      /* hardware-computed, read-only in CFGR[15:8] */
    bool decrypted;
} OtfRegion;

/* OTFDEC computes a CRC over the loaded key and publishes it in R{n}CFGR[15:8];
   the firmware recomputes the same value in software and refuses to continue if
   they disagree, so the exact algorithm has to match bit for bit. */
static u8 otfdec_key_crc(const u32 key[4])
{
    static const u32 iv[4] = { 0xAA55AA55u, 0x00000003u, 0x00000018u, 0x000000C0u };
    u32 crc = 0;
    for (int i = 0; i < 4; i++) {
        u32 mix;
        if (i == 0) {
            mix = iv[0];
        } else {
            u32 c = crc & 0xFF, v = iv[i];
            mix = c | (c << 16) | (v << 24) | (v << 8);
        }
        u32 d = key[i] ^ mix;
        u32 r = 0;
        for (int b = 0; b < 32; b++) {
            u32 in = (d >> (31 - b)) & 1;
            u32 top = (r & 0x80) >> 7;
            r <<= 1;
            if ((in ^ top) & 1) r ^= 7;
        }
        crc = r ^ 0x55u;
    }
    return (u8)crc;
}

static struct {
    u32 cr;
    OtfRegion rgn[4];
    u32 isr, icr, ier;
} otf;

/* Does any enabled+decrypted OTFDEC region cover this address? */
bool otfdec_covers(u32 addr)
{
    for (int i = 0; i < 4; i++) {
        OtfRegion *r = &otf.rgn[i];
        if (!r->decrypted) continue;
        if (addr >= r->start && addr <= r->end) return true;
    }
    return false;
}

static void otfdec_decrypt_region(int idx)
{
    OtfRegion *r = &otf.rgn[idx];
    if (!(r->cfgr & 1)) return;                       /* REG_EN */
    if (r->end < r->start) return;
    if (r->start < EXTFLASH_BASE || r->end >= EXTFLASH_BASE + extflash_size) {
        gwlog("[otfdec] region %d range %08x..%08x outside external flash\n", idx + 1, r->start, r->end);
        return;
    }

    /* Key: the four key registers, most significant register first, big-endian. */
    u8 key[16];
    for (int i = 0; i < 4; i++) {
        u32 v = r->key[3 - i];
        key[i * 4 + 0] = (u8)(v >> 24); key[i * 4 + 1] = (u8)(v >> 16);
        key[i * 4 + 2] = (u8)(v >> 8);  key[i * 4 + 3] = (u8)v;
    }
    AES128 aes;
    aes128_expand(&aes, key);

    u32 version = r->cfgr >> 16;
    u8 iv[16];
    for (int i = 0; i < 2; i++) {
        u32 v = r->nonce[1 - i];
        iv[i * 4 + 0] = (u8)(v >> 24); iv[i * 4 + 1] = (u8)(v >> 16);
        iv[i * 4 + 2] = (u8)(v >> 8);  iv[i * 4 + 3] = (u8)v;
    }
    iv[8] = 0; iv[9] = 0;
    iv[10] = (u8)(version >> 8); iv[11] = (u8)version;

    u32 first = r->start & ~0xFu;
    u32 last  = r->end | 0xFu;
    u32 nblk = 0;
    for (u32 addr = first; addr <= last && addr >= first; addr += 16) {
        u32 ctr = addr >> 4;
        iv[12] = (u8)(((u32)idx << 4) | ((ctr >> 24) & 0x0F));
        iv[13] = (u8)(ctr >> 16);
        iv[14] = (u8)(ctr >> 8);
        iv[15] = (u8)ctr;

        u8 ks[16];
        aes128_encrypt(&aes, iv, ks);

        u32 off = addr - EXTFLASH_BASE;
        for (int j = 0; j < 16; j++)
            extflash_plain[off + j] = mem_extflash[off + j] ^ ks[15 - j];
        nblk++;
        if (addr + 16 < addr) break;
    }
    r->decrypted = true;
    gwlog("[otfdec] region %d decrypted: %08x..%08x version=%04x (%u blocks)\n",
          idx + 1, r->start, r->end, version, nblk);
}

#define OTF_RGN_BASE 0x20
#define OTF_RGN_STRIDE 0x30

static u32 otfdec_read(u32 off, int size)
{
    (void)size;
    if (off == 0x000) return otf.cr;
    if (off == 0x300) return otf.isr;
    if (off == 0x308) return otf.ier;
    if (off >= OTF_RGN_BASE && off < OTF_RGN_BASE + 4 * OTF_RGN_STRIDE) {
        u32 i = (off - OTF_RGN_BASE) / OTF_RGN_STRIDE;
        u32 r = (off - OTF_RGN_BASE) % OTF_RGN_STRIDE;
        OtfRegion *g = &otf.rgn[i];
        switch (r) {
        case 0x00: return (g->cfgr & ~0xFF00u) | ((u32)g->keycrc << 8);
        case 0x04: return g->start;
        case 0x08: return g->end;
        case 0x0C: return g->nonce[0];
        case 0x10: return g->nonce[1];
        /* key registers are write-only on real hardware */
        case 0x14: case 0x18: case 0x1C: case 0x20: return 0;
        }
    }
    return 0;
}

static void otfdec_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off == 0x000) { otf.cr = val; return; }
    if (off == 0x30C) { otf.isr &= ~val; return; }
    if (off == 0x308) { otf.ier = val; return; }
    if (off >= OTF_RGN_BASE && off < OTF_RGN_BASE + 4 * OTF_RGN_STRIDE) {
        u32 i = (off - OTF_RGN_BASE) / OTF_RGN_STRIDE;
        u32 r = (off - OTF_RGN_BASE) % OTF_RGN_STRIDE;
        OtfRegion *g = &otf.rgn[i];
        switch (r) {
        case 0x00:
            g->cfgr = (val & ~0xFF00u);      /* KEYCRC is read-only */
            g->decrypted = false;
            if (val & 1) otfdec_decrypt_region(i);
            return;
        case 0x04: g->start = val; g->decrypted = false; return;
        case 0x08: g->end = val; g->decrypted = false; return;
        case 0x0C: g->nonce[0] = val; g->decrypted = false; return;
        case 0x10: g->nonce[1] = val; g->decrypted = false; return;
        case 0x14: g->key[0] = val; g->decrypted = false; return;
        case 0x18: g->key[1] = val; g->decrypted = false; return;
        case 0x1C: g->key[2] = val; g->decrypted = false; return;
        case 0x20:
            g->key[3] = val;
            g->decrypted = false;
            g->keycrc = otfdec_key_crc(g->key);
            gwlog("[otfdec] region %u key loaded, CRC=%02x\n", i + 1, g->keycrc);
            if (g->cfgr & 1) otfdec_decrypt_region(i);
            return;
        }
    }
}

/* ================================================================== */
/* MX25U8035F (1 MiB QSPI NOR) behind OCTOSPI1                        */
/* ================================================================== */

static struct {
    u32 cr, dcr1, dcr2, dcr3, dcr4;
    u32 sr, dlr, ar, ccr, tcr, ir, abr, lptr;
    u32 psmkr, psmar, pir;
    u32 wccr, wtcr, wir, wabr;
    /* response buffer for indirect reads */
    u8  rxbuf[512];
    u32 rxlen, rxpos;
    u8  txbuf[512];
    u32 txpos;
    bool wren;
    u32 cur_addr;
} qspi;

#define SR_TEF  (1u << 0)
#define SR_TCF  (1u << 1)
#define SR_FTF  (1u << 2)
#define SR_SMF  (1u << 3)
#define SR_TOF  (1u << 4)
#define SR_BUSY (1u << 5)

static void qspi_push_rx(const u8 *d, u32 n)
{
    if (n > sizeof qspi.rxbuf) n = sizeof qspi.rxbuf;
    memcpy(qspi.rxbuf, d, n);
    qspi.rxlen = n;
    qspi.rxpos = 0;
    qspi.sr |= SR_FTF;
}

static void qspi_execute(void)
{
    u32 fmode = (qspi.cr >> 28) & 3;
    u32 insn = qspi.ir & 0xFF;
    u32 addr = qspi.ar;
    u32 nbytes = (qspi.dlr == 0xFFFFFFFFu) ? 0 : qspi.dlr + 1;

    qspi.sr &= ~(SR_TCF | SR_SMF);

    if (fmode == 2) {                                 /* automatic status polling */
        /* The flash is never busy in the emulator, so the match happens at once. */
        qspi.sr |= SR_SMF | SR_TCF;
        if (qspi.cr & (1u << 19)) cpu_raise_irq(92, true);   /* OCTOSPI1 global IRQ */
        return;
    }
    if (fmode == 3) return;                           /* memory-mapped: bus handles reads */

    switch (insn) {
    case 0x9F: {                                      /* RDID */
        /* Macronix MX25U-series: the last byte is log2 of the capacity, so the
           firmware is told about whichever part the dump came from. */
        u8 cap = 0x34;                                /* 1 MiB */
        for (u32 sz = 1024u * 1024; sz < extflash_size; sz *= 2) cap++;
        const u8 id[3] = { 0xC2, 0x25, cap };
        qspi_push_rx(id, 3);
        break;
    }
    case 0x5A: {                                      /* Read SFDP */
        u8 z[16] = { 'S', 'F', 'D', 'P' };
        qspi_push_rx(z, sizeof z);
        break;
    }
    case 0x05: { u8 s = 0x00; qspi_push_rx(&s, 1); break; }   /* RDSR: never busy */
    case 0x15: { u8 s = 0x00; qspi_push_rx(&s, 1); break; }   /* RDCR */
    case 0x35: { u8 s = 0x00; qspi_push_rx(&s, 1); break; }   /* RDCR (QPI) */
    case 0x06: qspi.wren = true; break;                       /* WREN */
    case 0x04: qspi.wren = false; break;                      /* WRDI */
    case 0x66: case 0x99: break;                              /* reset enable / reset */
    case 0xB9: case 0xAB: break;                              /* deep power down / release */
    case 0x35 | 0x100: break;
    case 0x01: break;                                          /* WRSR */
    case 0x03: case 0x0B: case 0x3B: case 0x6B: case 0xBB: case 0xEB: {
        /* read data - indirect */
        if (addr < extflash_size) {
            u32 n = nbytes ? nbytes : 1;
            if (n > sizeof qspi.rxbuf) n = sizeof qspi.rxbuf;
            const u8 *src = otfdec_covers(EXTFLASH_BASE + addr) ? extflash_plain : mem_extflash;
            qspi_push_rx(src + addr, n);
        }
        break;
    }
    case 0x02: case 0x38: {                                    /* page program */
        qspi.cur_addr = addr;
        qspi.txpos = 0;
        qspi.sr |= SR_FTF;
        break;
    }
    case 0x20: case 0x52: case 0xD8: {                         /* erase */
        u32 sz = (insn == 0x20) ? 0x1000 : (insn == 0x52) ? 0x8000 : 0x10000;
        u32 base = addr & ~(sz - 1);
        if (base + sz <= extflash_size) {
            memset(mem_extflash + base, 0xFF, sz);
            memset(extflash_plain + base, 0xFF, sz);
            gwlog("[qspi] erase %06x..%06x\n", base, base + sz - 1);
        }
        break;
    }
    case 0x60: case 0xC7:                                      /* chip erase */
        memset(mem_extflash, 0xFF, extflash_size);
        memset(extflash_plain, 0xFF, extflash_size);
        gwlog("[qspi] chip erase\n");
        break;
    default:
        gwlog("[qspi] unhandled instruction %02x (fmode=%u addr=%08x len=%u)\n",
              insn, fmode, addr, nbytes);
        break;
    }

    qspi.sr |= SR_TCF;
    if (qspi.cr & (1u << 17)) cpu_raise_irq(92, true);
}

static u32 qspi_read(u32 off, int size)
{
    switch (off) {
    case 0x000: return qspi.cr;
    case 0x008: return qspi.dcr1;
    case 0x00C: return qspi.dcr2;
    case 0x010: return qspi.dcr3;
    case 0x014: return qspi.dcr4;
    case 0x020: return qspi.sr | ((qspi.rxlen - qspi.rxpos) << 8);
    case 0x024: return 0;
    case 0x040: return qspi.dlr;
    case 0x048: return qspi.ar;
    case 0x050: {                                     /* DR */
        u32 v = 0;
        for (int i = 0; i < size; i++) {
            u8 b = (qspi.rxpos < qspi.rxlen) ? qspi.rxbuf[qspi.rxpos++] : 0xFF;
            v |= (u32)b << (i * 8);
        }
        if (qspi.rxpos >= qspi.rxlen) qspi.sr &= ~SR_FTF;
        return v;
    }
    case 0x080: return qspi.psmkr;
    case 0x088: return qspi.psmar;
    case 0x090: return qspi.pir;
    case 0x100: return qspi.ccr;
    case 0x108: return qspi.tcr;
    case 0x110: return qspi.ir;
    case 0x120: return qspi.abr;
    case 0x130: return qspi.lptr;
    case 0x140: return qspi.wccr;
    case 0x148: return qspi.wtcr;
    case 0x150: return qspi.wir;
    case 0x160: return qspi.wabr;
    }
    return 0;
}

static void qspi_write(u32 off, u32 val, int size)
{
    switch (off) {
    case 0x000:
        qspi.cr = val;
        if (val & 2) { qspi.sr &= ~(SR_BUSY | SR_FTF); qspi.rxlen = qspi.rxpos = 0; qspi.cr &= ~2u; }
        return;
    case 0x008: qspi.dcr1 = val; return;
    case 0x00C: qspi.dcr2 = val; return;
    case 0x010: qspi.dcr3 = val; return;
    case 0x014: qspi.dcr4 = val; return;
    case 0x024:                                        /* FCR */
        qspi.sr &= ~(val & (SR_TEF | SR_TCF | SR_SMF | SR_TOF));
        return;
    case 0x040: qspi.dlr = val; return;
    case 0x048:
        qspi.ar = val;
        qspi_execute();
        return;
    case 0x050: {                                      /* DR: program data */
        for (int i = 0; i < size; i++) {
            u8 b = (u8)(val >> (i * 8));
            u32 a = qspi.cur_addr + qspi.txpos;
            if (a < extflash_size) {
                mem_extflash[a] &= b;                  /* NOR: writes can only clear bits */
                extflash_plain[a] &= b;
            }
            qspi.txpos++;
        }
        return;
    }
    case 0x080: qspi.psmkr = val; return;
    case 0x088: qspi.psmar = val; return;
    case 0x090: qspi.pir = val; return;
    case 0x100: qspi.ccr = val; return;
    case 0x108: qspi.tcr = val; return;
    case 0x110:
        qspi.ir = val;
        /* A command with no address phase starts as soon as IR is written. */
        if (((qspi.ccr >> 8) & 7) == 0 || ((qspi.cr >> 28) & 3) == 2) qspi_execute();
        return;
    case 0x120: qspi.abr = val; return;
    case 0x130: qspi.lptr = val; return;
    case 0x140: qspi.wccr = val; return;
    case 0x148: qspi.wtcr = val; return;
    case 0x150: qspi.wir = val; return;
    case 0x160: qspi.wabr = val; return;
    }
}

/* OCTOSPI I/O manager: pure configuration, nothing to model */
static u32 ospim_read(u32 off, int size) { (void)off; (void)size; return 0; }
static void ospim_write(u32 off, u32 val, int size) { (void)off; (void)val; (void)size; }

void octospi_init(void)
{
    memset(&qspi, 0, sizeof qspi);
    memset(&otf, 0, sizeof otf);
    qspi.dlr = 0xFFFFFFFFu;
    periph_register(0x52005000, 0x1000, "OCTOSPI1", qspi_read, qspi_write);
    periph_register(0x5200A000, 0x1000, "OCTOSPI2", qspi_read, qspi_write);
    periph_register(0x5200B400, 0x400, "OCTOSPIM", ospim_read, ospim_write);
    periph_register(0x5200B800, 0x400, "OTFDEC1", otfdec_read, otfdec_write);
    periph_register(0x5200BC00, 0x400, "OTFDEC2", otfdec_read, otfdec_write);
}

/* ================================================================== */
/* Save-state support                                                  */
/* ================================================================== */

void octospi_state_blocks(StateBlockFn fn, void *ctx)
{
    fn(ctx, "otfdec", &otf,  sizeof otf);
    fn(ctx, "qspi",   &qspi, sizeof qspi);
}
