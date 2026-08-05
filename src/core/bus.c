/* bus.c - STM32H7B0 memory map and peripheral dispatch */

#include "gw.h"

u8 *mem_itcm, *mem_dtcm, *mem_axi, *mem_sram1, *mem_sram2;
u8 *mem_srd, *mem_bkp, *mem_flash, *mem_extflash;
u32 extflash_size = 1024u * 1024;

u32 cpu_scs_read(u32 off, int size);
void cpu_scs_write(u32 off, u32 val, int size);

/* external flash plaintext view (filled in by the OTFDEC model) */
u8 *extflash_plain;
extern bool otfdec_covers(u32 addr);

/* ------------------------------------------------------------------ */
/* peripheral registry                                                 */
/* ------------------------------------------------------------------ */

#define MAX_PERIPH 64
typedef struct {
    u32 base, size;
    const char *name;
    periph_read_fn rd;
    periph_write_fn wr;
} Periph;

static Periph periphs[MAX_PERIPH];
static int nperiph;

/* 1 KiB-granular lookup over the 0x40000000..0x5FFFFFFF peripheral window */
#define PLUT_SHIFT 10
#define PLUT_SIZE  (0x20000000u >> PLUT_SHIFT)
static s16 plut[PLUT_SIZE];

void periph_register(u32 base, u32 size, const char *name,
                     periph_read_fn rd, periph_write_fn wr)
{
    for (int i = 0; i < nperiph; i++)
        if (periphs[i].base == base) { periphs[i] = (Periph){ base, size, name, rd, wr }; return; }
    if (nperiph >= MAX_PERIPH) { gwfatal("too many peripherals"); return; }
    periphs[nperiph] = (Periph){ base, size, name, rd, wr };
    if (base >= 0x40000000u && base < 0x60000000u) {
        u32 s = (base - 0x40000000u) >> PLUT_SHIFT;
        u32 e = (base - 0x40000000u + size + (1 << PLUT_SHIFT) - 1) >> PLUT_SHIFT;
        for (u32 i = s; i < e && i < PLUT_SIZE; i++) plut[i] = (s16)nperiph;
    }
    nperiph++;
}

static Periph *periph_find(u32 addr)
{
    if (addr >= 0x40000000u && addr < 0x60000000u) {
        s16 i = plut[(addr - 0x40000000u) >> PLUT_SHIFT];
        if (i >= 0) {
            Periph *p = &periphs[i];
            if (addr >= p->base && addr < p->base + p->size) return p;
        }
        /* Several peripherals can share one lookup slot (ADC1/ADC2/ADC_COMMON
           all live inside 0x40022000..0x400223ff), so fall through to a scan. */
    }
    for (int i = 0; i < nperiph; i++)
        if (addr >= periphs[i].base && addr < periphs[i].base + periphs[i].size)
            return &periphs[i];
    return NULL;
}

/* Unknown peripheral registers behave as plain storage so that
   read-modify-write init code doesn't spin forever. */
#define STUB_SLOTS 4096
static struct { u32 addr; u32 val; } stub[STUB_SLOTS];
static int nstub;

static u32 *stub_slot(u32 addr)
{
    for (int i = 0; i < nstub; i++) if (stub[i].addr == addr) return &stub[i].val;
    if (nstub < STUB_SLOTS) { stub[nstub].addr = addr; stub[nstub].val = 0; return &stub[nstub++].val; }
    static u32 sink;
    return &sink;
}

void cpu_dump_history(const char *why);

/* An access far outside every mapped region means the firmware is following a
   pointer we got wrong - worth a one-off backtrace. */
static void report_wild(u32 addr, const char *kind)
{
    static int reported;
    if (addr >= 0x40000000u && addr < 0x60000000u) return;
    if (addr >= 0xE0000000u) return;
    if (reported >= 4) return;
    reported++;
    gwlog("[bus] WILD %s at %08x (pc=%08x, insn #%llu)\n",
          kind, addr, cpu.pc, (unsigned long long)cpu.cycles);
    if (opt_trace) cpu_dump_history("wild access");
}

u32 periph_read(u32 addr, int size)
{
    Periph *p = periph_find(addr);
    if (p) return p->rd(addr - p->base, size);
    report_wild(addr, "read");
    if (opt_log_periph) gwlog("[bus] read%d  unmapped periph %08x (pc=%08x)\n", size * 8, addr, cpu.pc);
    return *stub_slot(addr & ~3u);
}

void periph_write(u32 addr, u32 val, int size)
{
    Periph *p = periph_find(addr);
    if (p) { p->wr(addr - p->base, val, size); return; }
    report_wild(addr, "write");
    if (opt_log_periph) gwlog("[bus] write%d unmapped periph %08x = %08x (pc=%08x)\n", size * 8, addr, val, cpu.pc);
    *stub_slot(addr & ~3u) = val;
}

