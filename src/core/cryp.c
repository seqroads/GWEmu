/* cryp.c - CRYP AES accelerator
 *
 * Zelda decrypts its NES ROMs through this on the way out of the menu, in
 * AES-128-GCM. Without it the firmware sets CRYPEN, waits for the hardware to
 * clear it, times out after 299 polls and gives up with a black screen.
 *
 * ECB, CBC and CTR are here too because once the block cipher and the FIFOs
 * exist they cost almost nothing.
 */

#include "gw.h"
#include "aes.h"

#define CRYP_BASE 0x48021000u

/* CR */
#define CR_ALGODIR   (1u << 2)          /* 0 encrypt, 1 decrypt */
#define CR_ALGOMODE  0x00080038u        /* [5:3] plus bit 19 as the top bit */
#define CR_KEYSIZE   (3u << 8)
#define CR_FFLUSH    (1u << 14)
#define CR_CRYPEN    (1u << 15)
#define CR_GCMPH     (3u << 16)

/* SR */
#define SR_IFEM      (1u << 0)          /* input FIFO empty     */
#define SR_IFNF      (1u << 1)          /* input FIFO not full  */
#define SR_OFNE      (1u << 2)          /* output FIFO not empty */

/* RISR/IMSCR: the driver is interrupt-driven, so these are what actually
   move it along. */
#define INRIS        (1u << 0)          /* input FIFO wants data     */
#define OUTRIS       (1u << 1)          /* output FIFO has data      */

#define IRQ_CRYP     79

enum { ALG_TDES_ECB = 0, ALG_TDES_CBC, ALG_DES_ECB, ALG_DES_CBC,
       ALG_AES_ECB, ALG_AES_CBC, ALG_AES_CTR, ALG_AES_KEY,
       ALG_AES_GCM, ALG_AES_CCM };

enum { PH_INIT = 0, PH_HEADER, PH_PAYLOAD, PH_FINAL };

static struct {
    u32 cr, dmacr, imscr, risr;
    u32 key[8];                 /* K0LR..K3RR */
    u32 iv[4];                  /* IV0LR..IV1RR */

    AES128 aes;
    u8  ctr[16];                /* GCM/CTR counter block, or CBC chaining state */
    u8  h[16];                  /* GHASH subkey */
    u8  ghash[16];              /* running GHASH */
    u8  tag_mask[16];           /* E(K, Y0), saved during init for the tag */

    u32 din[4];
    int din_count;
    u32 dout[4];
    int dout_count, dout_pos;
} cryp;

static u32 alg(void)   { return ((cryp.cr >> 3) & 7) | ((cryp.cr >> 16) & 8); }
static u32 phase(void) { return (cryp.cr >> 16) & 3; }

/* Flags follow the FIFOs directly rather than being latched, and the line is
   level-driven, so this can simply be called after anything that moves data. */
static u32 raw_status(void)
{
    if (!(cryp.cr & CR_CRYPEN)) return 0;
    u32 r = 0;
    if (cryp.din_count < 4) r |= INRIS;
    if (cryp.dout_pos < cryp.dout_count) r |= OUTRIS;
    return r;
}

static void update_irq(void)
{
    cryp.risr = raw_status();
    cpu_raise_irq(IRQ_CRYP, (cryp.risr & cryp.imscr) != 0);
}

/* ------------------------------------------------------------------ */
/* data swapping                                                       */
/* ------------------------------------------------------------------ */

/* DATATYPE says how the 32-bit words the firmware writes map onto the byte
   stream the cipher sees. */
static u32 swap_word(u32 v)
{
    switch ((cryp.cr >> 6) & 3) {
    case 0: return v;                                        /* 32-bit, no swap */
    case 1: return ((v & 0xFFFF0000u) >> 16) | ((v & 0x0000FFFFu) << 16);
    case 2: return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) |
                   ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
    default: {                                               /* bit swap */
        u32 r = 0;
        for (int i = 0; i < 32; i++) if (v & (1u << i)) r |= 1u << (31 - i);
        return r;
    }
    }
}

static void words_to_block(const u32 w[4], u8 b[16])
{
    for (int i = 0; i < 4; i++) {
        u32 v = swap_word(w[i]);
        b[i * 4 + 0] = (u8)(v >> 24);
        b[i * 4 + 1] = (u8)(v >> 16);
        b[i * 4 + 2] = (u8)(v >> 8);
        b[i * 4 + 3] = (u8)v;
    }
}

