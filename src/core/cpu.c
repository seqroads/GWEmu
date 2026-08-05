/* cpu.c - ARMv7E-M (Cortex-M7) interpreter: Thumb-1/Thumb-2 + DSP + FPv5-D16 */

#include "gw.h"
#include <math.h>

CPU cpu;

static bool exc_dirty = true;

/* Rolling history of recently executed instructions, for post-mortem dumps. */
#define HIST_SIZE 128
static struct { u32 pc; u32 r[16]; } hist[HIST_SIZE];
static u32 hist_pos;

u32 logcall[16];
int n_logcall;

void cpu_add_logcall(u32 addr) { if (n_logcall < 16) logcall[n_logcall++] = addr; }

/* Exact PC histogram over the executable regions: which code is hot / spinning. */
static u64 *prof_itcm, *prof_flash, *prof_ext;
static u64 prof_other;

static inline void prof_hit(u32 pc)
{
    if (pc < ITCM_SIZE) { if (prof_itcm) prof_itcm[pc >> 1]++; return; }
    if (pc - FLASH_BASE < FLASH_SIZE) { if (prof_flash) prof_flash[(pc - FLASH_BASE) >> 1]++; return; }
    if (pc - EXTFLASH_BASE < extflash_size) { if (prof_ext) prof_ext[(pc - EXTFLASH_BASE) >> 1]++; return; }
    prof_other++;
}

void cpu_profile_init(void)
{
    prof_itcm  = calloc(ITCM_SIZE / 2, sizeof(u64));
    prof_flash = calloc(FLASH_SIZE / 2, sizeof(u64));
    prof_ext   = calloc(EXTFLASH_MAX / 2, sizeof(u64));
}

bool cpu_profile_active(void) { return prof_itcm != NULL; }
u64  cpu_profile_other(void)  { return prof_other; }

/* Collects the `max` hottest program counters, hottest first. */
int cpu_profile_top(ProfileEntry *out, int max)
{
    int nb = 0;
    struct { u64 *a; u32 base, n; } regions[3] = {
        { prof_itcm,  ITCM_BASE,     ITCM_SIZE / 2 },
        { prof_flash, FLASH_BASE,    FLASH_SIZE / 2 },
        { prof_ext,   EXTFLASH_BASE, extflash_size / 2 },
    };
    if (max <= 0) return 0;

    for (int r = 0; r < 3; r++) {
        if (!regions[r].a) continue;
        for (u32 i = 0; i < regions[r].n; i++) {
            u64 c = regions[r].a[i];
            if (!c) continue;
            if (nb < max) {
                out[nb].pc = regions[r].base + i * 2; out[nb].count = c; nb++;
            } else {
                int worst = 0;
                for (int k = 1; k < nb; k++) if (out[k].count < out[worst].count) worst = k;
                if (c > out[worst].count) {
                    out[worst].pc = regions[r].base + i * 2;
                    out[worst].count = c;
                }
            }
        }
    }
    for (int a = 0; a < nb; a++)
        for (int b = a + 1; b < nb; b++)
            if (out[b].count > out[a].count) {
                ProfileEntry t = out[a]; out[a] = out[b]; out[b] = t;
            }
    return nb;
}

void cpu_dump_profile(int top)
{
    ProfileEntry best[64];
    if (top > 64) top = 64;
    int nb = cpu_profile_top(best, top);
    for (int i = 0; i < nb; i++)
        gwlog("[prof] %08x  %llu\n", best[i].pc, (unsigned long long)best[i].count);
    if (prof_other) gwlog("[prof] (outside itcm/flash/extflash) %llu\n", (unsigned long long)prof_other);
}

/* Copies the execution history oldest-first into `out`. */
int cpu_history(HistEntry *out, int max)
{
    int n = 0;
    for (int i = 0; i < HIST_SIZE && n < max; i++) {
        u32 k = (hist_pos + i) % HIST_SIZE;
        if (!hist[k].pc) continue;
        out[n].pc = hist[k].pc;
        memcpy(out[n].r, hist[k].r, sizeof out[n].r);
        n++;
    }
    return n;
}