/* ------------------------------------------------------------------ */
/* RAM-like region resolution                                          */
/* ------------------------------------------------------------------ */

static inline u8 *ram_ptr(u32 addr, u32 len)
{
    switch (addr >> 24) {
    case 0x00:
        if (addr + len <= ITCM_SIZE) return mem_itcm + addr;
        break;
    case 0x08:
        if (addr - FLASH_BASE + len <= FLASH_SIZE) return mem_flash + (addr - FLASH_BASE);
        break;
    case 0x20:
        if (addr - DTCM_BASE + len <= DTCM_SIZE) return mem_dtcm + (addr - DTCM_BASE);
        break;
    case 0x24:
        if (addr - AXISRAM_BASE + len <= AXISRAM_SIZE) return mem_axi + (addr - AXISRAM_BASE);
        break;
    case 0x30:
        if (addr - SRAM1_BASE + len <= SRAM1_SIZE) return mem_sram1 + (addr - SRAM1_BASE);
        if (addr >= SRAM2_BASE && addr - SRAM2_BASE + len <= SRAM2_SIZE) return mem_sram2 + (addr - SRAM2_BASE);
        break;
    case 0x38:
        if (addr - SRD_SRAM_BASE + len <= SRD_SRAM_SIZE) return mem_srd + (addr - SRD_SRAM_BASE);
        if (addr >= BKPSRAM_BASE && addr - BKPSRAM_BASE + len <= BKPSRAM_SIZE) return mem_bkp + (addr - BKPSRAM_BASE);
        break;
    case 0x90:
        if (addr - EXTFLASH_BASE + len <= extflash_size) {
            u8 *src = otfdec_covers(addr) ? extflash_plain : mem_extflash;
            return src + (addr - EXTFLASH_BASE);
        }
        break;
    }
    return NULL;
}

void *bus_host_ptr(u32 addr, u32 len) { return ram_ptr(addr, len); }

/* ------------------------------------------------------------------ */
/* accessors                                                           */
/* ------------------------------------------------------------------ */

/* The System Control Space sits at 0xE000E000; everything else in the private
   peripheral bus (ITM, DWT, FPB, ROM tables) is not modelled. */
#define SCS_BASE 0xE000E000u
#define IS_SCS(a) ((a) >= SCS_BASE && (a) < SCS_BASE + 0x1000u)

u16 bus_fetch16(u32 addr)
{
    u8 *p = ram_ptr(addr, 2);
    if (p) { u16 v; memcpy(&v, p, 2); return v; }
    gwlog("[bus] instruction fetch from unmapped %08x\n", addr);
    if (opt_trace) cpu_dump_history("bad instruction fetch");
    cpu.halted = true;
    return 0xBF00;
}

u32 bus_read32(u32 addr)
{
    u8 *p = ram_ptr(addr, 4);
    if (p) { u32 v; memcpy(&v, p, 4); return v; }
    if (IS_SCS(addr)) return cpu_scs_read(addr - SCS_BASE, 4);
    if (addr >= 0xE0000000u) return 0;
    return periph_read(addr, 4);
}

u32 bus_read16(u32 addr)
{
    u8 *p = ram_ptr(addr, 2);
    if (p) { u16 v; memcpy(&v, p, 2); return v; }
    if (IS_SCS(addr)) return cpu_scs_read(addr - SCS_BASE, 2) & 0xFFFF;
    if (addr >= 0xE0000000u) return 0;
    return periph_read(addr, 2) & 0xFFFF;
}

u32 bus_read8(u32 addr)
{
    u8 *p = ram_ptr(addr, 1);
    if (p) return *p;
    if (IS_SCS(addr)) return cpu_scs_read(addr - SCS_BASE, 1) & 0xFF;
    if (addr >= 0xE0000000u) return 0;
    return periph_read(addr, 1) & 0xFF;
}

u32 opt_watch_lo = 1, opt_watch_hi = 0;

static inline void watch_hit(u32 addr, u32 val, int size)
{
    if (addr < opt_watch_lo || addr > opt_watch_hi) return;
    gwlog("[watch] write%d %08x = %08x (pc=%08x lr=%08x insn #%llu)\n",
          size * 8, addr, val, cpu.pc, cpu.r[14], (unsigned long long)cpu.cycles);
}

void bus_write32(u32 addr, u32 val)
{
    watch_hit(addr, val, 4);
    if (addr >= EXTFLASH_BASE && addr < EXTFLASH_BASE + extflash_size) return; /* read-only */
    u8 *p = ram_ptr(addr, 4);
    if (p && (addr >> 24) != 0x08) { memcpy(p, &val, 4); return; }
    if (p) return;                                     /* internal flash: ignore */
    if (IS_SCS(addr)) { cpu_scs_write(addr - SCS_BASE, val, 4); return; }
    if (addr >= 0xE0000000u) return;
    periph_write(addr, val, 4);
}