static void block_to_words(const u8 b[16], u32 w[4])
{
    for (int i = 0; i < 4; i++) {
        u32 v = ((u32)b[i * 4] << 24) | ((u32)b[i * 4 + 1] << 16) |
                ((u32)b[i * 4 + 2] << 8) | b[i * 4 + 3];
        w[i] = swap_word(v);
    }
}

/* ------------------------------------------------------------------ */
/* GHASH                                                               */
/* ------------------------------------------------------------------ */

/* Multiplication in GF(2^128) with the GCM bit ordering, right-shift form. */
static void gf_mul(u8 x[16], const u8 y[16])
{
    u8 z[16] = {0}, v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1)
            for (int k = 0; k < 16; k++) z[k] ^= v[k];
        bool lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (u8)((v[k] >> 1) | (v[k - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ghash_block(const u8 block[16])
{
    for (int i = 0; i < 16; i++) cryp.ghash[i] ^= block[i];
    gf_mul(cryp.ghash, cryp.h);
}

static void ctr_inc(void)
{
    for (int i = 15; i >= 12; i--) if (++cryp.ctr[i]) break;
}

/* ------------------------------------------------------------------ */
/* key and start                                                       */
/* ------------------------------------------------------------------ */

static void load_key(void)
{
    /* A 128-bit key lives in the low half, K2LR..K3RR. */
    u8 k[16];
    for (int i = 0; i < 4; i++) {
        u32 v = cryp.key[4 + i];
        k[i * 4 + 0] = (u8)(v >> 24);
        k[i * 4 + 1] = (u8)(v >> 16);
        k[i * 4 + 2] = (u8)(v >> 8);
        k[i * 4 + 3] = (u8)v;
    }
    aes128_expand(&cryp.aes, k);

    if (((cryp.cr & CR_KEYSIZE) >> 8) != 0)
        gwlog("[cryp] %u-bit keys are not modelled; treating as 128\n",
              128u << ((cryp.cr & CR_KEYSIZE) >> 8));
}

static void iv_to_block(u8 b[16])
{
    for (int i = 0; i < 4; i++) {
        u32 v = cryp.iv[i];
        b[i * 4 + 0] = (u8)(v >> 24);
        b[i * 4 + 1] = (u8)(v >> 16);
        b[i * 4 + 2] = (u8)(v >> 8);
        b[i * 4 + 3] = (u8)v;
    }
}

/* The init phase is what the firmware waits on: it sets CRYPEN and spins
   until the hardware clears it again. */
static void gcm_init(void)
{
    load_key();

    u8 zero[16] = {0};
    aes128_encrypt(&cryp.aes, zero, cryp.h);

    iv_to_block(cryp.ctr);
    memset(cryp.ghash, 0, sizeof cryp.ghash);

    /* Software loads IV||2, so the block that masks the tag is IV||1. */
    u8 y0[16];
    memcpy(y0, cryp.ctr, 16);
    y0[12] = 0; y0[13] = 0; y0[14] = 0; y0[15] = 1;
    aes128_encrypt(&cryp.aes, y0, cryp.tag_mask);
}

static void start(void)
{
    cryp.din_count = cryp.dout_count = cryp.dout_pos = 0;

    switch (alg()) {
    case ALG_AES_GCM:
        if (phase() == PH_INIT) {
            gcm_init();
            /* Self-clearing: the init phase is over as soon as it is asked for. */
            cryp.cr &= ~CR_CRYPEN;
        }
        break;
    case ALG_AES_KEY:
        /* Key preparation for CBC/ECB decryption. Nothing to prepare here -
           the model only ever needs the forward cipher. */
        load_key();
        cryp.cr &= ~CR_CRYPEN;
        break;
    case ALG_AES_ECB:
    case ALG_AES_CBC:
    case ALG_AES_CTR:
        load_key();
        iv_to_block(cryp.ctr);
        break;
    default:
        gwlog("[cryp] algorithm %u is not modelled\n", alg());
        cryp.cr &= ~CR_CRYPEN;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* block processing                                                    */
/* ------------------------------------------------------------------ */

static void push_out(const u8 block[16])
{
    block_to_words(block, cryp.dout);
    cryp.dout_count = 4;
    cryp.dout_pos = 0;
}

static void process_block(void)
{
    u8 in[16], out[16], ks[16];
    words_to_block(cryp.din, in);
    cryp.din_count = 0;

    switch (alg()) {
    case ALG_AES_GCM:
        switch (phase()) {
        case PH_HEADER:
            ghash_block(in);
            return;                                  /* no output */

        case PH_PAYLOAD:
            aes128_encrypt(&cryp.aes, cryp.ctr, ks);
            ctr_inc();
            for (int i = 0; i < 16; i++) out[i] = in[i] ^ ks[i];
            /* GHASH always covers the ciphertext, whichever direction. */
            ghash_block((cryp.cr & CR_ALGODIR) ? in : out);
            push_out(out);
            return;

        case PH_FINAL:
            /* The block the firmware writes carries the two lengths, and the
               hardware hashes exactly what it was given. */
            ghash_block(in);
            for (int i = 0; i < 16; i++) out[i] = cryp.ghash[i] ^ cryp.tag_mask[i];
            push_out(out);
            return;
        default:
            return;
        }

    case ALG_AES_CTR:
        aes128_encrypt(&cryp.aes, cryp.ctr, ks);
        for (int i = 15; i >= 0; i--) if (++cryp.ctr[i]) break;
        for (int i = 0; i < 16; i++) out[i] = in[i] ^ ks[i];
        push_out(out);
        return;

    case ALG_AES_ECB:
        if (cryp.cr & CR_ALGODIR) {
            gwlog("[cryp] AES-ECB decryption is not modelled\n");
            memcpy(out, in, 16);
        } else {
            aes128_encrypt(&cryp.aes, in, out);
        }
        push_out(out);
        return;

    case ALG_AES_CBC:
        if (cryp.cr & CR_ALGODIR) {
            gwlog("[cryp] AES-CBC decryption is not modelled\n");
            memcpy(out, in, 16);
        } else {
            u8 x[16];
            for (int i = 0; i < 16; i++) x[i] = in[i] ^ cryp.ctr[i];
            aes128_encrypt(&cryp.aes, x, out);
            memcpy(cryp.ctr, out, 16);
        }
        push_out(out);
        return;

    default:
        push_out(in);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* registers                                                           */
/* ------------------------------------------------------------------ */

static u32 cryp_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x00: return cryp.cr;
    case 0x04: {
        /* The input side never stalls; the output side reports whatever the
           last processed block left behind. */
        u32 sr = SR_IFNF;
        if (!cryp.din_count) sr |= SR_IFEM;
        if (cryp.dout_pos < cryp.dout_count) sr |= SR_OFNE;
        return sr;
    }
    case 0x0C: {
        u32 v = 0;
        if (cryp.dout_pos < cryp.dout_count) v = cryp.dout[cryp.dout_pos++];
        update_irq();
        return v;
    }
    case 0x10: return cryp.dmacr;
    case 0x14: return cryp.imscr;
    case 0x18: return raw_status();
    case 0x1C: return raw_status() & cryp.imscr;
    default:
        if (off >= 0x20 && off < 0x40) return cryp.key[(off - 0x20) / 4];
        if (off >= 0x40 && off < 0x50) return cryp.iv[(off - 0x40) / 4];
        return 0;
    }
}

static void cryp_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    case 0x00: {
        u32 old = cryp.cr;
        if (val & CR_FFLUSH) {
            cryp.din_count = cryp.dout_count = cryp.dout_pos = 0;
            val &= ~CR_FFLUSH;
        }
        cryp.cr = val;

        /* Enabling starts a run, but so does changing phase or algorithm while
           already enabled - software moves through the GCM phases without
           dropping CRYPEN in between. */
        bool enabled  = (val & CR_CRYPEN) != 0;
        bool rising   = enabled && !(old & CR_CRYPEN);
        bool switched = ((val ^ old) & (CR_GCMPH | CR_ALGOMODE | CR_ALGODIR)) != 0;
        if (enabled && (rising || switched)) start();
        update_irq();
        return;
    }
    case 0x08:
        if (cryp.din_count < 4) cryp.din[cryp.din_count++] = val;
        if (cryp.din_count == 4 && (cryp.cr & CR_CRYPEN)) process_block();
        update_irq();
        return;
    case 0x10: cryp.dmacr = val; return;
    case 0x14: cryp.imscr = val; update_irq(); return;
    case 0x18: return;                       /* read-only */
    default:
        if (off >= 0x20 && off < 0x40) { cryp.key[(off - 0x20) / 4] = val; return; }
        if (off >= 0x40 && off < 0x50) { cryp.iv[(off - 0x40) / 4] = val; return; }
        return;
    }
}

void cryp_init(void)
{
    memset(&cryp, 0, sizeof cryp);
    cpu_raise_irq(IRQ_CRYP, false);
    periph_register(CRYP_BASE, 0x400, "CRYP", cryp_read, cryp_write);
}