void cpu_dump_history(const char *why)
{
    gwlog("---- execution history (%s), newest last ----\n", why);
    for (int i = 0; i < HIST_SIZE; i++) {
        u32 k = (hist_pos + i) % HIST_SIZE;
        if (!hist[k].pc) continue;
        gwlog("  %08x  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n"
              "            r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x\n",
              hist[k].pc, hist[k].r[0], hist[k].r[1], hist[k].r[2], hist[k].r[3],
              hist[k].r[4], hist[k].r[5], hist[k].r[6], hist[k].r[7],
              hist[k].r[8], hist[k].r[9], hist[k].r[10], hist[k].r[11], hist[k].r[12],
              hist[k].r[13], hist[k].r[14]);
    }
    gwlog("---- end history ----\n");
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static inline u32 ror32(u32 x, unsigned n) { n &= 31; return n ? ((x >> n) | (x << (32 - n))) : x; }
static inline u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline u32 bit(u32 v, int b) { return (v >> b) & 1; }
static inline u32 sext(u32 v, int nbits) {
    u32 m = 1u << (nbits - 1);
    return (v ^ m) - m;
}

/* SP banking: cpu.r[13] is always the active stack pointer */
static inline void sp_select(bool use_psp)
{
    bool cur_psp = (cpu.control & 2) && !cpu.handler_mode;
    if (cur_psp) cpu.sp_process = cpu.r[13]; else cpu.sp_main = cpu.r[13];
    cpu.r[13] = use_psp ? cpu.sp_process : cpu.sp_main;
}

static inline void sp_sync_out(void)
{
    if ((cpu.control & 2) && !cpu.handler_mode) cpu.sp_process = cpu.r[13];
    else cpu.sp_main = cpu.r[13];
}

static inline u32 shift_c(u32 value, int type, int amount, u32 *carry)
{
    if (amount == 0) return value;
    switch (type) {
    case 0: /* LSL */
        if (amount >= 32) { *carry = amount == 32 ? (value & 1) : 0; return 0; }
        *carry = (value >> (32 - amount)) & 1;
        return value << amount;
    case 1: /* LSR */
        if (amount >= 32) { *carry = amount == 32 ? (value >> 31) : 0; return 0; }
        *carry = (value >> (amount - 1)) & 1;
        return value >> amount;
    case 2: /* ASR */
        if (amount >= 32) { *carry = value >> 31; return (u32)((s32)value >> 31); }
        *carry = (u32)((s32)value >> (amount - 1)) & 1;
        return (u32)((s32)value >> amount);
    default: /* ROR */
        amount &= 31;
        if (amount == 0) { *carry = value >> 31; return value; }
        *carry = (value >> (amount - 1)) & 1;
        return ror32(value, amount);
    }
}

/* decode_imm_shift: returns type, sets *amount */
static inline int decode_shift(int type, int imm5, int *amount)
{
    switch (type) {
    case 0: *amount = imm5; return 0;
    case 1: *amount = imm5 ? imm5 : 32; return 1;
    case 2: *amount = imm5 ? imm5 : 32; return 2;
    default:
        if (imm5 == 0) { *amount = 1; return 4; } /* RRX */
        *amount = imm5; return 3;
    }
}

static inline u32 do_shift(u32 val, int type, int amount, u32 *carry)
{
    if (type == 4) { /* RRX */
        u32 c = *carry;
        *carry = val & 1;
        return (val >> 1) | (c << 31);
    }
    return shift_c(val, type, amount, carry);
}

static inline u32 thumb_expand_imm_c(u32 imm12, u32 *carry)
{
    if (bits(imm12, 11, 10) == 0) {
        u32 b = bits(imm12, 7, 0);
        switch (bits(imm12, 9, 8)) {
        case 0: return b;
        case 1: return (b << 16) | b;
        case 2: return (b << 24) | (b << 8);
        default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    } else {
        u32 v = 0x80u | bits(imm12, 6, 0);
        u32 rot = bits(imm12, 11, 7);
        u32 r = ror32(v, rot);
        *carry = r >> 31;
        return r;
    }
}

static inline u32 add_with_carry(u32 x, u32 y, u32 cin, u32 *co, u32 *vo)
{
    u64 usum = (u64)x + (u64)y + cin;
    s64 ssum = (s64)(s32)x + (s64)(s32)y + cin;
    u32 res = (u32)usum;
    *co = (usum >> 32) & 1;
    *vo = ((s64)(s32)res != ssum);
    return res;
}

static inline void set_nz(u32 r) { cpu.n = r >> 31; cpu.z = (r == 0); }

static bool cond_passed(int cond)
{
    bool r;
    switch (cond >> 1) {
    case 0: r = cpu.z; break;
    case 1: r = cpu.c; break;
    case 2: r = cpu.n; break;
    case 3: r = cpu.v; break;
    case 4: r = cpu.c && !cpu.z; break;
    case 5: r = (cpu.n == cpu.v); break;
    case 6: r = (cpu.n == cpu.v) && !cpu.z; break;
    default: return true;
    }
    return (cond & 1) ? !r : r;
}

static inline void it_advance(void)
{
    if ((cpu.itstate & 7) == 0) cpu.itstate = 0;
    else cpu.itstate = (cpu.itstate & 0xE0) | ((cpu.itstate << 1) & 0x1F);
}

static inline bool in_it_block(void) { return (cpu.itstate & 0x0F) != 0; }

static inline void branch_to(u32 addr)
{
    cpu.pc = addr & ~1u;
    cpu.pc_changed = true;
}

static void exception_return(u32 exc_return);

static inline void bx_write_pc(u32 addr)
{
    if (cpu.handler_mode && (addr >> 28) == 0xF) { exception_return(addr); return; }
    if (!(addr & 1)) {
        gwlog("[cpu] BX to non-thumb address %08x from pc=%08x\n", addr, cpu.pc);
    }
    branch_to(addr);
}

/* ------------------------------------------------------------------ */
/* saturation helpers                                                  */
/* ------------------------------------------------------------------ */
static inline u32 signed_sat(s64 v, int n, int *sat)
{
    s64 max = (1LL << (n - 1)) - 1, min = -(1LL << (n - 1));
    if (v > max) { *sat = 1; return (u32)max; }
    if (v < min) { *sat = 1; return (u32)min; }
    return (u32)v;
}
static inline u32 unsigned_sat(s64 v, int n, int *sat)
{
    s64 max = (n >= 32) ? 0xFFFFFFFFLL : ((1LL << n) - 1);
    if (v > max) { *sat = 1; return (u32)max; }
    if (v < 0) { *sat = 1; return 0; }
    return (u32)v;
}

/* ------------------------------------------------------------------ */
/* exceptions                                                          */
/* ------------------------------------------------------------------ */

const char *cpu_exc_name(int exc)
{
    static char buf[32];
    switch (exc) {
    case EXC_NMI: return "NMI";
    case EXC_HARDFAULT: return "HardFault";
    case EXC_MEMMANAGE: return "MemManage";
    case EXC_BUSFAULT: return "BusFault";
    case EXC_USAGEFAULT: return "UsageFault";
    case EXC_SVCALL: return "SVCall";
    case EXC_PENDSV: return "PendSV";
    case EXC_SYSTICK: return "SysTick";
    default: snprintf(buf, sizeof buf, "IRQ%d", exc - 16); return buf;
    }
}

static inline int exc_priority(int e)
{
    switch (e) {
    case EXC_RESET: return -3;
    case EXC_NMI: return -2;
    case EXC_HARDFAULT: return -1;
    default: return cpu.exc_prio[e];
    }
}

static int execution_priority(void)
{
    int p = 256;
    for (int e = 1; e < NUM_EXC; e++)
        if (cpu.exc_active[e]) { int q = exc_priority(e); if (q < p) p = q; }
    if (cpu.primask && p > 0) p = 0;
    if (cpu.faultmask && p > -1) p = -1;
    if (cpu.basepri && p > cpu.basepri) p = cpu.basepri;
    return p;
}

/* Highest-priority pending exception that can preempt, or 0 */
static int pending_exception(void)
{
    int best = 0, bestp = execution_priority();
    for (int e = 1; e < NUM_EXC; e++) {
        if (!cpu.exc_pending[e]) continue;
        if (e >= EXC_EXTERNAL && !cpu.exc_enabled[e]) continue;
        if (e == EXC_MEMMANAGE && !(cpu.shcsr & (1u << 16))) continue;
        if (e == EXC_BUSFAULT && !(cpu.shcsr & (1u << 17))) continue;
        if (e == EXC_USAGEFAULT && !(cpu.shcsr & (1u << 18))) continue;
        int p = exc_priority(e);
        if (p < bestp) { bestp = p; best = e; }
    }
    return best;
}

static bool fpu_enabled(void)
{
    return ((cpu.cpacr >> 20) & 0xF) != 0;
}

static bool last_push_had_fp = false;

static void push_stack(int exc)
{
    (void)exc;
    bool fp = (cpu.control & 4) && fpu_enabled();
    last_push_had_fp = fp;
    u32 framesize = fp ? 0x68 : 0x20;
    u32 spm = cpu.r[13] - framesize;
    u32 align = 0;
    if (cpu.ccr & 0x200) { align = (spm >> 2) & 1; spm &= ~7u; }
    u32 fp_ptr = spm;

    bus_write32(fp_ptr + 0x00, cpu.r[0]);
    bus_write32(fp_ptr + 0x04, cpu.r[1]);
    bus_write32(fp_ptr + 0x08, cpu.r[2]);
    bus_write32(fp_ptr + 0x0C, cpu.r[3]);
    bus_write32(fp_ptr + 0x10, cpu.r[12]);
    bus_write32(fp_ptr + 0x14, cpu.r[14]);
    bus_write32(fp_ptr + 0x18, cpu.pc);
    u32 xpsr = (cpu.n << 31) | (cpu.z << 30) | (cpu.c << 29) | (cpu.v << 28) |
               (cpu.q << 27) | (align << 9) | cpu.ipsr |
               ((cpu.itstate & 0x3) << 25) | ((cpu.itstate & 0xFC) << 8);
    bus_write32(fp_ptr + 0x1C, xpsr);

    if (fp) {
        for (int i = 0; i < 16; i++) bus_write32(fp_ptr + 0x20 + i * 4, cpu.s[i]);
        bus_write32(fp_ptr + 0x60, cpu.fpscr);
        bus_write32(fp_ptr + 0x64, 0);
        cpu.control &= ~4u;
    }
    cpu.r[13] = fp_ptr;
}

u64 exc_count[NUM_EXC];

void cpu_dump_exceptions(void)
{
    gwlog("---- exceptions taken ----\n");
    for (int e = 1; e < NUM_EXC; e++)
        if (exc_count[e]) gwlog("  %-12s %llu\n", cpu_exc_name(e), (unsigned long long)exc_count[e]);
    gwlog("---- end exceptions ----\n");
}

static void exception_taken(int exc)
{
    exc_count[exc]++;
    sp_sync_out();
    push_stack(exc);

    /* EXC_RETURN: bit4 = 0 when an extended (FP) frame was pushed */
    u32 lr;
    if (cpu.handler_mode) lr = 0xFFFFFFF1u;
    else lr = (cpu.control & 2) ? 0xFFFFFFFDu : 0xFFFFFFF9u;
    if (last_push_had_fp) lr &= ~0x10u;

    cpu.r[14] = lr;
    cpu.handler_mode = true;
    cpu.ipsr = exc;
    cpu.control &= ~2u;      /* use MSP */
    cpu.sp_main = cpu.r[13];
    cpu.itstate = 0;
    cpu.exc_active[exc] = 1;
    cpu.exc_pending[exc] = 0;
    exc_dirty = true;

    u32 vec = bus_read32(cpu.vtor + exc * 4);
    if (vec == 0 || vec == 0xFFFFFFFFu) {
        gwfatal("null vector for exception %d (%s) vtor=%08x", exc, cpu_exc_name(exc), cpu.vtor);
        return;
    }
    cpu.pc = vec & ~1u;
    cpu.pc_changed = true;
    cpu.sleeping = false;
}

static void exception_return(u32 exc_return)
{
    int returning = cpu.ipsr;
    if (returning <= 0 || returning >= NUM_EXC) {
        gwfatal("exception return with bogus IPSR %d", returning);
        return;
    }
    cpu.exc_active[returning] = 0;
    exc_dirty = true;

    bool ret_to_thread = (exc_return & 0xF) != 0x1;
    bool use_psp = (exc_return & 0x4) != 0;
    bool fp_frame = (exc_return & 0x10) == 0;

    /* select the stack we pop from */
    cpu.sp_main = cpu.r[13];
    u32 sp = (ret_to_thread && use_psp) ? cpu.sp_process : cpu.sp_main;

    u32 r0 = bus_read32(sp + 0x00), r1 = bus_read32(sp + 0x04);
    u32 r2 = bus_read32(sp + 0x08), r3 = bus_read32(sp + 0x0C);
    u32 r12 = bus_read32(sp + 0x10), lr = bus_read32(sp + 0x14);
    u32 pc = bus_read32(sp + 0x18), xpsr = bus_read32(sp + 0x1C);
    u32 framesize = 0x20;
    if (fp_frame) {
        if (fpu_enabled()) {
            for (int i = 0; i < 16; i++) cpu.s[i] = bus_read32(sp + 0x20 + i * 4);
            cpu.fpscr = bus_read32(sp + 0x60);
        }
        framesize = 0x68;
        cpu.control |= 4;
    }
    sp += framesize;
    if ((cpu.ccr & 0x200) && (xpsr & (1u << 9))) sp += 4;

    cpu.r[0] = r0; cpu.r[1] = r1; cpu.r[2] = r2; cpu.r[3] = r3;
    cpu.r[12] = r12; cpu.r[14] = lr;

    cpu.n = (xpsr >> 31) & 1; cpu.z = (xpsr >> 30) & 1;
    cpu.c = (xpsr >> 29) & 1; cpu.v = (xpsr >> 28) & 1;
    cpu.q = (xpsr >> 27) & 1;
    cpu.itstate = (u8)(((xpsr >> 25) & 3) | ((xpsr >> 8) & 0xFC));
    cpu.itstate_written = true;

    if (ret_to_thread) {
        cpu.handler_mode = false;
        cpu.ipsr = 0;
        if (use_psp) { cpu.control |= 2; cpu.sp_process = sp; }
        else { cpu.control &= ~2u; cpu.sp_main = sp; }
    } else {
        cpu.handler_mode = true;
        cpu.ipsr = (xpsr & 0x1FF);
        cpu.control &= ~2u;
        cpu.sp_main = sp;
    }
    cpu.r[13] = ((cpu.control & 2) && !cpu.handler_mode) ? cpu.sp_process : cpu.sp_main;
    cpu.pc = pc & ~1u;
    cpu.pc_changed = true;
}

void cpu_check_exceptions(void)
{
    if (!exc_dirty) return;
    exc_dirty = false;
    int e = pending_exception();
    if (e) exception_taken(e);
}

void cpu_set_pending(int exc)
{
    if (exc <= 0 || exc >= NUM_EXC) return;
    cpu.exc_pending[exc] = 1;
    exc_dirty = true;
}

void cpu_raise_irq(int irq, bool level)
{
    if (irq < 0 || irq >= NUM_IRQ) return;
    if (level) { cpu.exc_pending[EXC_EXTERNAL + irq] = 1; exc_dirty = true; }
}

static void fault(int exc, const char *why)
{
    gwlog("[cpu] fault %s (%s) at pc=%08x\n", cpu_exc_name(exc), why, cpu.pc);
    /* escalate to HardFault if the specific handler is disabled */
    if (exc == EXC_MEMMANAGE || exc == EXC_BUSFAULT || exc == EXC_USAGEFAULT) {
        int en = (exc == EXC_MEMMANAGE) ? 16 : (exc == EXC_BUSFAULT) ? 17 : 18;
        if (!(cpu.shcsr & (1u << en))) { cpu.hfsr |= (1u << 30); exc = EXC_HARDFAULT; }
    }
    cpu.exc_pending[exc] = 1;
    exc_dirty = true;
    cpu_check_exceptions();
}

/* ------------------------------------------------------------------ */
/* SysTick                                                             */
/* ------------------------------------------------------------------ */
void cpu_tick_systick(u32 cycles)
{
    if (!(cpu.systick_ctrl & 1)) return;
    u32 reload = cpu.systick_load & 0xFFFFFF;
    if (reload == 0) return;
    u32 v = cpu.systick_val;
    while (cycles) {
        u32 step = (v < cycles) ? v : cycles;
        if (step == 0) {
            v = reload;
            cpu.systick_ctrl |= (1u << 16);
            if (cpu.systick_ctrl & 2) cpu_set_pending(EXC_SYSTICK);
            continue;
        }
        v -= step;
        cycles -= step;
        if (v == 0) {
            v = reload;
            cpu.systick_ctrl |= (1u << 16);
            if (cpu.systick_ctrl & 2) cpu_set_pending(EXC_SYSTICK);
        }
    }
    cpu.systick_val = v;
}

/* ------------------------------------------------------------------ */
/* SCB / NVIC register block (0xE000E000)                              */
/* ------------------------------------------------------------------ */

static u32 scs_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x010: return cpu.systick_ctrl | 0x4;   /* CLKSOURCE = core */
    case 0x014: return cpu.systick_load;
    case 0x018: return cpu.systick_val;
    case 0x01C: return cpu.systick_calib;
    case 0xD00: return 0x411FC271;               /* CPUID: Cortex-M7 r1p1 */
    case 0xD04: {
        u32 v = cpu.ipsr;
        int p = pending_exception();
        if (p) v |= (u32)p << 12;
        if (cpu.exc_pending[EXC_PENDSV]) v |= 1u << 28;
        if (cpu.exc_pending[EXC_SYSTICK]) v |= 1u << 26;
        return v;
    }
    case 0xD08: return cpu.vtor;
    case 0xD0C: return 0xFA050000 | (cpu.aircr_prigroup << 8);
    case 0xD10: return cpu.scr;
    case 0xD14: return cpu.ccr;
    case 0xD24: return cpu.shcsr;
    case 0xD28: return cpu.cfsr;
    case 0xD2C: return cpu.hfsr;
    case 0xD34: return cpu.mmfar;
    case 0xD38: return cpu.bfar;
    case 0xD88: return cpu.cpacr;
    case 0xD90: return 0;                        /* MPU_TYPE: no MPU regions */
    case 0xF34: return cpu.fpccr;
    case 0xF38: return cpu.fpcar;
    case 0xF3C: return cpu.fpdscr;
    case 0x008: return cpu.actlr;
    }
    if (off >= 0xD18 && off < 0xD24) {           /* SHPR1..3 */
        u32 base = (off - 0xD18) + 4;            /* exception 4.. */
        u32 v = 0;
        for (int i = 0; i < 4; i++) v |= (u32)cpu.exc_prio[base + i] << (i * 8);
        return v;
    }
    if (off >= 0x100 && off < 0x120) {           /* NVIC_ISER */
        u32 idx = (off - 0x100) / 4, v = 0;
        for (int i = 0; i < 32; i++)
            if (cpu.exc_enabled[EXC_EXTERNAL + idx * 32 + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x180 && off < 0x1A0) {           /* NVIC_ICER */
        u32 idx = (off - 0x180) / 4, v = 0;
        for (int i = 0; i < 32; i++)
            if (cpu.exc_enabled[EXC_EXTERNAL + idx * 32 + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x200 && off < 0x220) {           /* NVIC_ISPR */
        u32 idx = (off - 0x200) / 4, v = 0;
        for (int i = 0; i < 32; i++)
            if (cpu.exc_pending[EXC_EXTERNAL + idx * 32 + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x280 && off < 0x2A0) {
        u32 idx = (off - 0x280) / 4, v = 0;
        for (int i = 0; i < 32; i++)
            if (cpu.exc_pending[EXC_EXTERNAL + idx * 32 + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x300 && off < 0x320) {           /* NVIC_IABR */
        u32 idx = (off - 0x300) / 4, v = 0;
        for (int i = 0; i < 32; i++)
            if (cpu.exc_active[EXC_EXTERNAL + idx * 32 + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x400 && off < 0x400 + NUM_IRQ) { /* NVIC_IPR (byte addressed) */
        u32 irq = off - 0x400;
        u32 v = 0;
        for (int i = 0; i < 4 && irq + i < NUM_IRQ; i++)
            v |= (u32)cpu.exc_prio[EXC_EXTERNAL + irq + i] << (i * 8);
        return v;
    }
    return 0;
}

static void scs_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    case 0x008: cpu.actlr = val; return;
    case 0x010: cpu.systick_ctrl = val & 7; return;
    case 0x014: cpu.systick_load = val & 0xFFFFFF; return;
    case 0x018: cpu.systick_val = 0; cpu.systick_ctrl &= ~(1u << 16); return;
    case 0xD04:
        if (val & (1u << 31)) cpu_set_pending(EXC_NMI);
        if (val & (1u << 28)) cpu_set_pending(EXC_PENDSV);
        if (val & (1u << 27)) { cpu.exc_pending[EXC_PENDSV] = 0; exc_dirty = true; }
        if (val & (1u << 26)) cpu_set_pending(EXC_SYSTICK);
        if (val & (1u << 25)) { cpu.exc_pending[EXC_SYSTICK] = 0; exc_dirty = true; }
        return;
    case 0xD08: cpu.vtor = val & ~0x7Fu; return;
    case 0xD0C:
        if ((val >> 16) != 0x05FA) return;
        cpu.aircr_prigroup = (val >> 8) & 7;
        if (val & 4) {                    /* SYSRESETREQ: reboot, do not stop */
            gwlog("[cpu] SYSRESETREQ\n");
            cpu.pending_sysreset = true;
        }
        return;
    case 0xD10: cpu.scr = val; return;
    case 0xD14: cpu.ccr = val; return;
    case 0xD24: cpu.shcsr = val; exc_dirty = true; return;
    case 0xD28: cpu.cfsr &= ~val; return;
    case 0xD2C: cpu.hfsr &= ~val; return;
    case 0xD88: cpu.cpacr = val; return;
    case 0xF34: cpu.fpccr = val; return;
    case 0xF38: cpu.fpcar = val; return;
    case 0xF3C: cpu.fpdscr = val; return;
    case 0xF50: case 0xF54: case 0xF58: case 0xF5C:
    case 0xF60: case 0xF64: case 0xF68: case 0xF6C:
    case 0xF70: case 0xF74: case 0xF78: case 0xF7C:
        return;                                   /* cache maintenance: no-op */
    }
    if (off >= 0xD18 && off < 0xD24) {
        u32 base = (off - 0xD18) + 4;
        for (int i = 0; i < 4; i++) cpu.exc_prio[base + i] = (val >> (i * 8)) & 0xFF;
        exc_dirty = true;
        return;
    }
    if (off >= 0x100 && off < 0x120) {
        u32 idx = (off - 0x100) / 4;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) cpu.exc_enabled[EXC_EXTERNAL + idx * 32 + i] = 1;
        exc_dirty = true; return;
    }
    if (off >= 0x180 && off < 0x1A0) {
        u32 idx = (off - 0x180) / 4;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) cpu.exc_enabled[EXC_EXTERNAL + idx * 32 + i] = 0;
        exc_dirty = true; return;
    }
    if (off >= 0x200 && off < 0x220) {
        u32 idx = (off - 0x200) / 4;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) cpu.exc_pending[EXC_EXTERNAL + idx * 32 + i] = 1;
        exc_dirty = true; return;
    }
    if (off >= 0x280 && off < 0x2A0) {
        u32 idx = (off - 0x280) / 4;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) cpu.exc_pending[EXC_EXTERNAL + idx * 32 + i] = 0;
        exc_dirty = true; return;
    }
    if (off >= 0x400 && off < 0x400 + NUM_IRQ) {
        u32 irq = off - 0x400;
        for (int i = 0; i < 4 && irq + i < NUM_IRQ; i++)
            cpu.exc_prio[EXC_EXTERNAL + irq + i] = (val >> (i * 8)) & 0xFF;
        exc_dirty = true;
        return;
    }
}

u32 cpu_scs_read(u32 off, int size) { return scs_read(off, size); }
void cpu_scs_write(u32 off, u32 val, int size) { scs_write(off, val, size); }

/* ------------------------------------------------------------------ */
/* FPU                                                                 */
/* ------------------------------------------------------------------ */

static inline double getd(int n) {
    u64 v = ((u64)cpu.s[n * 2 + 1] << 32) | cpu.s[n * 2];
    double d; memcpy(&d, &v, 8); return d;
}
static inline void setd(int n, double d) {
    u64 v; memcpy(&v, &d, 8);
    cpu.s[n * 2] = (u32)v; cpu.s[n * 2 + 1] = (u32)(v >> 32);
}
static inline float getf(int n) { float f; memcpy(&f, &cpu.s[n], 4); return f; }
static inline void setf(int n, float f) { memcpy(&cpu.s[n], &f, 4); }

static void fp_compare(double a, double b, bool quiet)
{
    (void)quiet;
    if (isnan(a) || isnan(b)) { cpu.n = 0; cpu.z = 0; cpu.c = 1; cpu.v = 1; }
    else if (a == b) { cpu.n = 0; cpu.z = 1; cpu.c = 1; cpu.v = 0; }
    else if (a < b) { cpu.n = 1; cpu.z = 0; cpu.c = 0; cpu.v = 0; }
    else { cpu.n = 0; cpu.z = 0; cpu.c = 1; cpu.v = 0; }
}

/* Returns true if handled */
static bool exec_vfp(u32 hw1, u32 hw2)
{
    u32 coproc = bits(hw2, 11, 8);
    if (coproc != 10 && coproc != 11) {
        gwlog("[cpu] unhandled coproc %u insn %04x %04x at %08x\n", coproc, hw1, hw2, cpu.pc);
        return false;
    }
    bool dp = (coproc == 11);           /* 11 = double precision (D registers) */
    u32 t = bits(hw1, 11, 9);

    /* ---- 64-bit transfers: VMOV core pair <-> S pair / D ---- */
    if (t == 6 && bits(hw1, 8, 5) == 0x2) {
        bool to_arm = bit(hw1, 4);
        int rt = bits(hw2, 15, 12), rt2 = bits(hw1, 3, 0);
        if (dp) {
            int m = bits(hw2, 3, 0) | (bit(hw2, 5) << 4);
            if (to_arm) { cpu.r[rt] = cpu.s[m * 2]; cpu.r[rt2] = cpu.s[m * 2 + 1]; }
            else { cpu.s[m * 2] = cpu.r[rt]; cpu.s[m * 2 + 1] = cpu.r[rt2]; }
        } else {
            int m = (bits(hw2, 3, 0) << 1) | bit(hw2, 5);
            if (to_arm) { cpu.r[rt] = cpu.s[m]; cpu.r[rt2] = cpu.s[m + 1]; }
            else { cpu.s[m] = cpu.r[rt]; cpu.s[m + 1] = cpu.r[rt2]; }
        }
        return true;
    }

    /* ---- extension register load/store: VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP ---- */
    if (t == 6) {
        bool P = bit(hw1, 8), U = bit(hw1, 7), W = bit(hw1, 5), L = bit(hw1, 4);
        u32 D = bit(hw1, 6);
        int rn = bits(hw1, 3, 0);
        int vd = bits(hw2, 15, 12);
        u32 imm8 = bits(hw2, 7, 0);
        int d = dp ? (vd | (D << 4)) : ((vd << 1) | D);

        if (P && !W) {                                  /* VLDR / VSTR */
            u32 base = (rn == 15) ? ((cpu.pc + 4) & ~3u) : cpu.r[rn];
            u32 addr = U ? base + imm8 * 4 : base - imm8 * 4;
            if (dp) {
                if (L) { cpu.s[d * 2] = bus_read32(addr); cpu.s[d * 2 + 1] = bus_read32(addr + 4); }
                else { bus_write32(addr, cpu.s[d * 2]); bus_write32(addr + 4, cpu.s[d * 2 + 1]); }
            } else {
                if (L) cpu.s[d] = bus_read32(addr);
                else bus_write32(addr, cpu.s[d]);
            }
            return true;
        }
        /* VLDM / VSTM / VPUSH / VPOP */
        u32 nregs = dp ? imm8 / 2 : imm8;
        u32 base = cpu.r[rn];
        u32 addr = U ? base : base - imm8 * 4;
        for (u32 i = 0; i < nregs; i++) {
            int reg = d + (int)i;
            if (dp) {
                if (L) { cpu.s[reg * 2] = bus_read32(addr); cpu.s[reg * 2 + 1] = bus_read32(addr + 4); }
                else { bus_write32(addr, cpu.s[reg * 2]); bus_write32(addr + 4, cpu.s[reg * 2 + 1]); }
                addr += 8;
            } else {
                if (L) cpu.s[reg] = bus_read32(addr);
                else bus_write32(addr, cpu.s[reg]);
                addr += 4;
            }
        }
        if (W) cpu.r[rn] = U ? base + imm8 * 4 : base - imm8 * 4;
        return true;
    }

    if (t != 7 || bit(hw1, 8) != 0) {
        gwlog("[cpu] unhandled coproc form %04x %04x at %08x\n", hw1, hw2, cpu.pc);
        return false;
    }

    /* ---- 8/16/32-bit transfer between core register and FP ---- */
    if (bit(hw2, 4)) {
        u32 L = bit(hw1, 4);
        u32 A = bits(hw1, 7, 5);
        int rt = bits(hw2, 15, 12);
        if (A == 0) {                                   /* VMOV core <-> single */
            int n = (bits(hw1, 3, 0) << 1) | bit(hw2, 7);
            if (L) cpu.r[rt] = cpu.s[n];
            else cpu.s[n] = cpu.r[rt];
            return true;
        }
        if (A == 7) {                                   /* VMSR / VMRS */
            u32 reg = bits(hw1, 3, 0);
            if (L) {
                if (reg == 1) {                         /* FPSCR */
                    if (rt == 15) {
                        cpu.n = (cpu.fpscr >> 31) & 1; cpu.z = (cpu.fpscr >> 30) & 1;
                        cpu.c = (cpu.fpscr >> 29) & 1; cpu.v = (cpu.fpscr >> 28) & 1;
                    } else cpu.r[rt] = cpu.fpscr;
                } else cpu.r[rt] = 0;
            } else {
                if (reg == 1) cpu.fpscr = cpu.r[rt];
            }
            return true;
        }
        gwlog("[cpu] unhandled VFP transfer A=%u at %08x\n", A, cpu.pc);
        return false;
    }

    /* ---- floating-point data processing ---- */
    {
        u32 opc1 = bits(hw1, 7, 4);        /* includes D as bit 2 */
        u32 D = bit(hw1, 6);
        u32 opc2 = bits(hw1, 3, 0);
        u32 vd = bits(hw2, 15, 12);
        u32 N = bit(hw2, 7), M = bit(hw2, 5);
        u32 opc3 = bits(hw2, 7, 6);
        u32 vn = bits(hw1, 3, 0), vm = bits(hw2, 3, 0);

        int d = dp ? (vd | (D << 4)) : ((vd << 1) | D);
        int n = dp ? (vn | (N << 4)) : ((vn << 1) | N);
        int m = dp ? (vm | (M << 4)) : ((vm << 1) | M);
        int sm = (vm << 1) | M;            /* single-precision view of Vm */
        int sd = (vd << 1) | D;            /* single-precision view of Vd */
        u32 o1 = opc1 & 0xB;

        switch (o1) {
        case 0x0: /* VMLA / VMLS */
            if (dp) setd(d, getd(d) + (bit(hw2, 6) ? -1.0 : 1.0) * (getd(n) * getd(m)));
            else setf(d, getf(d) + (bit(hw2, 6) ? -1.0f : 1.0f) * (getf(n) * getf(m)));
            return true;
        case 0x1: /* VNMLA / VNMLS */
            if (dp) setd(d, -getd(d) + (bit(hw2, 6) ? 1.0 : -1.0) * (getd(n) * getd(m)));
            else setf(d, -getf(d) + (bit(hw2, 6) ? 1.0f : -1.0f) * (getf(n) * getf(m)));
            return true;
        case 0x2: /* VMUL / VNMUL */
            if (bit(hw2, 6)) { if (dp) setd(d, -(getd(n) * getd(m))); else setf(d, -(getf(n) * getf(m))); }
            else { if (dp) setd(d, getd(n) * getd(m)); else setf(d, getf(n) * getf(m)); }
            return true;
        case 0x3: /* VADD / VSUB */
            if (bit(hw2, 6)) { if (dp) setd(d, getd(n) - getd(m)); else setf(d, getf(n) - getf(m)); }
            else { if (dp) setd(d, getd(n) + getd(m)); else setf(d, getf(n) + getf(m)); }
            return true;
        case 0x8: /* VDIV */
            if ((opc3 & 1) == 0) {
                if (dp) setd(d, getd(n) / getd(m)); else setf(d, getf(n) / getf(m));
                return true;
            }
            break;
        case 0x9: case 0xA: { /* VFNMA/VFNMS (0x9), VFMA/VFMS (0xA) */
            bool negate_product = bit(hw2, 6);
            bool negate_acc = (o1 == 0x9);
            if (dp) {
                double p = getd(n) * getd(m);
                if (negate_product) p = -p;
                double acc = negate_acc ? -getd(d) : getd(d);
                setd(d, acc + p);
            } else {
                float p = getf(n) * getf(m);
                if (negate_product) p = -p;
                float acc = negate_acc ? -getf(d) : getf(d);
                setf(d, acc + p);
            }
            return true;
        }
        case 0xB:
            if ((opc3 & 1) == 0) {                      /* VMOV (immediate) */
                u32 imm8 = (bits(hw1, 3, 0) << 4) | bits(hw2, 3, 0);
                if (dp) {
                    u64 v = ((u64)bit(imm8, 7) << 63) |
                            ((u64)(bit(imm8, 6) ? 0 : 1) << 62) |
                            ((u64)(bit(imm8, 6) ? 0xFF : 0) << 54) |
                            ((u64)bits(imm8, 5, 0) << 48);
                    double dv; memcpy(&dv, &v, 8); setd(d, dv);
                } else {
                    cpu.s[d] = (bit(imm8, 7) << 31) | ((bit(imm8, 6) ? 0u : 1u) << 30) |
                               ((bit(imm8, 6) ? 0x1Fu : 0u) << 25) | (bits(imm8, 5, 0) << 19);
                }
                return true;
            }
            switch (opc2) {
            case 0x0:
                if (opc3 == 1) { if (dp) setd(d, getd(m)); else cpu.s[d] = cpu.s[m]; }  /* VMOV reg */
                else { if (dp) setd(d, fabs(getd(m))); else setf(d, fabsf(getf(m))); }  /* VABS */
                return true;
            case 0x1:
                if (opc3 == 1) { if (dp) setd(d, -getd(m)); else setf(d, -getf(m)); }   /* VNEG */
                else { if (dp) setd(d, sqrt(getd(m))); else setf(d, sqrtf(getf(m))); }  /* VSQRT */
                return true;
            case 0x4: case 0x5: {                                                       /* VCMP / VCMPE */
                double a = dp ? getd(d) : (double)getf(d);
                double b = (opc2 == 5) ? 0.0 : (dp ? getd(m) : (double)getf(m));
                fp_compare(a, b, opc3 == 1);
                cpu.fpscr = (cpu.fpscr & 0x0FFFFFFF) |
                            ((u32)cpu.n << 31) | ((u32)cpu.z << 30) |
                            ((u32)cpu.c << 29) | ((u32)cpu.v << 28);
                return true;
            }
            case 0x7:                                                                   /* VCVT f32<->f64 */
                if (dp) setf(sd, (float)getd(m));       /* .F32.F64: Sd <- Dm */
                else setd(vd | (D << 4), (double)getf(m)); /* .F64.F32: Dd <- Sm */
                return true;
            case 0x8: {                                                                 /* VCVT int -> fp */
                bool is_signed = bit(hw2, 7);
                s32 sv = (s32)cpu.s[sm];
                u32 uv = cpu.s[sm];
                if (dp) setd(d, is_signed ? (double)sv : (double)uv);
                else setf(d, is_signed ? (float)sv : (float)uv);
                return true;
            }
            case 0xC: case 0xD: {                                                       /* VCVT fp -> int */
                bool is_signed = (opc2 == 0xD);
                double v = dp ? getd(m) : (double)getf(m);
                double r = bit(hw2, 7) ? trunc(v) : nearbyint(v);
                if (isnan(v)) r = 0;
                if (is_signed)
                    cpu.s[sd] = (u32)(s32)(r < -2147483648.0 ? -2147483648.0
                                          : (r > 2147483647.0 ? 2147483647.0 : r));
                else
                    cpu.s[sd] = (u32)(r < 0 ? 0.0 : (r > 4294967295.0 ? 4294967295.0 : r));
                return true;
            }

            /* VCVT between floating-point and fixed-point. opc2 encodes the
               direction in bit 2 and the signedness in bit 0; Vd is both the
               source and the destination. The number of fraction bits is what
               is left of the container after the immediate. */
            case 0xA: case 0xB: case 0xE: case 0xF: {
                bool to_fixed  = (opc2 & 0x4) != 0;
                bool is_signed = (opc2 & 0x1) == 0;
                bool wide      = bit(hw2, 7);                  /* 32-bit, else 16 */
                u32  size      = wide ? 32 : 16;
                u32  imm       = (bits(hw2, 3, 0) << 1) | bit(hw2, 5);
                int  frac      = (int)(size - imm);
                double scale   = ldexp(1.0, frac);

                /* The fixed-point value occupies the low bits of the same
                   register the float came out of. */
                if (to_fixed) {
                    double v = trunc((dp ? getd(d) : (double)getf(d)) * scale);
                    if (isnan(v)) v = 0;
                    double lo = is_signed ? -ldexp(1.0, (int)size - 1) : 0.0;
                    double hi = ldexp(1.0, (int)size - (is_signed ? 1 : 0)) - 1.0;
                    if (v < lo) v = lo;
                    if (v > hi) v = hi;

                    u32 r = is_signed ? (u32)(s32)v : (u32)v;
                    if (!wide) r &= 0xFFFF;
                    if (dp) { cpu.s[d * 2] = r; cpu.s[d * 2 + 1] = 0; }
                    else    cpu.s[sd] = r;
                } else {
                    u32 raw = dp ? cpu.s[d * 2] : cpu.s[sd];
                    double v;
                    if (wide) v = is_signed ? (double)(s32)raw : (double)raw;
                    else      v = is_signed ? (double)(s16)(raw & 0xFFFF)
                                            : (double)(u16)(raw & 0xFFFF);
                    v /= scale;
                    if (dp) setd(d, v); else setf(d, (float)v);
                }
                return true;
            }
            }
            break;
        }
        gwlog("[cpu] unhandled VFP dp opc1=%x opc2=%x opc3=%x at %08x\n", opc1, opc2, opc3, cpu.pc);
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* 16-bit instruction execution                                        */
/* ------------------------------------------------------------------ */

static void exec_thumb16(u16 insn)
{
    u32 op = insn >> 10;

    /* --- shift (imm), add, sub, mov, cmp (00xxxx) --- */
    if ((insn >> 14) == 0) {
        u32 top5 = insn >> 11;
        u32 co, vo;
        if (top5 < 3) {                      /* LSL/LSR/ASR immediate */
            int rd = insn & 7, rm = (insn >> 3) & 7;
            int imm5 = bits(insn, 10, 6);
            int amount; int st = decode_shift((int)top5, imm5, &amount);
            u32 carry = cpu.c;
            u32 res = do_shift(cpu.r[rm], st, amount, &carry);
            cpu.r[rd] = res;
            if (!in_it_block()) { set_nz(res); cpu.c = carry; }
            return;
        }
        if (top5 == 3) {                     /* ADD/SUB register or imm3 */
            int rd = insn & 7, rn = (insn >> 3) & 7;
            bool sub = bit(insn, 9), useimm = bit(insn, 10);
            u32 operand = useimm ? bits(insn, 8, 6) : cpu.r[bits(insn, 8, 6)];
            u32 res = sub ? add_with_carry(cpu.r[rn], ~operand, 1, &co, &vo)
                          : add_with_carry(cpu.r[rn], operand, 0, &co, &vo);
            cpu.r[rd] = res;
            if (!in_it_block()) { set_nz(res); cpu.c = co; cpu.v = vo; }
            return;
        }
        /* 001xx: MOV/CMP/ADD/SUB immediate 8-bit */
        {
            int rdn = bits(insn, 10, 8);
            u32 imm = insn & 0xFF;
            switch (top5 & 3) {
            case 0:
                cpu.r[rdn] = imm;
                if (!in_it_block()) set_nz(imm);
                return;
            case 1: {
                u32 res = add_with_carry(cpu.r[rdn], ~imm, 1, &co, &vo);
                set_nz(res); cpu.c = co; cpu.v = vo;
                return;
            }
            case 2: {
                u32 res = add_with_carry(cpu.r[rdn], imm, 0, &co, &vo);
                cpu.r[rdn] = res;
                if (!in_it_block()) { set_nz(res); cpu.c = co; cpu.v = vo; }
                return;
            }
            default: {
                u32 res = add_with_carry(cpu.r[rdn], ~imm, 1, &co, &vo);
                cpu.r[rdn] = res;
                if (!in_it_block()) { set_nz(res); cpu.c = co; cpu.v = vo; }
                return;
            }
            }
        }
    }

    /* --- data processing (register) 010000 --- */
    if (op == 0x10) {
        int rdn = insn & 7, rm = (insn >> 3) & 7;
        u32 co = cpu.c, vo = cpu.v, res;
        bool setflags = !in_it_block();
        switch (bits(insn, 9, 6)) {
        case 0x0: res = cpu.r[rdn] & cpu.r[rm]; cpu.r[rdn] = res; if (setflags) { set_nz(res); } return;
        case 0x1: res = cpu.r[rdn] ^ cpu.r[rm]; cpu.r[rdn] = res; if (setflags) { set_nz(res); } return;
        case 0x2: res = do_shift(cpu.r[rdn], 0, cpu.r[rm] & 0xFF, &co); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; } return;
        case 0x3: res = do_shift(cpu.r[rdn], 1, cpu.r[rm] & 0xFF, &co); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; } return;
        case 0x4: res = do_shift(cpu.r[rdn], 2, cpu.r[rm] & 0xFF, &co); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; } return;
        case 0x5: res = add_with_carry(cpu.r[rdn], cpu.r[rm], cpu.c, &co, &vo); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
        case 0x6: res = add_with_carry(cpu.r[rdn], ~cpu.r[rm], cpu.c, &co, &vo); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
        case 0x7: res = do_shift(cpu.r[rdn], 3, cpu.r[rm] & 0xFF, &co); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; } return;
        case 0x8: res = cpu.r[rdn] & cpu.r[rm]; set_nz(res); return;                       /* TST */
        case 0x9: res = add_with_carry(0, ~cpu.r[rm], 1, &co, &vo); cpu.r[rdn] = res; if (setflags) { set_nz(res); cpu.c = co; cpu.v = vo; } return; /* RSB #0 */
        case 0xA: res = add_with_carry(cpu.r[rdn], ~cpu.r[rm], 1, &co, &vo); set_nz(res); cpu.c = co; cpu.v = vo; return; /* CMP */
        case 0xB: res = add_with_carry(cpu.r[rdn], cpu.r[rm], 0, &co, &vo); set_nz(res); cpu.c = co; cpu.v = vo; return;  /* CMN */
        case 0xC: res = cpu.r[rdn] | cpu.r[rm]; cpu.r[rdn] = res; if (setflags) set_nz(res); return;
        case 0xD: res = cpu.r[rdn] * cpu.r[rm]; cpu.r[rdn] = res; if (setflags) set_nz(res); return;
        case 0xE: res = cpu.r[rdn] & ~cpu.r[rm]; cpu.r[rdn] = res; if (setflags) set_nz(res); return;
        default:  res = ~cpu.r[rm]; cpu.r[rdn] = res; if (setflags) set_nz(res); return;
        }
    }

    /* --- special data instructions and BX/BLX --- */
    if (op == 0x11) {
        u32 opc = bits(insn, 9, 8);
        int rm = bits(insn, 6, 3);
        int rdn = (insn & 7) | (bit(insn, 7) << 3);
        u32 co, vo;
        switch (opc) {
        case 0: {                                     /* ADD (register) */
            u32 res = cpu.r[rdn] + cpu.r[rm];
            if (rdn == 15) { branch_to(res); return; }
            cpu.r[rdn] = res;
            return;
        }
        case 1: {                                     /* CMP (register) */
            u32 res = add_with_carry(cpu.r[rdn], ~cpu.r[rm], 1, &co, &vo);
            set_nz(res); cpu.c = co; cpu.v = vo;
            return;
        }
        case 2: {                                     /* MOV (register) */
            u32 v = cpu.r[rm];
            if (rdn == 15) { branch_to(v); return; }
            cpu.r[rdn] = v;
            return;
        }
        default: {                                    /* BX / BLX */
            u32 target = cpu.r[rm];
            if (bit(insn, 7)) {                       /* BLX */
                cpu.r[14] = (cpu.pc + 2) | 1;
            }
            bx_write_pc(target);
            return;
        }
        }
    }

    /* --- LDR (literal) --- */
    if (op == 0x12 || op == 0x13) {
        int rt = bits(insn, 10, 8);
        u32 addr = ((cpu.pc + 4) & ~3u) + (insn & 0xFF) * 4;
        cpu.r[rt] = bus_read32(addr);
        return;
    }

    /* --- load/store register offset (0101xx) --- */
    if (op >= 0x14 && op <= 0x17) {
        int rt = insn & 7, rn = (insn >> 3) & 7, rm = (insn >> 6) & 7;
        u32 addr = cpu.r[rn] + cpu.r[rm];
        switch (bits(insn, 11, 9)) {
        case 0: bus_write32(addr, cpu.r[rt]); return;
        case 1: bus_write16(addr, cpu.r[rt]); return;
        case 2: bus_write8(addr, cpu.r[rt]); return;
        case 3: cpu.r[rt] = sext(bus_read8(addr), 8); return;
        case 4: cpu.r[rt] = bus_read32(addr); return;
        case 5: cpu.r[rt] = bus_read16(addr); return;
        case 6: cpu.r[rt] = bus_read8(addr); return;
        default: cpu.r[rt] = sext(bus_read16(addr), 16); return;
        }
    }

    /* --- load/store word/byte immediate (011xxx) --- */
    if (op >= 0x18 && op <= 0x1F) {
        int rt = insn & 7, rn = (insn >> 3) & 7;
        u32 imm5 = bits(insn, 10, 6);
        bool byte = bit(insn, 12), load = bit(insn, 11);
        u32 addr = cpu.r[rn] + (byte ? imm5 : imm5 * 4);
        if (byte) { if (load) cpu.r[rt] = bus_read8(addr); else bus_write8(addr, cpu.r[rt]); }
        else { if (load) cpu.r[rt] = bus_read32(addr); else bus_write32(addr, cpu.r[rt]); }
        return;
    }

    /* --- load/store halfword immediate (1000xx) --- */
    if (op >= 0x20 && op <= 0x23) {
        int rt = insn & 7, rn = (insn >> 3) & 7;
        u32 addr = cpu.r[rn] + bits(insn, 10, 6) * 2;
        if (bit(insn, 11)) cpu.r[rt] = bus_read16(addr);
        else bus_write16(addr, cpu.r[rt]);
        return;
    }

    /* --- load/store SP relative (1001xx) --- */
    if (op >= 0x24 && op <= 0x27) {
        int rt = bits(insn, 10, 8);
        u32 addr = cpu.r[13] + (insn & 0xFF) * 4;
        if (bit(insn, 11)) cpu.r[rt] = bus_read32(addr);
        else bus_write32(addr, cpu.r[rt]);
        return;
    }

    /* --- ADR (10100x) / ADD SP (10101x) --- */
    if (op == 0x28 || op == 0x29) {
        int rd = bits(insn, 10, 8);
        cpu.r[rd] = ((cpu.pc + 4) & ~3u) + (insn & 0xFF) * 4;
        return;
    }
    if (op == 0x2A || op == 0x2B) {
        int rd = bits(insn, 10, 8);
        cpu.r[rd] = cpu.r[13] + (insn & 0xFF) * 4;
        return;
    }

    /* --- misc 16-bit (1011xx) --- */
    if (op >= 0x2C && op <= 0x2F) {
        u32 o = bits(insn, 11, 5);
        if ((o & 0x7C) == 0x00) {                   /* ADD SP, imm */
            cpu.r[13] += (insn & 0x7F) * 4; return;
        }
        if ((o & 0x7C) == 0x04) {                   /* SUB SP, imm */
            cpu.r[13] -= (insn & 0x7F) * 4; return;
        }
        if ((insn & 0xF500) == 0xB100) {            /* CBZ / CBNZ */
            int rn = insn & 7;
            u32 imm = (bits(insn, 7, 3) << 1) | (bit(insn, 9) << 6);
            bool nz = bit(insn, 11);
            if ((cpu.r[rn] == 0) != nz) branch_to(cpu.pc + 4 + imm);
            return;
        }
        switch (bits(insn, 11, 6)) {
        case 0x08: cpu.r[insn & 7] = sext(cpu.r[(insn >> 3) & 7], 16); return;  /* SXTH */
        case 0x09: cpu.r[insn & 7] = sext(cpu.r[(insn >> 3) & 7], 8); return;   /* SXTB */
        case 0x0A: cpu.r[insn & 7] = cpu.r[(insn >> 3) & 7] & 0xFFFF; return;   /* UXTH */
        case 0x0B: cpu.r[insn & 7] = cpu.r[(insn >> 3) & 7] & 0xFF; return;     /* UXTB */
        case 0x28: cpu.r[insn & 7] = __builtin_bswap32(cpu.r[(insn >> 3) & 7]); return; /* REV */
        case 0x29: {                                                             /* REV16 */
            u32 v = cpu.r[(insn >> 3) & 7];
            cpu.r[insn & 7] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
            return;
        }
        case 0x2B: {                                                             /* REVSH */
            u32 v = cpu.r[(insn >> 3) & 7];
            cpu.r[insn & 7] = sext(((v & 0xFF) << 8) | ((v >> 8) & 0xFF), 16);
            return;
        }
        }
        if ((insn & 0xFE00) == 0xB400) {            /* PUSH */
            u32 list = (insn & 0xFF) | (bit(insn, 8) << 14);
            u32 sp = cpu.r[13];
            for (int i = 14; i >= 0; i--) if (list & (1u << i)) { sp -= 4; bus_write32(sp, cpu.r[i]); }
            cpu.r[13] = sp;
            return;
        }
        if ((insn & 0xFE00) == 0xBC00) {            /* POP */
            u32 list = (insn & 0xFF) | (bit(insn, 8) << 15);
            u32 sp = cpu.r[13];
            for (int i = 0; i < 16; i++) if (list & (1u << i)) {
                u32 v = bus_read32(sp); sp += 4;
                if (i == 15) { cpu.r[13] = sp; bx_write_pc(v); return; }
                cpu.r[i] = v;
            }
            cpu.r[13] = sp;
            return;
        }
        if ((insn & 0xFFE8) == 0xB660) {            /* CPS */
            bool disable = bit(insn, 4);
            if (insn & 2) cpu.primask = disable;
            if (insn & 1) cpu.faultmask = disable;
            exc_dirty = true;
            return;
        }
        if ((insn & 0xFF00) == 0xBF00) {            /* IT and hints */
            u32 mask = insn & 0xF;
            if (mask) { cpu.itstate = insn & 0xFF; cpu.itstate_written = true; return; }
            switch (bits(insn, 7, 4)) {
            case 0: return;                          /* NOP */
            case 1: return;                          /* YIELD */
            case 2: cpu.sleeping = true; return;     /* WFE */
            case 3: cpu.sleeping = true; return;     /* WFI */
            case 4: return;                          /* SEV */
            default: return;
            }
        }
        if ((insn & 0xFF00) == 0xBE00) {            /* BKPT */
            gwlog("[cpu] BKPT #%u at %08x\n", insn & 0xFF, cpu.pc);
            return;
        }
        gwlog("[cpu] undefined 16-bit misc %04x at %08x\n", insn, cpu.pc);
        return;
    }

    /* --- STM / LDM (1100xx) --- */
    if (op >= 0x30 && op <= 0x33) {
        int rn = bits(insn, 10, 8);
        u32 list = insn & 0xFF;
        u32 addr = cpu.r[rn];
        bool load = bit(insn, 11);
        if (load) {
            for (int i = 0; i < 8; i++) if (list & (1u << i)) { cpu.r[i] = bus_read32(addr); addr += 4; }
            if (!(list & (1u << rn))) cpu.r[rn] = addr;
        } else {
            for (int i = 0; i < 8; i++) if (list & (1u << i)) { bus_write32(addr, cpu.r[i]); addr += 4; }
            cpu.r[rn] = addr;
        }
        return;
    }

    /* --- conditional branch / SVC (1101xx) --- */
    if (op >= 0x34 && op <= 0x37) {
        u32 cond = bits(insn, 11, 8);
        if (cond == 0xE) { gwlog("[cpu] UDF at %08x\n", cpu.pc); return; }
        if (cond == 0xF) { cpu_set_pending(EXC_SVCALL); return; }
        if (cond_passed(cond)) branch_to(cpu.pc + 4 + (sext(insn & 0xFF, 8) << 1));
        return;
    }

    /* --- unconditional branch (11100x) --- */
    if (op == 0x38 || op == 0x39) {
        branch_to(cpu.pc + 4 + (sext(insn & 0x7FF, 11) << 1));
        return;
    }

    gwlog("[cpu] undefined 16-bit %04x at %08x\n", insn, cpu.pc);
}

/* ------------------------------------------------------------------ */
/* 32-bit instruction execution                                        */
/* ------------------------------------------------------------------ */

static void ldm_stm(u32 hw1, u32 hw2, bool load, bool inc, bool before, bool wback)
{
    int rn = bits(hw1, 3, 0);
    u32 list = hw2 & 0xFFFF;
    int count = __builtin_popcount(list);
    u32 base = cpu.r[rn];
    u32 addr = inc ? (before ? base + 4 : base) : (before ? base - count * 4 : base - count * 4 + 4);
    u32 final = inc ? base + count * 4 : base - count * 4;

    if (load) {
        u32 newpc = 0; bool dopc = false;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) {
            u32 v = bus_read32(addr); addr += 4;
            if (i == 15) { newpc = v; dopc = true; }
            else cpu.r[i] = v;
        }
        if (wback && !(list & (1u << rn))) cpu.r[rn] = final;
        if (dopc) bx_write_pc(newpc);
    } else {
        for (int i = 0; i < 16; i++) if (list & (1u << i)) { bus_write32(addr, cpu.r[i]); addr += 4; }
        if (wback) cpu.r[rn] = final;
    }
}

static void exec_thumb32(u32 hw1, u32 hw2)
{
    u32 op1 = bits(hw1, 12, 11);
    u32 op2 = bits(hw1, 10, 4);

    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) {              /* load/store multiple */
            u32 opc = bits(hw1, 8, 7);
            bool L = bit(hw1, 4), W = bit(hw1, 5);
            int rn = bits(hw1, 3, 0);
            if (opc == 1) {                       /* LDMIA/STMIA (incl. POP) */
                if (L && rn == 13 && W) {         /* POP */
                    u32 list = hw2 & 0xFFFF;
                    u32 sp = cpu.r[13];
                    u32 newpc = 0; bool dopc = false;
                    for (int i = 0; i < 16; i++) if (list & (1u << i)) {
                        u32 v = bus_read32(sp); sp += 4;
                        if (i == 15) { newpc = v; dopc = true; } else cpu.r[i] = v;
                    }
                    cpu.r[13] = sp;
                    if (dopc) bx_write_pc(newpc);
                    return;
                }
                ldm_stm(hw1, hw2, L, true, false, W);
                return;
            }
            if (opc == 2) {                       /* LDMDB/STMDB (incl. PUSH) */
                if (!L && rn == 13 && W) {        /* PUSH */
                    u32 list = hw2 & 0xFFFF;
                    u32 sp = cpu.r[13];
                    for (int i = 15; i >= 0; i--) if (list & (1u << i)) { sp -= 4; bus_write32(sp, cpu.r[i]); }
                    cpu.r[13] = sp;
                    return;
                }
                ldm_stm(hw1, hw2, L, false, true, W);
                return;
            }
            gwlog("[cpu] SRS/RFE unsupported %04x %04x at %08x\n", hw1, hw2, cpu.pc);
            return;
        }
        if ((op2 & 0x64) == 0x04) {              /* load/store dual, exclusive, table branch */
            /* hw1[8:4] = P,U,1,W,L for LDRD/STRD; the four values below are the
               encodings where P=W=0, which the architecture reuses for the
               exclusive accesses and table branches. */
            u32 sel = bits(hw1, 8, 4);
            int rn = bits(hw1, 3, 0);
            if (sel == 0x04) {                    /* STREX */
                int rd = bits(hw2, 11, 8), rt = bits(hw2, 15, 12);
                u32 addr = cpu.r[rn] + (hw2 & 0xFF) * 4;
                if (cpu.excl_valid && cpu.excl_addr == addr) {
                    bus_write32(addr, cpu.r[rt]); cpu.r[rd] = 0;
                } else cpu.r[rd] = 1;
                cpu.excl_valid = false;
                return;
            }
            if (sel == 0x05) {                    /* LDREX */
                int rt = bits(hw2, 15, 12);
                u32 addr = cpu.r[rn] + (hw2 & 0xFF) * 4;
                cpu.r[rt] = bus_read32(addr);
                cpu.excl_valid = true; cpu.excl_addr = addr;
                return;
            }
            if (sel == 0x0C) {                    /* STREXB / STREXH / STREXD */
                int rd = bits(hw2, 3, 0), rt = bits(hw2, 15, 12);
                u32 addr = cpu.r[rn];
                u32 sz = bits(hw2, 7, 4);
                if (cpu.excl_valid && cpu.excl_addr == addr) {
                    if (sz == 4) bus_write8(addr, cpu.r[rt]);
                    else if (sz == 5) bus_write16(addr, cpu.r[rt]);
                    else { bus_write32(addr, cpu.r[rt]); bus_write32(addr + 4, cpu.r[bits(hw2, 11, 8)]); }
                    cpu.r[rd] = 0;
                } else cpu.r[rd] = 1;
                cpu.excl_valid = false;
                return;
            }
            if (sel == 0x0D) {
                u32 sz = bits(hw2, 7, 4);
                if (sz == 0 || sz == 1) {         /* TBB / TBH */
                    int rm = bits(hw2, 3, 0);
                    bool h = (sz == 1);
                    u32 base = (rn == 15) ? cpu.pc + 4 : cpu.r[rn];
                    u32 off = h ? bus_read16(base + cpu.r[rm] * 2) : bus_read8(base + cpu.r[rm]);
                    branch_to(cpu.pc + 4 + off * 2);
                    return;
                }
                if (sz == 2) { cpu.excl_valid = false; return; }   /* CLREX */
                {                                 /* LDREXB / LDREXH / LDREXD */
                    int rt = bits(hw2, 15, 12);
                    u32 addr = cpu.r[rn];
                    if (sz == 4) cpu.r[rt] = bus_read8(addr);
                    else if (sz == 5) cpu.r[rt] = bus_read16(addr);
                    else { cpu.r[rt] = bus_read32(addr); cpu.r[bits(hw2, 11, 8)] = bus_read32(addr + 4); }
                    cpu.excl_valid = true; cpu.excl_addr = addr;
                    return;
                }
            }
            /* LDRD / STRD (immediate) */
            {
                bool P = bit(hw1, 8), U = bit(hw1, 7), W = bit(hw1, 5), L = bit(hw1, 4);
                int rt = bits(hw2, 15, 12), rt2 = bits(hw2, 11, 8);
                u32 imm = (hw2 & 0xFF) * 4;
                u32 base = (rn == 15) ? ((cpu.pc + 4) & ~3u) : cpu.r[rn];
                u32 addr = P ? (U ? base + imm : base - imm) : base;
                if (L) { cpu.r[rt] = bus_read32(addr); cpu.r[rt2] = bus_read32(addr + 4); }
                else { bus_write32(addr, cpu.r[rt]); bus_write32(addr + 4, cpu.r[rt2]); }
                if (W && rn != 15) cpu.r[rn] = U ? base + imm : base - imm;
                return;
            }
        }
        if ((op2 & 0x60) == 0x20) {              /* data processing (shifted register) */
            u32 opc = bits(hw1, 8, 5);
            bool S = bit(hw1, 4);
            int rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8), rm = bits(hw2, 3, 0);
            int imm5 = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
            int type = bits(hw2, 5, 4);
            int amount; int st = decode_shift(type, imm5, &amount);
            u32 carry = cpu.c;
            u32 shifted = do_shift(cpu.r[rm], st, amount, &carry);
            u32 res, co, vo;
            switch (opc) {
            case 0x0:                                          /* AND / TST */
                res = cpu.r[rn] & shifted;
                if (rd == 15 && S) { set_nz(res); cpu.c = carry; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x1: res = cpu.r[rn] & ~shifted; cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return; /* BIC */
            case 0x2:                                          /* ORR / MOV */
                res = (rn == 15) ? shifted : (cpu.r[rn] | shifted);
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x3:                                          /* ORN / MVN */
                res = (rn == 15) ? ~shifted : (cpu.r[rn] | ~shifted);
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x4:                                          /* EOR / TEQ */
                res = cpu.r[rn] ^ shifted;
                if (rd == 15 && S) { set_nz(res); cpu.c = carry; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x6:                                          /* PKH */
                if (type == 0) res = (cpu.r[rn] & 0xFFFF) | (shifted & 0xFFFF0000);
                else res = (shifted & 0xFFFF) | (cpu.r[rn] & 0xFFFF0000);
                cpu.r[rd] = res; return;
            case 0x8:                                          /* ADD / CMN */
                res = add_with_carry(cpu.r[rn], shifted, 0, &co, &vo);
                if (rd == 15 && S) { set_nz(res); cpu.c = co; cpu.v = vo; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xA: res = add_with_carry(cpu.r[rn], shifted, cpu.c, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return; /* ADC */
            case 0xB: res = add_with_carry(cpu.r[rn], ~shifted, cpu.c, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return; /* SBC */
            case 0xD:                                          /* SUB / CMP */
                res = add_with_carry(cpu.r[rn], ~shifted, 1, &co, &vo);
                if (rd == 15 && S) { set_nz(res); cpu.c = co; cpu.v = vo; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xE: res = add_with_carry(~cpu.r[rn], shifted, 1, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return; /* RSB */
            }
            gwlog("[cpu] unhandled dp-shifted opc=%x at %08x\n", opc, cpu.pc);
            return;
        }
        /* coprocessor / FP */
        exec_vfp(hw1, hw2);
        return;
    }

    if (op1 == 2) {
        if (bit(hw2, 15)) {                       /* branches and misc control */
            u32 opb = bits(hw1, 10, 4);
            u32 op3 = bits(hw2, 14, 12);
            if ((op3 & 5) == 0) {                 /* conditional branch or misc */
                if ((opb & 0x38) != 0x38) {       /* B<c>.W */
                    u32 cond = bits(hw1, 9, 6);
                    u32 imm = (bits(hw2, 10, 0) << 1) | (bits(hw1, 5, 0) << 12) |
                              (bit(hw2, 13) << 18) | (bit(hw2, 11) << 19) | (bit(hw1, 10) << 20);
                    if (cond_passed(cond)) branch_to(cpu.pc + 4 + sext(imm, 21));
                    return;
                }
                /* misc control */
                switch (opb) {
                case 0x38: case 0x39: {           /* MSR */
                    int rn = bits(hw1, 3, 0);
                    u32 sysm = hw2 & 0xFF;
                    u32 v = cpu.r[rn];
                    u32 mask = bits(hw2, 11, 10);
                    if (sysm <= 3) {              /* xPSR */
                        if (mask & 2) {
                            cpu.n = v >> 31; cpu.z = (v >> 30) & 1;
                            cpu.c = (v >> 29) & 1; cpu.v = (v >> 28) & 1; cpu.q = (v >> 27) & 1;
                        }
                        if (mask & 1) cpu.ge = (v >> 16) & 0xF;
                    } else switch (sysm) {
                    case 8: if (cpu.handler_mode) cpu.sp_main = v; else cpu.r[13] = v; break;
                    case 9: if ((cpu.control & 2) && !cpu.handler_mode) cpu.r[13] = v; else cpu.sp_process = v; break;
                    case 16: cpu.primask = v & 1; exc_dirty = true; break;
                    case 17: cpu.basepri = v & 0xFF; exc_dirty = true; break;
                    case 18: if (v & 0xFF) { if (cpu.basepri == 0 || (v & 0xFF) < cpu.basepri) cpu.basepri = v & 0xFF; } exc_dirty = true; break;
                    case 19: cpu.faultmask = v & 1; exc_dirty = true; break;
                    case 20: {
                        bool old_psp = (cpu.control & 2) && !cpu.handler_mode;
                        u8 nc = v & 7;
                        if (!cpu.handler_mode) {
                            bool new_psp = (nc & 2) != 0;
                            if (new_psp != old_psp) {
                                if (old_psp) cpu.sp_process = cpu.r[13]; else cpu.sp_main = cpu.r[13];
                                cpu.r[13] = new_psp ? cpu.sp_process : cpu.sp_main;
                            }
                            cpu.control = (cpu.control & ~3u) | (nc & 3);
                        } else cpu.control = (cpu.control & ~1u) | (nc & 1);
                        break;
                    }
                    default: gwlog("[cpu] MSR unknown sysm %u\n", sysm); break;
                    }
                    return;
                }
                case 0x3A: {                      /* CPS and hints (NOP.W, WFI.W, ...) */
                    if (bits(hw2, 10, 8) == 0) {
                        switch (hw2 & 0xFF) {
                        case 2: case 3: cpu.sleeping = true; break;   /* WFE / WFI */
                        default: break;                               /* NOP/YIELD/SEV/DBG */
                        }
                    }
                    return;
                }
                case 0x3B: {                      /* misc: DSB/DMB/ISB/CLREX */
                    u32 o = bits(hw2, 7, 4);
                    if (o == 2) cpu.excl_valid = false;
                    return;
                }
                case 0x3E: case 0x3F: {           /* MRS */
                    int rd = bits(hw2, 11, 8);
                    u32 sysm = hw2 & 0xFF;
                    u32 v = 0;
                    if (sysm <= 3) {
                        if (sysm == 0 || sysm == 3)
                            v |= ((u32)cpu.n << 31) | ((u32)cpu.z << 30) | ((u32)cpu.c << 29) |
                                 ((u32)cpu.v << 28) | ((u32)cpu.q << 27) | ((u32)cpu.ge << 16);
                        if (sysm == 1 || sysm == 2 || sysm == 3) v |= cpu.ipsr;
                    } else switch (sysm) {
                    case 8: v = cpu.handler_mode ? cpu.r[13] : (((cpu.control & 2) ? cpu.sp_main : cpu.r[13])); break;
                    case 9: v = ((cpu.control & 2) && !cpu.handler_mode) ? cpu.r[13] : cpu.sp_process; break;
                    case 16: v = cpu.primask; break;
                    case 17: case 18: v = cpu.basepri; break;
                    case 19: v = cpu.faultmask; break;
                    case 20: v = cpu.control; break;
                    }
                    cpu.r[rd] = v;
                    return;
                }
                }
                gwlog("[cpu] unhandled misc-control %04x %04x at %08x\n", hw1, hw2, cpu.pc);
                return;
            }
            if ((op3 & 5) == 1) {                 /* B.W */
                u32 s = bit(hw1, 10);
                u32 i1 = !(bit(hw2, 13) ^ s), i2 = !(bit(hw2, 11) ^ s);
                u32 imm = (bits(hw2, 10, 0) << 1) | (bits(hw1, 9, 0) << 12) |
                          (i2 << 22) | (i1 << 23) | (s << 24);
                branch_to(cpu.pc + 4 + sext(imm, 25));
                return;
            }
            if ((op3 & 5) == 5 || (op3 & 5) == 4) { /* BL / BLX */
                u32 s = bit(hw1, 10);
                u32 i1 = !(bit(hw2, 13) ^ s), i2 = !(bit(hw2, 11) ^ s);
                u32 imm = (bits(hw2, 10, 0) << 1) | (bits(hw1, 9, 0) << 12) |
                          (i2 << 22) | (i1 << 23) | (s << 24);
                u32 target = cpu.pc + 4 + sext(imm, 25);
                cpu.r[14] = (cpu.pc + 4) | 1;
                branch_to(target);
                return;
            }
        }
        if (!bit(hw1, 9)) {                       /* data processing (modified immediate) */
            u32 opc = bits(hw1, 8, 5);
            bool S = bit(hw1, 4);
            int rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8);
            u32 imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | (hw2 & 0xFF);
            u32 carry = cpu.c;
            u32 imm = thumb_expand_imm_c(imm12, &carry);
            u32 res, co, vo;
            switch (opc) {
            case 0x0:
                res = cpu.r[rn] & imm;
                if (rd == 15) { set_nz(res); cpu.c = carry; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x1: res = cpu.r[rn] & ~imm; cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x2: res = (rn == 15) ? imm : (cpu.r[rn] | imm); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x3: res = (rn == 15) ? ~imm : (cpu.r[rn] | ~imm); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x4:
                res = cpu.r[rn] ^ imm;
                if (rd == 15) { set_nz(res); cpu.c = carry; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = carry; } return;
            case 0x8:
                res = add_with_carry(cpu.r[rn], imm, 0, &co, &vo);
                if (rd == 15) { set_nz(res); cpu.c = co; cpu.v = vo; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xA: res = add_with_carry(cpu.r[rn], imm, cpu.c, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xB: res = add_with_carry(cpu.r[rn], ~imm, cpu.c, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xD:
                res = add_with_carry(cpu.r[rn], ~imm, 1, &co, &vo);
                if (rd == 15) { set_nz(res); cpu.c = co; cpu.v = vo; return; }
                cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            case 0xE: res = add_with_carry(~cpu.r[rn], imm, 1, &co, &vo); cpu.r[rd] = res; if (S) { set_nz(res); cpu.c = co; cpu.v = vo; } return;
            }
            gwlog("[cpu] unhandled dp-imm opc=%x at %08x\n", opc, cpu.pc);
            return;
        }
        /* data processing (plain binary immediate) */
        {
            u32 opc = bits(hw1, 8, 4);
            int rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8);
            u32 i = bit(hw1, 10), imm3 = bits(hw2, 14, 12), imm8 = hw2 & 0xFF;
            u32 imm12 = (i << 11) | (imm3 << 8) | imm8;
            switch (opc) {
            case 0x00:                                   /* ADDW / ADR */
                cpu.r[rd] = (rn == 15) ? (((cpu.pc + 4) & ~3u) + imm12) : (cpu.r[rn] + imm12);
                return;
            case 0x04:                                   /* MOVW */
                cpu.r[rd] = (bits(hw1, 3, 0) << 12) | imm12;
                return;
            case 0x0A:                                   /* SUBW / ADR */
                cpu.r[rd] = (rn == 15) ? (((cpu.pc + 4) & ~3u) - imm12) : (cpu.r[rn] - imm12);
                return;
            case 0x0C:                                   /* MOVT */
                cpu.r[rd] = (cpu.r[rd] & 0xFFFF) | (((bits(hw1, 3, 0) << 12) | imm12) << 16);
                return;
            case 0x10: case 0x12: {                      /* SSAT / SSAT16 */
                u32 satimm = (hw2 & 0x1F) + 1;
                int sh = (imm3 << 2) | bits(hw2, 7, 6);
                int sat = 0;
                if (opc == 0x12 && sh == 0 && bit(hw1, 5) == 0) {
                    /* SSAT16 */
                    u32 v = cpu.r[rn];
                    u32 lo = (u32)(u16)signed_sat((s16)(v & 0xFFFF), satimm, &sat);
                    u32 hi = (u32)(u16)signed_sat((s16)(v >> 16), satimm, &sat);
                    cpu.r[rd] = (hi << 16) | lo;
                } else {
                    u32 carry = cpu.c;
                    u32 v = shift_c(cpu.r[rn], bit(hw1, 5) ? 2 : 0, sh, &carry);
                    cpu.r[rd] = signed_sat((s32)v, satimm, &sat);
                }
                if (sat) cpu.q = 1;
                return;
            }
            case 0x14: {                                 /* SBFX */
                u32 lsb = (imm3 << 2) | bits(hw2, 7, 6);
                u32 width = (hw2 & 0x1F) + 1;
                cpu.r[rd] = sext((cpu.r[rn] >> lsb) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1)), width);
                return;
            }
            case 0x16: {                                 /* BFI / BFC */
                u32 lsb = (imm3 << 2) | bits(hw2, 7, 6);
                u32 msb = hw2 & 0x1F;
                if (msb < lsb) return;
                u32 width = msb - lsb + 1;
                u32 mask = ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
                u32 src = (rn == 15) ? 0 : cpu.r[rn];
                cpu.r[rd] = (cpu.r[rd] & ~mask) | ((src << lsb) & mask);
                return;
            }
            case 0x18: case 0x1A: {                      /* USAT / USAT16 */
                u32 satimm = hw2 & 0x1F;
                int sh = (imm3 << 2) | bits(hw2, 7, 6);
                int sat = 0;
                if (opc == 0x1A && sh == 0 && bit(hw1, 5) == 0) {
                    u32 v = cpu.r[rn];
                    u32 lo = unsigned_sat((s16)(v & 0xFFFF), satimm, &sat) & 0xFFFF;
                    u32 hi = unsigned_sat((s16)(v >> 16), satimm, &sat) & 0xFFFF;
                    cpu.r[rd] = (hi << 16) | lo;
                } else {
                    u32 carry = cpu.c;
                    u32 v = shift_c(cpu.r[rn], bit(hw1, 5) ? 2 : 0, sh, &carry);
                    cpu.r[rd] = unsigned_sat((s32)v, satimm, &sat);
                }
                if (sat) cpu.q = 1;
                return;
            }
            case 0x1C: {                                 /* UBFX */
                u32 lsb = (imm3 << 2) | bits(hw2, 7, 6);
                u32 width = (hw2 & 0x1F) + 1;
                cpu.r[rd] = (cpu.r[rn] >> lsb) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1));
                return;
            }
            }
            gwlog("[cpu] unhandled plain-imm opc=%02x at %08x\n", opc, cpu.pc);
            return;
        }
    }

    /* ---------------- op1 == 3 ---------------- */
    if ((op2 & 0x71) == 0x00) {                  /* store single data item */
        u32 sz = bits(hw1, 6, 5);
        int rn = bits(hw1, 3, 0), rt = bits(hw2, 15, 12);
        u32 addr, base = cpu.r[rn];
        bool wb = false; u32 wbval = 0;
        if (bit(hw1, 7)) {                        /* immediate 12-bit */
            addr = base + (hw2 & 0xFFF);
        } else if (bit(hw2, 11)) {                /* immediate 8-bit, P/U/W */
            bool P = bit(hw2, 10), U = bit(hw2, 9), W = bit(hw2, 8);
            u32 imm = hw2 & 0xFF;
            u32 off = U ? base + imm : base - imm;
            addr = P ? off : base;
            if (W) { wb = true; wbval = off; }
        } else {                                  /* register offset */
            int rm = bits(hw2, 3, 0);
            u32 sh = bits(hw2, 5, 4);
            addr = base + (cpu.r[rm] << sh);
        }
        switch (sz) {
        case 0: bus_write8(addr, cpu.r[rt]); break;
        case 1: bus_write16(addr, cpu.r[rt]); break;
        default: bus_write32(addr, cpu.r[rt]); break;
        }
        if (wb) cpu.r[rn] = wbval;
        return;
    }

    if ((op2 & 0x67) == 0x01 || (op2 & 0x67) == 0x03 || (op2 & 0x67) == 0x05) {
        /* load byte / halfword / word */
        u32 sz = (op2 & 0x07) >> 1;               /* 0=byte 1=half 2=word */
        bool sign = bit(hw1, 8);
        int rn = bits(hw1, 3, 0), rt = bits(hw2, 15, 12);
        u32 addr, base;
        bool wb = false; u32 wbval = 0;
        if (rn == 15) {
            u32 imm = hw2 & 0xFFF;
            base = (cpu.pc + 4) & ~3u;
            addr = bit(hw1, 7) ? base + imm : base - imm;
        } else {
            base = cpu.r[rn];
            if (bit(hw1, 7)) addr = base + (hw2 & 0xFFF);
            else if (bit(hw2, 11)) {
                bool P = bit(hw2, 10), U = bit(hw2, 9), W = bit(hw2, 8);
                u32 imm = hw2 & 0xFF;
                u32 off = U ? base + imm : base - imm;
                addr = P ? off : base;
                if (W) { wb = true; wbval = off; }
            } else {
                int rm = bits(hw2, 3, 0);
                u32 sh = bits(hw2, 5, 4);
                addr = base + (cpu.r[rm] << sh);
            }
        }
        u32 v;
        switch (sz) {
        case 0: v = bus_read8(addr); if (sign) v = sext(v, 8); break;
        case 1: v = bus_read16(addr); if (sign) v = sext(v, 16); break;
        default: v = bus_read32(addr); break;
        }
        if (rt != 15) cpu.r[rt] = v; else bx_write_pc(v);
        if (wb) cpu.r[rn] = wbval;
        return;
    }

    if ((op2 & 0x70) == 0x20) {                  /* data processing (register) */
        u32 opc1 = bits(hw1, 7, 4);
        u32 opc2 = bits(hw2, 7, 4);
        int rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8), rm = bits(hw2, 3, 0);

        if ((opc1 & 0x8) == 0 && opc2 == 0) {    /* LSL/LSR/ASR/ROR register */
            int type = (opc1 >> 1) & 3;
            u32 carry = cpu.c;
            u32 res = do_shift(cpu.r[rn], type, cpu.r[rm] & 0xFF, &carry);
            cpu.r[rd] = res;
            if (bit(hw1, 4)) { set_nz(res); cpu.c = carry; }
            return;
        }
        if ((opc2 & 0x8) == 0x8) {               /* extend / extend-and-add */
            u32 rot = bits(hw2, 5, 4) * 8;
            u32 v = ror32(cpu.r[rm], rot);
            bool add = (rn != 15);
            u32 a = add ? cpu.r[rn] : 0;
            switch (opc1) {
            case 0x0: /* SXTAH / SXTH */ cpu.r[rd] = a + sext(v & 0xFFFF, 16); return;
            case 0x1: /* UXTAH / UXTH */ cpu.r[rd] = a + (v & 0xFFFF); return;
            case 0x4: /* SXTAB / SXTB */ cpu.r[rd] = a + sext(v & 0xFF, 8); return;
            case 0x5: /* UXTAB / UXTB */ cpu.r[rd] = a + (v & 0xFF); return;
            case 0x2: /* SXTAB16 / SXTB16 */ {
                u32 lo = (u32)(u16)((s16)(s8)(v & 0xFF) + (s16)(a & 0xFFFF));
                u32 hi = (u32)(u16)((s16)(s8)((v >> 16) & 0xFF) + (s16)(a >> 16));
                cpu.r[rd] = (hi << 16) | lo; return;
            }
            case 0x3: /* UXTAB16 / UXTB16 */ {
                u32 lo = (u32)(u16)((v & 0xFF) + (a & 0xFFFF));
                u32 hi = (u32)(u16)(((v >> 16) & 0xFF) + (a >> 16));
                cpu.r[rd] = (hi << 16) | lo; return;
            }
            }
        }
        if ((opc1 & 0x8) == 0 && (opc2 & 0xC) == 0x0 && opc2 != 0) {
            /* parallel add/sub (signed/unsigned) */
            u32 n = cpu.r[rn], m = cpu.r[rm];
            s32 n0 = (s16)(n & 0xFFFF), n1 = (s16)(n >> 16);
            s32 m0 = (s16)(m & 0xFFFF), m1 = (s16)(m >> 16);
            u32 un0 = n & 0xFFFF, un1 = n >> 16, um0 = m & 0xFFFF, um1 = m >> 16;
            u32 op = opc1 & 7;
            bool uns = (opc2 & 0x4) != 0;
            int sat = 0;
            u32 res = 0;
            switch (op) {
            case 1: /* ADD16 */
                if (opc2 == 0x0) { /* SADD16 */
                    s32 a = n0 + m0, b = n1 + m1;
                    cpu.ge = ((a >= 0) ? 3 : 0) | ((b >= 0) ? 0xC : 0);
                    res = ((u32)(u16)b << 16) | (u16)a;
                } else if (opc2 == 0x1) { /* QADD16 */
                    res = ((u32)(u16)signed_sat(n1 + m1, 16, &sat) << 16) | (u16)signed_sat(n0 + m0, 16, &sat);
                } else if (opc2 == 0x2) { /* SHADD16 */
                    res = ((u32)(u16)((n1 + m1) >> 1) << 16) | (u16)((n0 + m0) >> 1);
                } else if (opc2 == 0x4) { /* UADD16 */
                    u32 a = un0 + um0, b = un1 + um1;
                    cpu.ge = ((a >> 16) ? 3 : 0) | ((b >> 16) ? 0xC : 0);
                    res = ((b & 0xFFFF) << 16) | (a & 0xFFFF);
                } else if (opc2 == 0x5) { /* UQADD16 */
                    res = ((u32)unsigned_sat((s64)un1 + um1, 16, &sat) << 16) | unsigned_sat((s64)un0 + um0, 16, &sat);
                } else { /* UHADD16 */
                    res = (((un1 + um1) >> 1) << 16) | (((un0 + um0) >> 1) & 0xFFFF);
                }
                break;
            case 4: /* SUB16 */
                if (opc2 == 0x0) {
                    s32 a = n0 - m0, b = n1 - m1;
                    cpu.ge = ((a >= 0) ? 3 : 0) | ((b >= 0) ? 0xC : 0);
                    res = ((u32)(u16)b << 16) | (u16)a;
                } else if (opc2 == 0x1) {
                    res = ((u32)(u16)signed_sat(n1 - m1, 16, &sat) << 16) | (u16)signed_sat(n0 - m0, 16, &sat);
                } else if (opc2 == 0x2) {
                    res = ((u32)(u16)((n1 - m1) >> 1) << 16) | (u16)((n0 - m0) >> 1);
                } else if (opc2 == 0x4) {
                    u32 a = un0 - um0, b = un1 - um1;
                    cpu.ge = ((un0 >= um0) ? 3 : 0) | ((un1 >= um1) ? 0xC : 0);
                    res = ((b & 0xFFFF) << 16) | (a & 0xFFFF);
                } else if (opc2 == 0x5) {
                    res = ((u32)unsigned_sat((s64)un1 - um1, 16, &sat) << 16) | unsigned_sat((s64)un0 - um0, 16, &sat);
                } else {
                    res = ((((s32)un1 - (s32)um1) >> 1) << 16) | ((((s32)un0 - (s32)um0) >> 1) & 0xFFFF);
                }
                break;
            case 0: { /* ADD8 */
                u32 o = 0;
                for (int i = 0; i < 4; i++) {
                    s32 a = uns ? (s32)((n >> (i * 8)) & 0xFF) : (s32)(s8)((n >> (i * 8)) & 0xFF);
                    s32 b = uns ? (s32)((m >> (i * 8)) & 0xFF) : (s32)(s8)((m >> (i * 8)) & 0xFF);
                    s32 s = a + b;
                    u32 byte;
                    if ((opc2 & 3) == 1) byte = uns ? unsigned_sat(s, 8, &sat) : (u8)signed_sat(s, 8, &sat);
                    else if ((opc2 & 3) == 2) byte = (u8)(s >> 1);
                    else byte = (u8)s;
                    o |= (byte & 0xFF) << (i * 8);
                }
                res = o; break;
            }
            case 5: { /* SUB8 */
                u32 o = 0;
                for (int i = 0; i < 4; i++) {
                    s32 a = uns ? (s32)((n >> (i * 8)) & 0xFF) : (s32)(s8)((n >> (i * 8)) & 0xFF);
                    s32 b = uns ? (s32)((m >> (i * 8)) & 0xFF) : (s32)(s8)((m >> (i * 8)) & 0xFF);
                    s32 s = a - b;
                    u32 byte;
                    if ((opc2 & 3) == 1) byte = uns ? unsigned_sat(s, 8, &sat) : (u8)signed_sat(s, 8, &sat);
                    else if ((opc2 & 3) == 2) byte = (u8)(s >> 1);
                    else byte = (u8)s;
                    o |= (byte & 0xFF) << (i * 8);
                }
                res = o; break;
            }
            case 2: /* ASX */
                res = ((u32)(u16)(n1 + m0) << 16) | (u16)(n0 - m1);
                break;
            case 6: /* SAX */
                res = ((u32)(u16)(n1 - m0) << 16) | (u16)(n0 + m1);
                break;
            default:
                gwlog("[cpu] unhandled parallel op %u opc2=%x at %08x\n", op, opc2, cpu.pc);
                break;
            }
            if (sat) cpu.q = 1;
            cpu.r[rd] = res;
            return;
        }
        if (opc1 == 0x8 || opc1 == 0x9 || opc1 == 0xA || opc1 == 0xB) {
            /* misc: QADD/QSUB/REV/CLZ/SEL/... */
            if (opc1 == 0x8 && (opc2 & 0xC) == 0x8) {   /* QADD/QDADD/QSUB/QDSUB */
                s64 a = (s32)cpu.r[rm], b = (s32)cpu.r[rn];
                int sat = 0;
                u32 res;
                switch (opc2 & 3) {
                case 0: res = signed_sat(a + b, 32, &sat); break;
                case 1: res = signed_sat(a - b, 32, &sat); break;
                case 2: res = signed_sat(a + 2 * b, 32, &sat); break;
                default: res = signed_sat(a - 2 * b, 32, &sat); break;
                }
                if (sat) cpu.q = 1;
                cpu.r[rd] = res;
                return;
            }
            if (opc1 == 0x9) {
                u32 v = cpu.r[rm];
                switch (opc2 & 3) {
                case 0: cpu.r[rd] = __builtin_bswap32(v); return;
                case 1: cpu.r[rd] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF); return;
                case 2: { u32 o = 0; for (int i = 0; i < 32; i++) if (v & (1u << i)) o |= 1u << (31 - i); cpu.r[rd] = o; return; }
                default: cpu.r[rd] = sext(((v & 0xFF) << 8) | ((v >> 8) & 0xFF), 16); return;
                }
            }
            if (opc1 == 0xA && (opc2 & 0xC) == 0x8) {   /* SEL */
                u32 n = cpu.r[rn], m = cpu.r[rm], o = 0;
                for (int i = 0; i < 4; i++)
                    o |= ((cpu.ge & (1 << i)) ? (n >> (i * 8)) : (m >> (i * 8))) & 0xFF ? (((cpu.ge & (1 << i)) ? (n >> (i * 8)) : (m >> (i * 8))) & 0xFF) << (i * 8) : 0;
                /* recompute cleanly */
                o = 0;
                for (int i = 0; i < 4; i++) {
                    u32 byte = ((cpu.ge & (1 << i)) ? n : m) >> (i * 8);
                    o |= (byte & 0xFF) << (i * 8);
                }
                cpu.r[rd] = o;
                return;
            }
            if (opc1 == 0xB && (opc2 & 0xC) == 0x8) {   /* CLZ */
                u32 v = cpu.r[rm];
                cpu.r[rd] = v ? __builtin_clz(v) : 32;
                return;
            }
        }
        gwlog("[cpu] unhandled dp-reg opc1=%x opc2=%x at %08x\n", opc1, opc2, cpu.pc);
        return;
    }

    if ((op2 & 0x78) == 0x30) {                  /* multiply, multiply-accumulate */
        u32 opc1 = bits(hw1, 6, 4);
        u32 opc2 = bits(hw2, 5, 4);
        int rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8);
        int ra = bits(hw2, 15, 12), rm = bits(hw2, 3, 0);
        switch (opc1) {
        case 0:
            if (bits(hw2, 7, 4) == 0) {          /* MLA / MUL */
                u32 p = cpu.r[rn] * cpu.r[rm];
                cpu.r[rd] = (ra == 15) ? p : cpu.r[ra] + p;
            } else {                              /* MLS */
                cpu.r[rd] = cpu.r[ra] - cpu.r[rn] * cpu.r[rm];
            }
            return;
        case 1: {                                 /* SMULxy / SMLAxy */
            s32 a = bit(hw2, 5) ? (s16)(cpu.r[rn] >> 16) : (s16)(cpu.r[rn] & 0xFFFF);
            s32 b = bit(hw2, 4) ? (s16)(cpu.r[rm] >> 16) : (s16)(cpu.r[rm] & 0xFFFF);
            s64 p = (s64)a * b;
            if (ra == 15) cpu.r[rd] = (u32)p;
            else {
                int sat = 0;
                cpu.r[rd] = signed_sat((s64)(s32)cpu.r[ra] + p, 32, &sat);
                if (sat) cpu.q = 1;
            }
            return;
        }
        case 2: {                                 /* SMUAD / SMLAD */
            u32 n = cpu.r[rn], m = cpu.r[rm];
            if (bit(hw2, 4)) m = (m >> 16) | (m << 16);   /* X: swap halves */
            s64 p = (s64)(s16)(n & 0xFFFF) * (s16)(m & 0xFFFF) +
                    (s64)(s16)(n >> 16) * (s16)(m >> 16);
            if (ra != 15) p += (s32)cpu.r[ra];
            int sat = 0;
            cpu.r[rd] = signed_sat(p, 32, &sat);
            if (sat) cpu.q = 1;
            return;
        }
        case 3: {                                 /* SMULWy / SMLAWy */
            s32 b = bit(hw2, 4) ? (s16)(cpu.r[rm] >> 16) : (s16)(cpu.r[rm] & 0xFFFF);
            s64 p = ((s64)(s32)cpu.r[rn] * b) >> 16;
            if (ra == 15) cpu.r[rd] = (u32)p;
            else { int sat = 0; cpu.r[rd] = signed_sat((s64)(s32)cpu.r[ra] + p, 32, &sat); if (sat) cpu.q = 1; }
            return;
        }
        case 4: {                                 /* SMUSD / SMLSD */
            u32 n = cpu.r[rn], m = cpu.r[rm];
            if (bit(hw2, 4)) m = (m >> 16) | (m << 16);
            s64 p = (s64)(s16)(n & 0xFFFF) * (s16)(m & 0xFFFF) -
                    (s64)(s16)(n >> 16) * (s16)(m >> 16);
            if (ra != 15) p += (s32)cpu.r[ra];
            int sat = 0;
            cpu.r[rd] = signed_sat(p, 32, &sat);
            if (sat) cpu.q = 1;
            return;
        }
        case 5: {                                 /* SMMUL / SMMLA / SMMLS */
            s64 p = (s64)(s32)cpu.r[rn] * (s32)cpu.r[rm];
            if (ra != 15) {
                if (opc2 & 2) p = ((s64)(s32)cpu.r[ra] << 32) - p;
                else p = ((s64)(s32)cpu.r[ra] << 32) + p;
            }
            if (bit(hw2, 4)) p += 0x80000000LL;   /* R: round */
            cpu.r[rd] = (u32)(p >> 32);
            return;
        }
        case 6: {                                 /* USAD8 / USADA8 */
            u32 n = cpu.r[rn], m = cpu.r[rm], sum = 0;
            for (int i = 0; i < 4; i++) {
                int a = (n >> (i * 8)) & 0xFF, b = (m >> (i * 8)) & 0xFF;
                sum += (a > b) ? (a - b) : (b - a);
            }
            cpu.r[rd] = (ra == 15) ? sum : cpu.r[ra] + sum;
            return;
        }
        }
        gwlog("[cpu] unhandled multiply opc1=%x at %08x\n", opc1, cpu.pc);
        return;
    }

    if ((op2 & 0x78) == 0x38) {                  /* long multiply / divide */
        u32 opc1 = bits(hw1, 6, 4);
        u32 opc2 = bits(hw2, 7, 4);
        int rn = bits(hw1, 3, 0), rm = bits(hw2, 3, 0);
        int rdlo = bits(hw2, 15, 12), rdhi = bits(hw2, 11, 8);
        switch (opc1) {
        case 0: {                                 /* SMULL */
            s64 p = (s64)(s32)cpu.r[rn] * (s32)cpu.r[rm];
            cpu.r[rdlo] = (u32)p; cpu.r[rdhi] = (u32)(p >> 32);
            return;
        }
        case 1: {                                 /* SDIV */
            s32 a = (s32)cpu.r[rn], b = (s32)cpu.r[rm];
            cpu.r[rdhi] = (b == 0) ? 0 : ((a == INT32_MIN && b == -1) ? (u32)INT32_MIN : (u32)(a / b));
            return;
        }
        case 2: {                                 /* UMULL */
            u64 p = (u64)cpu.r[rn] * cpu.r[rm];
            cpu.r[rdlo] = (u32)p; cpu.r[rdhi] = (u32)(p >> 32);
            return;
        }
        case 3:                                   /* UDIV */
            cpu.r[rdhi] = cpu.r[rm] ? (cpu.r[rn] / cpu.r[rm]) : 0;
            return;
        case 4: {                                 /* SMLAL / SMLALxy / SMLALD */
            s64 acc = ((s64)(s32)cpu.r[rdhi] << 32) | cpu.r[rdlo];
            if (opc2 == 0) acc += (s64)(s32)cpu.r[rn] * (s32)cpu.r[rm];
            else if ((opc2 & 0xC) == 0x8) {       /* SMLALxy */
                s32 a = bit(hw2, 5) ? (s16)(cpu.r[rn] >> 16) : (s16)(cpu.r[rn] & 0xFFFF);
                s32 b = bit(hw2, 4) ? (s16)(cpu.r[rm] >> 16) : (s16)(cpu.r[rm] & 0xFFFF);
                acc += (s64)a * b;
            } else {                               /* SMLALD */
                u32 m = cpu.r[rm];
                if (bit(hw2, 4)) m = (m >> 16) | (m << 16);
                acc += (s64)(s16)(cpu.r[rn] & 0xFFFF) * (s16)(m & 0xFFFF) +
                       (s64)(s16)(cpu.r[rn] >> 16) * (s16)(m >> 16);
            }
            cpu.r[rdlo] = (u32)acc; cpu.r[rdhi] = (u32)(acc >> 32);
            return;
        }
        case 5: {                                 /* SMLSLD */
            s64 acc = ((s64)(s32)cpu.r[rdhi] << 32) | cpu.r[rdlo];
            u32 m = cpu.r[rm];
            if (bit(hw2, 4)) m = (m >> 16) | (m << 16);
            acc += (s64)(s16)(cpu.r[rn] & 0xFFFF) * (s16)(m & 0xFFFF) -
                   (s64)(s16)(cpu.r[rn] >> 16) * (s16)(m >> 16);
            cpu.r[rdlo] = (u32)acc; cpu.r[rdhi] = (u32)(acc >> 32);
            return;
        }
        case 6: {                                 /* UMLAL / UMAAL */
            if (opc2 == 6) {                      /* UMAAL */
                u64 p = (u64)cpu.r[rn] * cpu.r[rm] + cpu.r[rdlo] + cpu.r[rdhi];
                cpu.r[rdlo] = (u32)p; cpu.r[rdhi] = (u32)(p >> 32);
            } else {
                u64 acc = ((u64)cpu.r[rdhi] << 32) | cpu.r[rdlo];
                acc += (u64)cpu.r[rn] * cpu.r[rm];
                cpu.r[rdlo] = (u32)acc; cpu.r[rdhi] = (u32)(acc >> 32);
            }
            return;
        }
        }
        gwlog("[cpu] unhandled long-multiply opc1=%x at %08x\n", opc1, cpu.pc);
        return;
    }

    if ((op2 & 0x40) == 0x40) {                  /* coprocessor / FP */
        exec_vfp(hw1, hw2);
        return;
    }

    gwlog("[cpu] undefined 32-bit %04x %04x at %08x\n", hw1, hw2, cpu.pc);
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

void cpu_reset(void)
{
    memset(&cpu, 0, sizeof cpu);
    cpu.ccr = 0x00000200;                 /* STKALIGN */
    cpu.fpdscr = 0;
    cpu.vtor = FLASH_BASE;
    cpu.systick_calib = 0x40000000;
    for (int i = 0; i < NUM_EXC; i++) cpu.exc_prio[i] = 0;
    cpu.sp_main = bus_read32(FLASH_BASE + 0) & ~3u;
    cpu.r[13] = cpu.sp_main;
    u32 entry = bus_read32(FLASH_BASE + 4);
    cpu.pc = entry & ~1u;
    cpu.handler_mode = false;
    cpu.ipsr = 0;
    exc_dirty = true;
    gwlog("[cpu] reset: SP=%08x PC=%08x\n", cpu.sp_main, cpu.pc);
}

u32 cpu_run(u32 budget)
{
    u32 executed = 0;
    /* Instruction fetch almost always stays inside one region; cache the host
       pointer for it and only go through the bus when it leaves. */
    const u8 *fetch_base = NULL;
    u32 fetch_lo = 1, fetch_hi = 0;
    const bool debug = opt_trace || opt_profile || n_logcall;
    while (executed < budget && !cpu.halted) {
        if (__builtin_expect(cpu.pending_sysreset, 0)) {
            cpu.pending_sysreset = false;
            system_reset();
            fetch_lo = 1; fetch_hi = 0;
            continue;
        }
        if (exc_dirty) {
            exc_dirty = false;
            int e = pending_exception();
            if (e) { exception_taken(e); }
        }
        if (cpu.sleeping) {
            /* burn the remaining budget; timers keep running outside */
            executed = budget;
            break;
        }

        u32 pc = cpu.pc;
        u16 hw1;
        if (pc >= fetch_lo && pc + 2 <= fetch_hi) {
            memcpy(&hw1, fetch_base + (pc - fetch_lo), 2);
        } else {
            u8 *p = bus_host_ptr(pc & ~0xFFFu, 0x1000);
            if (p && (pc & 0xFFF) <= 0xFFC) {
                fetch_base = p; fetch_lo = pc & ~0xFFFu; fetch_hi = fetch_lo + 0x1000;
                memcpy(&hw1, fetch_base + (pc - fetch_lo), 2);
            } else {
                fetch_lo = 1; fetch_hi = 0;
                hw1 = bus_fetch16(pc);
            }
        }
        cpu.r[15] = pc + 4;
        cpu.pc_changed = false;
        cpu.itstate_written = false;

        if (__builtin_expect(debug, 0)) {
            if (opt_profile) prof_hit(pc);
            for (int i = 0; i < n_logcall; i++)
                if (logcall[i] == pc)
                    gwlog("[call] %08x(r0=%08x r1=%08x r2=%08x) from lr=%08x t=%llu\n",
                          pc, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[14],
                          (unsigned long long)cpu.cycles);
            if (opt_trace) {
                hist[hist_pos].pc = pc;
                memcpy(hist[hist_pos].r, cpu.r, sizeof cpu.r);
                hist_pos = (hist_pos + 1) % HIST_SIZE;
            }
        }

        bool cond_ok = true;
        u8 it = cpu.itstate;
        if (it & 0x0F) cond_ok = cond_passed(it >> 4);

        if ((hw1 & 0xF800) >= 0xE800) {           /* 32-bit encoding */
            u16 hw2;
        if (pc + 4 <= fetch_hi) memcpy(&hw2, fetch_base + (pc + 2 - fetch_lo), 2);
        else hw2 = bus_fetch16(pc + 2);
            if (cond_ok) exec_thumb32(hw1, hw2);
            if (!cpu.pc_changed) cpu.pc = pc + 4;
        } else {
            if (cond_ok) exec_thumb16(hw1);
            if (!cpu.pc_changed) cpu.pc = pc + 2;
        }

        /* Advance ITSTATE, unless the instruction set it itself: an IT
           instruction loads it, and an exception return restores the value
           saved in the stacked xPSR - advancing either would drop a
           conditional instruction out of its block. */
        if (!cpu.itstate_written && (cpu.itstate & 0x0F)) it_advance();

        executed++;
        cpu.cycles++;
    }
    return executed;
}