void bus_write16(u32 addr, u32 val)
{
    watch_hit(addr, val, 2);
    if (addr >= EXTFLASH_BASE && addr < EXTFLASH_BASE + extflash_size) return;
    u8 *p = ram_ptr(addr, 2);
    if (p && (addr >> 24) != 0x08) { u16 v = (u16)val; memcpy(p, &v, 2); return; }
    if (p) return;
    if (IS_SCS(addr)) { cpu_scs_write(addr - SCS_BASE, val, 2); return; }
    if (addr >= 0xE0000000u) return;
    periph_write(addr, val, 2);
}

void bus_write8(u32 addr, u32 val)
{
    watch_hit(addr, val, 1);
    if (addr >= EXTFLASH_BASE && addr < EXTFLASH_BASE + extflash_size) return;
    u8 *p = ram_ptr(addr, 1);
    if (p && (addr >> 24) != 0x08) { *p = (u8)val; return; }
    if (p) return;
    if (IS_SCS(addr)) { cpu_scs_write(addr - SCS_BASE, val, 1); return; }
    if (addr >= 0xE0000000u) return;
    periph_write(addr, val, 1);
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

/* Always allocated at the architectural size, so a short or oversized dump
   cannot make the machine read out of bounds. */
static u8 *image_region(const void *src, u32 len, u32 size)
{
    u8 *buf = calloc(1, size);
    if (buf && src) memcpy(buf, src, len < size ? len : size);
    return buf;
}

void bus_free(void)
{
    free(mem_itcm);  free(mem_dtcm);  free(mem_axi);
    free(mem_sram1); free(mem_sram2); free(mem_srd);
    free(mem_bkp);   free(mem_flash); free(mem_extflash);
    free(extflash_plain);
    mem_itcm = mem_dtcm = mem_axi = mem_sram1 = mem_sram2 = NULL;
    mem_srd = mem_bkp = mem_flash = mem_extflash = extflash_plain = NULL;
    nperiph = 0;
    nstub = 0;
}

void bus_init_mem(const void *intflash, u32 intlen,
                  const void *extflash, u32 extlen)
{
    for (u32 i = 0; i < PLUT_SIZE; i++) plut[i] = -1;

    mem_itcm  = calloc(1, ITCM_SIZE);
    mem_dtcm  = calloc(1, DTCM_SIZE);
    mem_axi   = calloc(1, AXISRAM_SIZE);
    mem_sram1 = calloc(1, SRAM1_SIZE);
    mem_sram2 = calloc(1, SRAM2_SIZE);
    mem_srd   = calloc(1, SRD_SRAM_SIZE);
    mem_bkp   = calloc(1, BKPSRAM_SIZE);

    /* The dump size is the size of the fitted part; anything unrecognised is
       rounded up to the next power of two so the model still has room. */
    extflash_size = 1024u * 1024;
    while (extflash_size < extlen && extflash_size < EXTFLASH_MAX) extflash_size *= 2;

    mem_flash    = image_region(intflash, intlen, FLASH_SIZE);
    mem_extflash = image_region(extflash, extlen, extflash_size);
    gwlog("[bus] internal flash: %u bytes, external flash: %u bytes (%u MiB part)\n",
          intlen, extlen, extflash_size / (1024u * 1024));

    extflash_plain = calloc(1, extflash_size);
    memcpy(extflash_plain, mem_extflash, extflash_size);
}

/* ------------------------------------------------------------------ */
/* Save-state support                                                  */
/* ------------------------------------------------------------------ */

/* Internal flash is excluded: never written, so it comes back from the dump.
   The decrypted external-flash view is included because rebuilding it would
   mean replaying every OTFDEC configuration write. */
void bus_state_blocks(StateBlockFn fn, void *ctx)
{
    fn(ctx, "itcm",     mem_itcm,       ITCM_SIZE);
    fn(ctx, "dtcm",     mem_dtcm,       DTCM_SIZE);
    fn(ctx, "axisram",  mem_axi,        AXISRAM_SIZE);
    fn(ctx, "sram1",    mem_sram1,      SRAM1_SIZE);
    fn(ctx, "sram2",    mem_sram2,      SRAM2_SIZE);
    fn(ctx, "srdsram",  mem_srd,        SRD_SRAM_SIZE);
    fn(ctx, "bkpsram",  mem_bkp,        BKPSRAM_SIZE);
    fn(ctx, "extflash", mem_extflash,   extflash_size);
    fn(ctx, "extplain", extflash_plain, extflash_size);
    fn(ctx, "stub",     stub,           sizeof stub);
    fn(ctx, "stubn",    &nstub,         sizeof nstub);
}
