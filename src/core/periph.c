/* periph.c - STM32H7B0 peripherals: RCC, PWR, FLASH, GPIO, EXTI, SYSCFG,
 *            timers, DMA/DMAMUX, LTDC, DMA2D, SAI and assorted stubs.
 *
 * The model is deliberately behavioural rather than cycle-accurate: registers
 * read back what was written, "ready"/"done" status bits assert immediately,
 * and only the pieces the firmware actually depends on are simulated.
 */

#include "gw.h"
#include <time.h>

/* IRQ numbers (STM32H7 NVIC layout) */
#define IRQ_EXTI0        6
#define IRQ_EXTI1        7
#define IRQ_EXTI2        8
#define IRQ_EXTI3        9
#define IRQ_EXTI4       10
#define IRQ_DMA1_S0     11
#define IRQ_EXTI9_5     23
#define IRQ_TIM1_UP     25
#define IRQ_TIM2        28
#define IRQ_TIM3        29
#define IRQ_TIM4        30
#define IRQ_EXTI15_10   40
#define IRQ_TIM5        50
#define IRQ_TIM6_DAC    54
#define IRQ_TIM7        55
#define IRQ_DMA2_S0     56
#define IRQ_SAI1        87
#define IRQ_LTDC        88
#define IRQ_DMA2D       90
#define IRQ_SAI2        91
#define IRQ_OCTOSPI1    92
#define IRQ_LPTIM1      93

/* ================================================================== */
/* generic register-file helper                                        */
/* ================================================================== */

typedef struct {
    const char *name;
    u32 base, size;
    u32 *regs;
} RegFile;

static u32 rf_read(RegFile *f, u32 off) { return (off / 4 < f->size / 4) ? f->regs[off / 4] : 0; }
static void rf_write(RegFile *f, u32 off, u32 v) { if (off / 4 < f->size / 4) f->regs[off / 4] = v; }

/* ================================================================== */
/* RCC                                                                 */
/* ================================================================== */

static u32 rcc_regs[0x400 / 4];
static u32 bdcr_r, bdcr_w;

static u32 rcc_read(u32 off, int size)
{
    (void)size;
    if (off >= sizeof rcc_regs) return 0;
    u32 v = rcc_regs[off / 4];
    if (off == 0x00) {                    /* RCC_CR: oscillators are ready at once */
        if (v & (1u << 0))  v |= 1u << 2;     /* HSION  -> HSIRDY  */
        if (v & (1u << 7))  v |= 1u << 8;     /* CSION  -> CSIRDY  */
        if (v & (1u << 12)) v |= 1u << 13;    /* HSI48ON-> HSI48RDY*/
        if (v & (1u << 16)) v |= 1u << 17;    /* HSEON  -> HSERDY  */
        if (v & (1u << 24)) v |= 1u << 25;    /* PLL1ON -> PLL1RDY */
        if (v & (1u << 26)) v |= 1u << 27;
        if (v & (1u << 28)) v |= 1u << 29;
        v |= (1u << 14) | (1u << 15);         /* CPUCKRDY / CDCKRDY */
        v |= (1u << 5);                       /* HSIDIVF */
    } else if (off == 0x10) {             /* RCC_CFGR: SWS follows SW */
        v = (v & ~(7u << 3)) | ((v & 7) << 3);
    } else if (off == 0x70) {             /* RCC_BDCR */
        bdcr_r++;
        if (v & 1) v |= 2;                    /* LSEON -> LSERDY */
    } else if (off == 0x74) {             /* RCC_CSR */
        if (v & 1) v |= 2;                    /* LSION -> LSIRDY */
    }
    return v;
}

static void rcc_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off == 0x70) {                    /* RCC_BDCR */
        bdcr_w++;
        if ((val & (1u << 16)) && !(rcc_regs[0x70 / 4] & (1u << 16))) {
            gwlog("[rcc] backup domain reset (BDRST)\n");
            rtc_backup_reset();
        }
    }
    if (off < sizeof rcc_regs) rcc_regs[off / 4] = val;
}

u32 rcc_get_bdcr(void) { return rcc_regs[0x70 / 4]; }
void rcc_set_bdcr(u32 v) { rcc_regs[0x70 / 4] = v; }

void rcc_init(void)
{
    u32 bdcr = rcc_regs[0x70 / 4];        /* backup domain: survives reset */
    memset(rcc_regs, 0, sizeof rcc_regs);
    rcc_regs[0x70 / 4] = bdcr;
    rcc_regs[0] = 0x00000025;             /* HSION + HSIRDY + HSIDIVF */
    periph_register(0x58024400, 0x400, "RCC", rcc_read, rcc_write);
}

/* ================================================================== */
/* PWR                                                                 */
/* ================================================================== */

static u32 pwr_regs[0x100 / 4];

static u32 pwr_read(u32 off, int size)
{
    (void)size;
    if (off >= sizeof pwr_regs) return 0;
    u32 v = pwr_regs[off / 4];
    switch (off) {
    case 0x04: v |= (1u << 13) | (1u << 16) | (1u << 17); break;  /* CSR1: ACTVOSRDY etc */
    case 0x0C: v |= (1u << 13); break;                            /* CR3: SDEXTRDY */
    case 0x18: v |= (1u << 13); break;                            /* SRDCR: VOSRDY */
    }
    return v;
}

static void pwr_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off >= sizeof pwr_regs) return;
    if (off == 0x10) {                        /* PWR_CPUCR */
        /* CSSF clears the Standby and Stop flags; without this every reboot
           still looks like a wake from Standby and the firmware powers on. */
        if (val & (1u << 9)) val &= ~((1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9));
        pwr_regs[off / 4] = val;
        return;
    }
    pwr_regs[off / 4] = val;
}

void pwr_init(void)
{
    memset(pwr_regs, 0, sizeof pwr_regs);
    pwr_regs[0x0C / 4] = 0x00000006;
    periph_register(0x58024800, 0x400, "PWR", pwr_read, pwr_write);
}

/* ================================================================== */
/* embedded FLASH interface                                            */
/* ================================================================== */

static u32 flash_regs[0x200 / 4];

static u32 flashif_read(u32 off, int size)
{
    (void)size;
    if (off >= sizeof flash_regs) return 0;
    u32 v = flash_regs[off / 4];
    if (off == 0x10 || off == 0x110) v &= ~(1u << 0);   /* SR: never busy */
    if (off == 0x1C || off == 0x11C) v |= 0x40000000;   /* OPTSR_CUR: OPT_BUSY clear */
    return v;
}

static void flashif_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off < sizeof flash_regs) flash_regs[off / 4] = val;
}

void flashif_init(void)
{
    memset(flash_regs, 0, sizeof flash_regs);
    periph_register(0x52002000, 0x400, "FLASH", flashif_read, flashif_write);
}

/* ================================================================== */
/* GPIO                                                                */
/* ================================================================== */

#define NGPIO 11                          /* GPIOA..GPIOK */

typedef struct {
    u32 moder, otyper, ospeedr, pupdr, odr, lckr, afrl, afrh;
    u16 input;                            /* externally driven level, 1 = high */
} Gpio;

static Gpio gpios[NGPIO];

/* Buttons are active-low with pull-ups: idle reads high. */
void gpio_set_button(int port, int pin, bool pressed)
{
    if (port < 0 || port >= NGPIO || pin < 0 || pin > 15) return;
    if (pressed) gpios[port].input &= ~(1u << pin);
    else gpios[port].input |= (1u << pin);
    pwr_check_wakeup();
}

/* PWR wakeup pins (STM32H7 mapping). Asserting one of these resumes the CPU
   from STOP mode, which is how the Game & Watch's POWER button turns it on. */
static const struct { int port, pin; } wkup_pin[6] = {
    { 0, 0 },   /* WKUP1 = PA0  (POWER) */
    { 0, 2 },   /* WKUP2 = PA2  */
    { 8, 8 },   /* WKUP3 = PI8  */
    { 2, 13 },  /* WKUP4 = PC13 */
    { 8, 11 },  /* WKUP5 = PI11 */
    { 2, 1 },   /* WKUP6 = PC1  */
};

void pwr_check_wakeup(void)
{
    u32 epr = pwr_regs[0x28 / 4];
    for (int i = 0; i < 6; i++) {
        if (!(epr & (1u << i))) continue;                  /* WKUPEN */
        bool level = (gpios[wkup_pin[i].port].input >> wkup_pin[i].pin) & 1;
        bool active_low = (epr >> (8 + i)) & 1;            /* WKUPP polarity */
        if (!(active_low ? !level : level)) continue;

        pwr_regs[0x24 / 4] |= (1u << i);                   /* WKUPFR */
        if (!cpu.sleeping) continue;

        /* Deep sleep with PDDS set is Standby: waking from it resets the
           system, and the firmware reads SBF to learn why it restarted. */
        bool deep = (cpu.scr & 4) != 0;
        bool pdds = (pwr_regs[0x10 / 4] & 0x5) != 0;
        if (deep && pdds) {
            gwlog("[pwr] wakeup pin %d asserted; leaving Standby via system reset\n", i + 1);
            system_reset();
            pwr_regs[0x10 / 4] |= (1u << 6);               /* CPUCR.SBF */
            pwr_regs[0x24 / 4] |= (1u << i);
            pwr_regs[0x28 / 4] = epr;                      /* backup domain survives */
        } else {
            gwlog("[pwr] wakeup pin %d asserted; resuming from STOP\n", i + 1);
            cpu.sleeping = false;
        }
        return;
    }
}

/* Has the firmware put the machine into Standby (its "off" state)? */
bool pwr_in_standby(void)
{
    return cpu.sleeping && (cpu.scr & 4) && (pwr_regs[0x10 / 4] & 0x5);
}

/* Standby wake behaves like a power-on reset for everything except the
   backup domain, which the PWR/RTC models keep. PWR itself is carried across
   so that the firmware can see why it woke. */
void system_reset(void)
{
    u32 saved_pwr[sizeof pwr_regs / 4];
    memcpy(saved_pwr, pwr_regs, sizeof pwr_regs);
    periph_init();
    memcpy(pwr_regs, saved_pwr, sizeof pwr_regs);
    cpu_reset();
}

/* Reset menu item: comes up as from cold, PWR included, so the firmware
   drops into Standby waiting for POWER. The backup domain still survives -
   it is battery-backed, and pulling the cell is not what "reset" means. */
void system_cold_reset(void)
{
    periph_init();
    cpu_reset();
}

static u32 gpio_idr(Gpio *g)
{
    u32 v = 0;
    for (int i = 0; i < 16; i++) {
        u32 mode = (g->moder >> (i * 2)) & 3;
        if (mode == 1) v |= ((g->odr >> i) & 1) << i;     /* output: read back ODR */
        else v |= ((g->input >> i) & 1) << i;             /* input / AF / analog */
    }
    return v;
}

static int gpio_index(u32 base) { return (int)((base - 0x58020000u) / 0x400u); }

/* Record which code polls which input port, so the button wiring can be
   identified from a run instead of guessed. */
static struct { u32 pc; u8 port; } idr_sites[64];
static int n_idr_sites;
static u64 idr_reads[NGPIO];

static void note_idr_read(int n)
{
    idr_reads[n]++;
    for (int i = 0; i < n_idr_sites; i++)
        if (idr_sites[i].pc == cpu.pc && idr_sites[i].port == n) return;
    if (n_idr_sites < 64) { idr_sites[n_idr_sites].pc = cpu.pc; idr_sites[n_idr_sites].port = (u8)n; n_idr_sites++; }
}

static u32 gpio_read_n(int n, u32 off)
{
    Gpio *g = &gpios[n];
    if (off == 0x10) note_idr_read(n);
    switch (off) {
    case 0x00: return g->moder;
    case 0x04: return g->otyper;
    case 0x08: return g->ospeedr;
    case 0x0C: return g->pupdr;
    case 0x10: return gpio_idr(g);
    case 0x14: return g->odr;
    case 0x1C: return g->lckr;
    case 0x20: return g->afrl;
    case 0x24: return g->afrh;
    }
    return 0;
}

static void gpio_write_n(int n, u32 off, u32 val)
{
    Gpio *g = &gpios[n];
    switch (off) {
    case 0x00: g->moder = val; return;
    case 0x04: g->otyper = val; return;
    case 0x08: g->ospeedr = val; return;
    case 0x0C: g->pupdr = val; return;
    case 0x14: g->odr = val & 0xFFFF; return;
    case 0x18: g->odr = (g->odr | (val & 0xFFFF)) & ~(val >> 16); return;   /* BSRR */
    case 0x1C: g->lckr = val; return;
    case 0x20: g->afrl = val; return;
    case 0x24: g->afrh = val; return;
    }
}

#define GPIO_PORT(L, N) \
    static u32 gpio##L##_read(u32 off, int size) { (void)size; return gpio_read_n(N, off); } \
    static void gpio##L##_write(u32 off, u32 val, int size) { (void)size; gpio_write_n(N, off, val); }
GPIO_PORT(A, 0) GPIO_PORT(B, 1) GPIO_PORT(C, 2) GPIO_PORT(D, 3)
GPIO_PORT(E, 4) GPIO_PORT(F, 5) GPIO_PORT(G, 6) GPIO_PORT(H, 7)
GPIO_PORT(I, 8) GPIO_PORT(J, 9) GPIO_PORT(K, 10)

void gpio_init(void)
{
    u16 saved[NGPIO];
    static bool have_saved;
    for (int i = 0; i < NGPIO; i++) saved[i] = gpios[i].input;
    memset(gpios, 0, sizeof gpios);
    /* Pin levels are driven from outside the chip: they survive a reset. */
    for (int i = 0; i < NGPIO; i++) gpios[i].input = have_saved ? saved[i] : 0xFFFF;
    /* PC5 is charger detect, active low. The emulated unit is always on the
       charger, so the firmware never shows a flat battery. */
    if (!have_saved) gpios[2].input &= (u16)~(1u << 5);
    have_saved = true;
    periph_register(0x58020000, 0x400, "GPIOA", gpioA_read, gpioA_write);
    periph_register(0x58020400, 0x400, "GPIOB", gpioB_read, gpioB_write);
    periph_register(0x58020800, 0x400, "GPIOC", gpioC_read, gpioC_write);
    periph_register(0x58020C00, 0x400, "GPIOD", gpioD_read, gpioD_write);
    periph_register(0x58021000, 0x400, "GPIOE", gpioE_read, gpioE_write);
    periph_register(0x58021400, 0x400, "GPIOF", gpioF_read, gpioF_write);
    periph_register(0x58021800, 0x400, "GPIOG", gpioG_read, gpioG_write);
    periph_register(0x58021C00, 0x400, "GPIOH", gpioH_read, gpioH_write);
    periph_register(0x58022000, 0x400, "GPIOI", gpioI_read, gpioI_write);
    periph_register(0x58022400, 0x400, "GPIOJ", gpioJ_read, gpioJ_write);
    periph_register(0x58022800, 0x400, "GPIOK", gpioK_read, gpioK_write);
    (void)gpio_index;
}

/* ================================================================== */
/* EXTI                                                                */
/* ================================================================== */

static struct { u32 rtsr, ftsr, swier, imr, emr, pr; } exti;

void exti_trigger(int line)
{
    if (line < 0 || line > 31) return;
    if (!(exti.imr & (1u << line))) return;
    exti.pr |= 1u << line;
    if (line < 5) cpu_raise_irq(IRQ_EXTI0 + line, true);
    else if (line < 10) cpu_raise_irq(IRQ_EXTI9_5, true);
    else if (line < 16) cpu_raise_irq(IRQ_EXTI15_10, true);
}

static u32 exti_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x00: return exti.rtsr;
    case 0x04: return exti.ftsr;
    case 0x08: return exti.swier;
    case 0x80: return exti.imr;
    case 0x84: return exti.emr;
    case 0x88: return exti.pr;
    }
    return 0;
}

static void exti_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    case 0x00: exti.rtsr = val; return;
    case 0x04: exti.ftsr = val; return;
    case 0x08:
        exti.swier = val;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) exti_trigger(i);
        return;
    case 0x80: exti.imr = val; return;
    case 0x84: exti.emr = val; return;
    case 0x88: exti.pr &= ~val; return;
    }
}

void exti_init(void)
{
    memset(&exti, 0, sizeof exti);
    periph_register(0x58000000, 0x400, "EXTI", exti_read, exti_write);
}

/* ================================================================== */
/* SYSCFG                                                              */
/* ================================================================== */

static u32 syscfg_regs[0x400 / 4];
static u32 syscfg_read(u32 off, int size) { (void)size; return off < sizeof syscfg_regs ? syscfg_regs[off / 4] : 0; }
static void syscfg_write(u32 off, u32 val, int size) { (void)size; if (off < sizeof syscfg_regs) syscfg_regs[off / 4] = val; }

void syscfg_init(void)
{
    memset(syscfg_regs, 0, sizeof syscfg_regs);
    periph_register(0x58000400, 0x400, "SYSCFG", syscfg_read, syscfg_write);
}

/* ================================================================== */
/* timers                                                              */
/* ================================================================== */

typedef struct {
    const char *name;
    u32 base;
    int irq;
    u32 cr1, cr2, smcr, dier, sr, egr, ccmr1, ccmr2, ccer;
    u32 cnt, psc, arr, rcr;
    u32 ccr[4];
    u32 bdtr, dcr, dmar;
    u32 prescale_acc;
} Timer;

#define NTIMERS 12
static Timer timers[NTIMERS];

static u32 tim_read(Timer *t, u32 off)
{
    switch (off) {
    case 0x00: return t->cr1;
    case 0x04: return t->cr2;
    case 0x08: return t->smcr;
    case 0x0C: return t->dier;
    case 0x10: return t->sr;
    case 0x18: return t->ccmr1;
    case 0x1C: return t->ccmr2;
    case 0x20: return t->ccer;
    case 0x24: return t->cnt;
    case 0x28: return t->psc;
    case 0x2C: return t->arr;
    case 0x30: return t->rcr;
    case 0x34: return t->ccr[0];
    case 0x38: return t->ccr[1];
    case 0x3C: return t->ccr[2];
    case 0x40: return t->ccr[3];
    case 0x44: return t->bdtr;
    case 0x48: return t->dcr;
    case 0x4C: return t->dmar;
    }
    return 0;
}

static void tim_write(Timer *t, u32 off, u32 val)
{
    switch (off) {
    case 0x00: t->cr1 = val; return;
    case 0x04: t->cr2 = val; return;
    case 0x08: t->smcr = val; return;
    case 0x0C: t->dier = val; return;
    case 0x10: t->sr &= val; return;                    /* write 0 to clear */
    case 0x14:                                          /* EGR */
        if (val & 1) { t->cnt = 0; t->sr |= 1; }
        return;
    case 0x18: t->ccmr1 = val; return;
    case 0x1C: t->ccmr2 = val; return;
    case 0x20: t->ccer = val; return;
    case 0x24: t->cnt = val; return;
    case 0x28: t->psc = val; return;
    case 0x2C: t->arr = val; return;
    case 0x30: t->rcr = val; return;
    case 0x34: t->ccr[0] = val; return;
    case 0x38: t->ccr[1] = val; return;
    case 0x3C: t->ccr[2] = val; return;
    case 0x40: t->ccr[3] = val; return;
    case 0x44: t->bdtr = val; return;
    case 0x48: t->dcr = val; return;
    case 0x4C: t->dmar = val; return;
    }
}

#define TIMER_ACCESSOR(N) \
    static u32 tim##N##_read(u32 off, int size) { (void)size; return tim_read(&timers[N], off); } \
    static void tim##N##_write(u32 off, u32 val, int size) { (void)size; tim_write(&timers[N], off, val); }
TIMER_ACCESSOR(0) TIMER_ACCESSOR(1) TIMER_ACCESSOR(2) TIMER_ACCESSOR(3)
TIMER_ACCESSOR(4) TIMER_ACCESSOR(5) TIMER_ACCESSOR(6) TIMER_ACCESSOR(7)
TIMER_ACCESSOR(8) TIMER_ACCESSOR(9) TIMER_ACCESSOR(10) TIMER_ACCESSOR(11)

static const periph_read_fn tim_readers[NTIMERS] = {
    tim0_read, tim1_read, tim2_read, tim3_read, tim4_read, tim5_read,
    tim6_read, tim7_read, tim8_read, tim9_read, tim10_read, tim11_read };
static const periph_write_fn tim_writers[NTIMERS] = {
    tim0_write, tim1_write, tim2_write, tim3_write, tim4_write, tim5_write,
    tim6_write, tim7_write, tim8_write, tim9_write, tim10_write, tim11_write };

static const struct { const char *name; u32 base; int irq; } tim_defs[NTIMERS] = {
    { "TIM1",  0x40010000, IRQ_TIM1_UP },
    { "TIM2",  0x40000000, IRQ_TIM2 },
    { "TIM3",  0x40000400, IRQ_TIM3 },
    { "TIM4",  0x40000800, IRQ_TIM4 },
    { "TIM5",  0x40000C00, IRQ_TIM5 },
    { "TIM6",  0x40001000, IRQ_TIM6_DAC },
    { "TIM7",  0x40001400, IRQ_TIM7 },
    { "TIM8",  0x40010400, 46 },
    { "TIM12", 0x40001800, 43 },
    { "TIM13", 0x40001C00, 44 },
    { "TIM14", 0x40002000, 45 },
    { "TIM15", 0x40014000, 116 },
};

void tim_init(void)
{
    memset(timers, 0, sizeof timers);
    for (int i = 0; i < NTIMERS; i++) {
        timers[i].name = tim_defs[i].name;
        timers[i].base = tim_defs[i].base;
        timers[i].irq = tim_defs[i].irq;
        timers[i].arr = 0xFFFF;
        periph_register(tim_defs[i].base, 0x400, tim_defs[i].name,
                        tim_readers[i], tim_writers[i]);
    }
}

void tim_tick(u32 cycles)
{
    for (int i = 0; i < NTIMERS; i++) {
        Timer *t = &timers[i];
        if (!(t->cr1 & 1)) continue;                    /* CEN */
        u32 div = t->psc + 1;
        t->prescale_acc += cycles;
        u32 ticks = t->prescale_acc / div;
        if (!ticks) continue;
        t->prescale_acc -= ticks * div;
        u32 arr = t->arr ? t->arr : 0xFFFF;
        u64 c = (u64)t->cnt + ticks;
        if (c > arr) {
            t->sr |= 1;                                 /* UIF */
            c %= (u64)arr + 1;
            if ((t->dier & 1) && !(t->cr1 & (1u << 1))) cpu_raise_irq(t->irq, true);
        }
        t->cnt = (u32)c;
    }
}

/* ================================================================== */
/* DMA (DMA1/DMA2, 8 streams each) + DMAMUX                            */
/* ================================================================== */

typedef struct {
    u32 cr, ndtr, par, m0ar, m1ar, fcr;
    u32 remaining;
} DmaStream;

typedef struct {
    const char *name;
    u32 base;
    int irq0;
    u32 isr[2];                                          /* LISR, HISR */
    DmaStream s[8];
} DmaCtrl;

static DmaCtrl dma[2];

/* Flag bit position within LISR/HISR for stream n (0..3 within each word) */
static const u8 dma_flag_shift[4] = { 0, 6, 16, 22 };

static void dma_set_flag(DmaCtrl *d, int stream, int bit)
{
    int w = stream / 4, k = stream % 4;
    d->isr[w] |= 1u << (dma_flag_shift[k] + bit);
}

/* NVIC vectors per controller/stream (STM32H7 layout). */
static const u8 dma_irq[2][8] = {
    { 11, 12, 13, 14, 15, 16, 17, 47 },
    { 56, 57, 58, 59, 60, 68, 69, 70 },
};

/* Run one full transfer for a stream (the emulator has no bus contention,
   so a triggered transfer completes immediately). */
static void dma_run_stream(DmaCtrl *d, int n)
{
    DmaStream *s = &d->s[n];
    if (!(s->cr & 1)) return;

    u32 dir = (s->cr >> 6) & 3;               /* 0 p->m, 1 m->p, 2 m->m */
    u32 psize = 1u << ((s->cr >> 11) & 3);
    u32 msize = 1u << ((s->cr >> 13) & 3);
    bool pinc = (s->cr >> 9) & 1;
    bool minc = (s->cr >> 10) & 1;
    bool circ = (s->cr >> 8) & 1;

    u32 count = s->ndtr;
    u32 pa = s->par, ma = s->m0ar;
    if ((s->cr >> 19) & 1) ma = s->m1ar;      /* CT: current target */

    for (u32 i = 0; i < count; i++) {
        u32 v;
        if (dir == 0) {                       /* peripheral -> memory */
            v = (psize == 1) ? bus_read8(pa) : (psize == 2) ? bus_read16(pa) : bus_read32(pa);
            if (msize == 1) bus_write8(ma, v); else if (msize == 2) bus_write16(ma, v); else bus_write32(ma, v);
        } else {                              /* memory -> peripheral (or m2m) */
            v = (msize == 1) ? bus_read8(ma) : (msize == 2) ? bus_read16(ma) : bus_read32(ma);
            if (psize == 1) bus_write8(pa, v); else if (psize == 2) bus_write16(pa, v); else bus_write32(pa, v);
        }
        if (pinc) pa += psize;
        if (minc) ma += msize;
    }

    dma_set_flag(d, n, 4);                    /* HTIF */
    dma_set_flag(d, n, 5);                    /* TCIF */
    if (s->cr & (1u << 3)) cpu_raise_irq(dma_irq[d == &dma[1]][n], true);   /* HTIE */
    if (s->cr & (1u << 4)) cpu_raise_irq(dma_irq[d == &dma[1]][n], true);   /* TCIE */

    bool dbm = (s->cr >> 18) & 1;
    if (!circ && !dbm) s->cr &= ~1u;          /* single shot: clear EN */
    else if (dbm) s->cr ^= 1u << 19;          /* double buffer: flip CT */
}

static u32 dma_read(DmaCtrl *d, u32 off)
{
    if (off == 0x00) return d->isr[0];
    if (off == 0x04) return d->isr[1];
    if (off >= 0x10 && off < 0x10 + 8 * 0x18) {
        int n = (off - 0x10) / 0x18;
        u32 r = (off - 0x10) % 0x18;
        DmaStream *s = &d->s[n];
        switch (r) {
        case 0x00: return s->cr;
        case 0x04: return s->ndtr;
        case 0x08: return s->par;
        case 0x0C: return s->m0ar;
        case 0x10: return s->m1ar;
        case 0x14: return s->fcr;
        }
    }
    return 0;
}

static void dma_write(DmaCtrl *d, u32 off, u32 val)
{
    if (off == 0x08) { d->isr[0] &= ~val; return; }      /* LIFCR */
    if (off == 0x0C) { d->isr[1] &= ~val; return; }      /* HIFCR */
    if (off >= 0x10 && off < 0x10 + 8 * 0x18) {
        int n = (off - 0x10) / 0x18;
        u32 r = (off - 0x10) % 0x18;
        DmaStream *s = &d->s[n];
        switch (r) {
        case 0x00: {
            bool was = s->cr & 1;
            s->cr = val;
            if (!was && (val & 1)) {
                u32 dir = (val >> 6) & 3;
                /* memory-to-memory transfers are self-triggering; peripheral
                   transfers wait for their request (driven by the peripheral) */
                if (dir == 2) dma_run_stream(d, n);
            }
            return;
        }
        case 0x04: s->ndtr = val; return;
        case 0x08: s->par = val; return;
        case 0x0C: s->m0ar = val; return;
        case 0x10: s->m1ar = val; return;
        case 0x14: s->fcr = val; return;
        }
    }
}

static u32 dma1_read(u32 off, int size) { (void)size; return dma_read(&dma[0], off); }
static void dma1_write(u32 off, u32 val, int size) { (void)size; dma_write(&dma[0], off, val); }
static u32 dma2_read(u32 off, int size) { (void)size; return dma_read(&dma[1], off); }
static void dma2_write(u32 off, u32 val, int size) { (void)size; dma_write(&dma[1], off, val); }

static u32 dmamux_regs[0x400 / 4];
static u32 dmamux_read(u32 off, int size)
{
    (void)size;
    if (off >= 0x80 && off < 0x90) return 0;             /* channel status: no events */
    return off < sizeof dmamux_regs ? dmamux_regs[off / 4] : 0;
}
static void dmamux_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off < sizeof dmamux_regs) dmamux_regs[off / 4] = val;
}

/* Run whichever enabled stream is wired to this peripheral register. */
bool dma_service_par(u32 par)
{
    for (int c = 0; c < 2; c++)
        for (int n = 0; n < 8; n++)
            if ((dma[c].s[n].cr & 1) && dma[c].s[n].par == par) {
                dma_run_stream(&dma[c], n);
                return true;
            }
    return false;
}

/* Called by peripherals that want their DMA request serviced. */
void dma_service_request(int ctrl, int stream)
{
    if (ctrl < 0 || ctrl > 1 || stream < 0 || stream > 7) return;
    dma_run_stream(&dma[ctrl], stream);
}

void dma_init(void)
{
    memset(dma, 0, sizeof dma);
    dma[0].name = "DMA1"; dma[0].base = 0x40020000; dma[0].irq0 = IRQ_DMA1_S0;
    dma[1].name = "DMA2"; dma[1].base = 0x40020400; dma[1].irq0 = IRQ_DMA2_S0;
    periph_register(0x40020000, 0x400, "DMA1", dma1_read, dma1_write);
    periph_register(0x40020400, 0x400, "DMA2", dma2_read, dma2_write);
    periph_register(0x40020800, 0x400, "DMAMUX1", dmamux_read, dmamux_write);
}

void dma_tick(u32 cycles) { (void)cycles; }

/* ================================================================== */
/* LTDC                                                                */
/* ================================================================== */

u32 ltdc_framebuffer[320 * 240];
int ltdc_width = 320, ltdc_height = 240;
volatile int ltdc_frame_counter;

typedef struct {
    u32 cr, whpcr, wvpcr, ckcr, pfcr, cacr, dccr, bfcr;
    u32 cfbar, cfblr, cfblnr;
    u32 clut[256];
} LtdcLayer;

static struct {
    u32 sscr, bpcr, awcr, twcr, gcr, srcr, bccr, ier, isr, lipcr, cpsr, cdsr;
    LtdcLayer layer[2];
    u32 line_acc;
} ltdc;

static inline u32 rgb565(u16 v)
{
    u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Decode one source pixel into 0xAARRGGBB. */
static u32 ltdc_texel(const u8 *p, u32 fmt, const u32 *clut)
{
    switch (fmt) {
    case 0: { u32 v; memcpy(&v, p, 4); return v; }                       /* ARGB8888 */
    case 1: return 0xFF000000u | p[0] | (p[1] << 8) | (p[2] << 16);      /* RGB888   */
    case 2: { u16 v; memcpy(&v, p, 2); return rgb565(v); }               /* RGB565   */
    case 3: { u16 v; memcpy(&v, p, 2);                                   /* ARGB1555 */
              u32 r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
              return ((v & 0x8000) ? 0xFF000000u : 0u) |
                     ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2); }
    case 4: { u16 v; memcpy(&v, p, 2);                                   /* ARGB4444 */
              u32 a = (v >> 12) & 0xF, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
              return ((a * 17) << 24) | ((r * 17) << 16) | ((g * 17) << 8) | (b * 17); }
    case 5: return 0xFF000000u | (clut[p[0]] & 0xFFFFFF);                /* L8       */
    case 6: {                                                            /* AL44     */
        u32 a = (p[0] >> 4) * 17, l = p[0] & 0x0F;
        return (a << 24) | (clut[l * 0x11] & 0xFFFFFF);   /* CLUT programmed at L*0x11 */
    }
    case 7: {                                                            /* AL88     */
        u16 v; memcpy(&v, p, 2);
        return ((u32)(v >> 8) << 24) | (clut[v & 0xFF] & 0xFFFFFF);
    }
    default: return 0;
    }
}

static void ltdc_compose(void)
{
    /* Active area from AWCR/BPCR gives the visible resolution. */
    u32 aw = ((ltdc.awcr >> 16) & 0xFFF), ah = (ltdc.awcr & 0xFFF);
    u32 bpw = ((ltdc.bpcr >> 16) & 0xFFF), bph = (ltdc.bpcr & 0xFFF);
    int w = (int)(aw - bpw), h = (int)(ah - bph);
    if (w <= 0 || h <= 0 || w > 320 || h > 240) { w = 320; h = 240; }
    ltdc_width = w; ltdc_height = h;

    u32 bg = ltdc.bccr & 0xFFFFFF;
    for (int i = 0; i < w * h; i++) ltdc_framebuffer[i] = 0xFF000000u | bg;

    for (int li = 0; li < 2; li++) {
        LtdcLayer *L = &ltdc.layer[li];
        if (!(L->cr & 1)) continue;                       /* LEN */
        u32 fmt = L->pfcr & 7;
        u32 pitch = (L->cfblr >> 16) & 0x1FFF;
        u32 lines = L->cfblnr & 0x7FF;
        u32 x0 = (L->whpcr & 0xFFF), x1 = ((L->whpcr >> 16) & 0xFFF);
        u32 y0 = (L->wvpcr & 0xFFF);
        int lx = (int)x0 - (int)bpw - 1;
        int ly = (int)y0 - (int)bph - 1;
        int lw = (int)(x1 - x0) + 1;
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (lw <= 0 || lw > w) lw = w;
        if (lines == 0 || lines > (u32)h) lines = h;

        static const u8 bpp_of[8] = { 4, 3, 2, 2, 2, 1, 1, 2 };
        u32 bpp = bpp_of[fmt];
        u32 stride = pitch ? pitch : (u32)lw * bpp;
        u8 *fb = bus_host_ptr(L->cfbar, stride * lines);
        if (!fb) continue;

        /* Blend factors: BF1 selects constant alpha or pixel-alpha x constant. */
        u32 ca = L->cacr & 0xFF;
        u32 bf1 = (L->bfcr >> 8) & 7;
        bool use_pixel_alpha = (bf1 == 6);

        for (u32 y = 0; y < lines && (int)y + ly < h; y++) {
            const u8 *row = fb + (size_t)y * stride;
            u32 *dst = &ltdc_framebuffer[(size_t)(y + ly) * w + lx];
            for (int x = 0; x < lw && x + lx < w; x++) {
                u32 c = ltdc_texel(row + (size_t)x * bpp, fmt, L->clut);
                u32 a = use_pixel_alpha ? ((c >> 24) * ca) / 255 : ca;
                if (a == 0) continue;
                if (a >= 255) { dst[x] = 0xFF000000u | (c & 0xFFFFFF); continue; }
                u32 d = dst[x];
                u32 r = (((c >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
                u32 g = (((c >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
                u32 b = ((c & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    ltdc_frame_counter++;
}

static u32 ltdc_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x08: return ltdc.sscr;
    case 0x0C: return ltdc.bpcr;
    case 0x10: return ltdc.awcr;
    case 0x14: return ltdc.twcr;
    case 0x18: return ltdc.gcr | (1u << 0);
    case 0x24: return ltdc.srcr;
    case 0x2C: return ltdc.bccr;
    case 0x34: return ltdc.ier;
    case 0x38: return ltdc.isr;
    case 0x40: return ltdc.lipcr;
    case 0x44: return ltdc.cpsr;
    case 0x48: return ltdc.cdsr | 0xF;
    }
    if (off >= 0x84) {
        int li = (off >= 0x104) ? 1 : 0;
        u32 r = off - (li ? 0x104 : 0x84);
        LtdcLayer *L = &ltdc.layer[li];
        switch (r) {
        case 0x00: return L->cr;
        case 0x04: return L->whpcr;
        case 0x08: return L->wvpcr;
        case 0x0C: return L->ckcr;
        case 0x10: return L->pfcr;
        case 0x14: return L->cacr;
        case 0x18: return L->dccr;
        case 0x1C: return L->bfcr;
        case 0x28: return L->cfbar;
        case 0x2C: return L->cfblr;
        case 0x30: return L->cfblnr;
        }
    }
    return 0;
}

static int ltdc_log;
static void ltdc_write(u32 off, u32 val, int size)
{
    (void)size;
    if (opt_log_periph && ltdc_log < 200) {
        ltdc_log++;
        gwlog("[ltdc] wr %03x = %08x pc=%08x\n", off, val, cpu.pc);
    }
    switch (off) {
    case 0x08: ltdc.sscr = val; return;
    case 0x0C: ltdc.bpcr = val; return;
    case 0x10: ltdc.awcr = val; return;
    case 0x14: ltdc.twcr = val; return;
    case 0x18: ltdc.gcr = val; return;
    case 0x24: ltdc.srcr = val; return;                  /* shadow reload */
    case 0x2C: ltdc.bccr = val; return;
    case 0x34: ltdc.ier = val; return;
    case 0x3C: ltdc.isr &= ~val; return;
    case 0x40: ltdc.lipcr = val; return;
    }
    if (off >= 0x84) {
        int li = (off >= 0x104) ? 1 : 0;
        u32 r = off - (li ? 0x104 : 0x84);
        LtdcLayer *L = &ltdc.layer[li];
        switch (r) {
        case 0x00: L->cr = val; return;
        case 0x04: L->whpcr = val; return;
        case 0x08: L->wvpcr = val; return;
        case 0x0C: L->ckcr = val; return;
        case 0x10: L->pfcr = val; return;
        case 0x14: L->cacr = val; return;
        case 0x18: L->dccr = val; return;
        case 0x1C: L->bfcr = val; return;
        case 0x28: L->cfbar = val; return;
        case 0x2C: L->cfblr = val; return;
        case 0x30: L->cfblnr = val; return;
        case 0x40: {                                       /* LxCLUTWR */
                L->clut[(val >> 24) & 0xFF] = val & 0xFFFFFF; return;
        }
        }
    }
}

void ltdc_init(void)
{
    memset(&ltdc, 0, sizeof ltdc);
    /* Reset values that matter: BF1=6 (pixel alpha x constant alpha), BF2=7,
       constant alpha 0xFF. Starting these at zero makes every layer opaque. */
    for (int i = 0; i < 2; i++) {
        ltdc.layer[i].bfcr = 0x0607;
        ltdc.layer[i].cacr = 0x000000FF;
    }
    periph_register(0x50001000, 0x1000, "LTDC", ltdc_read, ltdc_write);
}

/* Drive line/frame interrupts and produce a composed frame. */
void ltdc_tick(u32 cycles)
{
    if (!(ltdc.gcr & 1)) return;                          /* LTDCEN */
    ltdc.line_acc += cycles;
    /* One frame every ~1/60 s of core time. */
    extern u32 opt_core_hz;
    u32 frame_cycles = opt_core_hz / 60;
    if (frame_cycles == 0) frame_cycles = 1;
    if (ltdc.line_acc < frame_cycles) return;
    ltdc.line_acc -= frame_cycles;

    ltdc_compose();

    ltdc.isr |= 1u << 0;                                  /* LIF */
    ltdc.isr |= 1u << 3;                                  /* RRIF */
    if (ltdc.ier & 1) cpu_raise_irq(IRQ_LTDC, true);
    if (ltdc.ier & (1u << 3)) cpu_raise_irq(IRQ_LTDC, true);
    /* honour a pending shadow reload */
    if (ltdc.srcr & 3) ltdc.srcr = 0;
}

/* ================================================================== */
/* DMA2D                                                               */
/* ================================================================== */

static struct {
    u32 cr, isr, ifcr, fgmar, fgor, bgmar, bgor;
    u32 fgpfccr, fgcolr, bgpfccr, bgcolr, omar, oor, opfccr, ocolr, nlr;
    u32 fgcmar, bgcmar, lwr, amtcr;
    u32 fgclut[256], bgclut[256];
} d2d;

/* DMA2D_ISR / IFCR bits */
#define D2D_TEIF   (1u << 0)
#define D2D_TCIF   (1u << 1)
#define D2D_TWIF   (1u << 2)
#define D2D_CAEIF  (1u << 3)
#define D2D_CTCIF  (1u << 4)
#define D2D_CEIF   (1u << 5)

/* Automatic CLUT loading: DMA2D copies the palette from memory into its own
   look-up table and raises CTCIF when it is done. The firmware waits on that
   interrupt before it will issue the blit. */
static void d2d_load_clut(bool foreground)
{
    u32 pfccr = foreground ? d2d.fgpfccr : d2d.bgpfccr;
    u32 addr  = foreground ? d2d.fgcmar  : d2d.bgcmar;
    u32 *dst  = foreground ? d2d.fgclut  : d2d.bgclut;
    u32 count = ((pfccr >> 8) & 0xFF) + 1;
    bool rgb888 = (pfccr >> 4) & 1;                  /* CCM: 0 = ARGB8888 */

    u8 *src = bus_host_ptr(addr, count * (rgb888 ? 3 : 4));
    if (!src) {
        d2d.isr |= D2D_CAEIF;                        /* CLUT access error */
        if (d2d.cr & (1u << 11)) cpu_raise_irq(IRQ_DMA2D, true);
        return;
    }
    for (u32 i = 0; i < count; i++) {
        if (rgb888) dst[i] = 0xFF000000u | src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
        else memcpy(&dst[i], src + i * 4, 4);
    }
    d2d.isr |= D2D_CTCIF;
    if (d2d.cr & (1u << 12)) cpu_raise_irq(IRQ_DMA2D, true);   /* CTCIE */
}

static u32 d2d_bpp(u32 fmt)
{
    switch (fmt) {
    case 0: return 4;                 /* ARGB8888 */
    case 1: return 3;                 /* RGB888 */
    case 2: case 3: case 4: return 2; /* RGB565 / ARGB1555 / ARGB4444 */
    case 5: return 1;                 /* L8 */
    default: return 1;
    }
}

static u32 d2d_load(u8 *p, u32 fmt, const u32 *clut)
{
    switch (fmt) {
    case 0: { u32 v; memcpy(&v, p, 4); return v; }
    case 1: return 0xFF000000u | p[0] | (p[1] << 8) | (p[2] << 16);
    case 2: { u16 v; memcpy(&v, p, 2); return rgb565(v); }
    case 3: { u16 v; memcpy(&v, p, 2);
              u32 r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
              return ((v & 0x8000) ? 0xFF000000u : 0) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2); }
    case 4: { u16 v; memcpy(&v, p, 2);
              u32 a = (v >> 12) & 0xF, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
              return ((a * 17) << 24) | ((r * 17) << 16) | ((g * 17) << 8) | (b * 17); }
    case 5: return clut[*p];                  /* L8: CLUT entries are ARGB8888 */
    default: return 0xFF000000u;
    }
}

/* xPFCCR alpha mode: 0 = use source alpha, 1 = replace, 2 = multiply. */
static inline u32 d2d_apply_am(u32 c, u32 pfccr)
{
    u32 am = (pfccr >> 16) & 3;
    if (am == 0) return c;
    u32 alpha = (pfccr >> 24) & 0xFF;
    u32 a = (am == 1) ? alpha : (((c >> 24) * alpha) / 255);
    return (a << 24) | (c & 0xFFFFFF);
}

static void d2d_store(u8 *p, u32 fmt, u32 c)
{
    switch (fmt) {
    case 0: memcpy(p, &c, 4); return;
    case 1: p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16); return;
    case 2: { u16 v = (u16)((((c >> 16) & 0xFF) >> 3 << 11) | (((c >> 8) & 0xFF) >> 2 << 5) | ((c & 0xFF) >> 3));
              memcpy(p, &v, 2); return; }
    case 3: { u16 v = (u16)(((c >> 24) ? 0x8000 : 0) | (((c >> 16) & 0xFF) >> 3 << 10) |
                            (((c >> 8) & 0xFF) >> 3 << 5) | ((c & 0xFF) >> 3));
              memcpy(p, &v, 2); return; }
    case 4: { u16 v = (u16)(((c >> 28) << 12) | ((((c >> 16) & 0xFF) >> 4) << 8) |
                            ((((c >> 8) & 0xFF) >> 4) << 4) | ((c & 0xFF) >> 4));
              memcpy(p, &v, 2); return; }
    default: *p = (u8)c; return;
    }
}

static void d2d_run(void)
{
    u32 mode = (d2d.cr >> 16) & 3;
    {
        static int n;
        if (++n > 400 && n < 412)
            gwlog("[d2d] mode=%u nlr=%08x (pl=%u nl=%u) omar=%08x oor=%u opf=%u "
                  "fgmar=%08x fgor=%u fgpf=%08x bgmar=%08x bgor=%u bgpf=%08x lwr=%08x\n",
                  mode, d2d.nlr, d2d.nlr & 0x3FFF, (d2d.nlr >> 16) & 0xFFFF,
                  d2d.omar, d2d.oor, d2d.opfccr & 7, d2d.fgmar, d2d.fgor, d2d.fgpfccr,
                  d2d.bgmar, d2d.bgor, d2d.bgpfccr, d2d.lwr);
    }
    /* DMA2D_NLR: PL (pixels per line) is bits[29:16], NL (lines) is bits[15:0]. */
    u32 pixels = (d2d.nlr >> 16) & 0x3FFF;
    u32 lines = d2d.nlr & 0xFFFF;
    u32 ofmt = d2d.opfccr & 7, obpp = d2d_bpp(ofmt);
    u32 ffmt = d2d.fgpfccr & 7, fbpp = d2d_bpp(ffmt);
    u32 bfmt = d2d.bgpfccr & 7, bbpp = d2d_bpp(bfmt);

    for (u32 y = 0; y < lines; y++) {
        u32 oaddr = d2d.omar + y * (pixels + d2d.oor) * obpp;
        u8 *op = bus_host_ptr(oaddr, pixels * obpp);
        if (!op) break;
        u8 *fp = NULL, *bp = NULL;
        if (mode != 3) fp = bus_host_ptr(d2d.fgmar + y * (pixels + d2d.fgor) * fbpp, pixels * fbpp);
        if (mode == 2) bp = bus_host_ptr(d2d.bgmar + y * (pixels + d2d.bgor) * bbpp, pixels * bbpp);

        for (u32 x = 0; x < pixels; x++) {
            u32 c;
            if (mode == 3) c = d2d.ocolr;                             /* register to memory */
            else if (!fp) break;
            else {
                c = d2d_apply_am(d2d_load(fp + x * fbpp, ffmt, d2d.fgclut), d2d.fgpfccr);
                if (mode == 2 && bp) {                                /* blend */
                    u32 b = d2d_apply_am(d2d_load(bp + x * bbpp, bfmt, d2d.bgclut), d2d.bgpfccr);
                    u32 a = c >> 24;
                    u32 r = (((c >> 16) & 0xFF) * a + ((b >> 16) & 0xFF) * (255 - a)) / 255;
                    u32 g = (((c >> 8) & 0xFF) * a + ((b >> 8) & 0xFF) * (255 - a)) / 255;
                    u32 bl = ((c & 0xFF) * a + (b & 0xFF) * (255 - a)) / 255;
                    c = 0xFF000000u | (r << 16) | (g << 8) | bl;
                }
            }
            d2d_store(op + x * obpp, ofmt, c);
        }
    }

    d2d.cr &= ~1u;                                                    /* START clears */
    d2d.isr |= D2D_TCIF;
    if (d2d.cr & (1u << 9)) cpu_raise_irq(IRQ_DMA2D, true);           /* TCIE */
}

static u32 d2d_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x00: return d2d.cr;
    case 0x04: return d2d.isr;
    case 0x0C: return d2d.fgmar;
    case 0x10: return d2d.fgor;
    case 0x14: return d2d.bgmar;
    case 0x18: return d2d.bgor;
    case 0x1C: return d2d.fgpfccr;
    case 0x2C: return d2d.fgcmar;
    case 0x30: return d2d.bgcmar;
    case 0x48: return d2d.lwr;
    case 0x4C: return d2d.amtcr;
    case 0x20: return d2d.fgcolr;
    case 0x24: return d2d.bgpfccr;
    case 0x28: return d2d.bgcolr;
    case 0x3C: return d2d.omar;
    case 0x40: return d2d.oor;
    case 0x34: return d2d.opfccr;
    case 0x38: return d2d.ocolr;
    case 0x44: return d2d.nlr;
    }
    return 0;
}

static void d2d_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    case 0x00: d2d.cr = val; if (val & 1) d2d_run(); return;
    case 0x08: d2d.isr &= ~val; return;
    case 0x0C: d2d.fgmar = val; return;
    case 0x10: d2d.fgor = val; return;
    case 0x14: d2d.bgmar = val; return;
    case 0x18: d2d.bgor = val; return;
    case 0x1C:
        d2d.fgpfccr = val & ~(1u << 5);
        if (val & (1u << 5)) { d2d.fgpfccr = val & ~(1u << 5); d2d_load_clut(true); }
        return;
    case 0x2C: d2d.fgcmar = val; return;
    case 0x30: d2d.bgcmar = val; return;
    case 0x48: d2d.lwr = val; return;
    case 0x4C: d2d.amtcr = val; return;
    case 0x20: d2d.fgcolr = val; return;
    case 0x24:
        d2d.bgpfccr = val & ~(1u << 5);
        if (val & (1u << 5)) { d2d.bgpfccr = val & ~(1u << 5); d2d_load_clut(false); }
        return;
    case 0x28: d2d.bgcolr = val; return;
    case 0x34: d2d.opfccr = val; return;
    case 0x38: d2d.ocolr = val; return;
    case 0x3C: d2d.omar = val; return;
    case 0x40: d2d.oor = val; return;
    case 0x44: d2d.nlr = val; return;
    }
    if (off >= 0x400 && off < 0x800) { d2d.fgclut[(off - 0x400) / 4] = val; return; }
    if (off >= 0x800 && off < 0xC00) { d2d.bgclut[(off - 0x800) / 4] = val; return; }
}

void dma2d_init(void)
{
    memset(&d2d, 0, sizeof d2d);
    periph_register(0x52001000, 0x1000, "DMA2D", d2d_read, d2d_write);
}

/* ================================================================== */
/* SAI (audio)                                                         */
/* ================================================================== */

s16 audio_ring[AUDIO_RING];
volatile u32 audio_wr, audio_rd;

static struct {
    u32 gcr;
    struct { u32 cr1, cr2, frcr, slotr, im, sr, clrfr, dr; } blk[2];
    u32 sample_acc;
} sai;

/* Kernel clock feeding SAI1, derived from the RCC registers the firmware
   programmed rather than assumed. */
static u32 rcc_pll_ref_hz(void)
{
    u32 cr = rcc_regs[0x00 / 4];
    switch (rcc_regs[0x28 / 4] & 3) {                        /* PLLSRC */
    case 0: return 64000000u >> ((cr >> 3) & 3);             /* HSI, divided by HSIDIV */
    case 1: return 4000000u;                                 /* CSI */
    case 2: return 24000000u;                                /* HSE */
    default: return 0;
    }
}

/* PLL n (1..3): VCO = ref/DIVM * (DIVN+1); outputs P/Q/R divide that. */
static u32 rcc_pll_out_hz(int pll, char out)
{
    static const u32 divr_off[3] = { 0x30, 0x38, 0x40 };
    u32 pllckselr = rcc_regs[0x28 / 4];
    u32 divm = (pllckselr >> (4 + 8 * (pll - 1))) & 0x3F;
    if (!divm) return 0;
    u32 divr = rcc_regs[divr_off[pll - 1] / 4];
    u32 vco = (rcc_pll_ref_hz() / divm) * ((divr & 0x1FF) + 1);
    u32 d;
    switch (out) {
    case 'P': d = ((divr >> 9) & 0x7F) + 1; break;
    case 'Q': d = ((divr >> 16) & 0x7F) + 1; break;
    default:  d = ((divr >> 24) & 0x7F) + 1; break;
    }
    return d ? vco / d : 0;
}

static u32 sai1_kernel_hz(void)
{
    /* RM0455: SAI1SEL lives in RCC_CDCCIP1R at offset 0x50. */
    switch (rcc_regs[0x50 / 4] & 7) {
    case 0: return rcc_pll_out_hz(1, 'Q');                   /* pll1_q_ck */
    case 1: return rcc_pll_out_hz(2, 'P');                   /* pll2_p_ck */
    case 2: return rcc_pll_out_hz(3, 'P');                   /* pll3_p_ck */
    case 4: return rcc_pll_ref_hz();                         /* per_ck */
    default: return rcc_pll_out_hz(2, 'P');
    }
}

/* Frame (sample) rate the SAI is clocking out. */
u32 opt_audio_hz = 42969;

static void sai_update_rate(void)
{
    u32 cr1 = sai.blk[0].cr1;
    u32 mckdiv = (cr1 >> 20) & 0x3F;
    bool nodiv = (cr1 >> 19) & 1;
    u32 frl = (sai.blk[0].frcr & 0xFF) + 1;                 /* bit clocks per frame */
    u32 ker = sai1_kernel_hz();
    if (!ker || !mckdiv || !frl) return;

    /* NODIV=1: MCKDIV divides the kernel clock straight down to the bit clock.
       NODIV=0: the master clock is kernel/MCKDIV and the frame is MCLK/256. */
    u32 rate = nodiv ? ker / (mckdiv * frl) : ker / (mckdiv * 256);
    if (rate >= 4000 && rate <= 192000 && rate != opt_audio_hz) {
        opt_audio_hz = rate;
        gwlog("[sai] sample rate = %u Hz (kernel %u Hz, MCKDIV %u, FRL %u, NODIV %d)\n",
              rate, ker, mckdiv, frl, (int)nodiv);
    }
}

u64 audio_pushed;
s16 audio_min, audio_max;

static void audio_push(s16 s)
{
    audio_pushed++;
    if (s < audio_min) audio_min = s;
    if (s > audio_max) audio_max = s;
    u32 next = (audio_wr + 1) % AUDIO_RING;
    if (next == audio_rd) return;                 /* ring full: drop */
    audio_ring[audio_wr] = s;
    audio_wr = next;
}

static u32 sai_read(u32 off, int size)
{
    (void)size;
    if (off == 0x00) return sai.gcr;
    int b = (off >= 0x24) ? 1 : 0;
    u32 r = off - (b ? 0x24 : 0x04);
    switch (r) {
    case 0x00: return sai.blk[b].cr1;
    case 0x04: return sai.blk[b].cr2;
    case 0x08: return sai.blk[b].frcr;
    case 0x0C: return sai.blk[b].slotr;
    case 0x10: return sai.blk[b].im;
    case 0x14: return sai.blk[b].sr | (1u << 0);   /* OVRUDR clear, FREQ ready */
    case 0x18: return 0;
    case 0x1C: return sai.blk[b].dr;
    }
    return 0;
}

static void sai_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off == 0x00) { sai.gcr = val; return; }
    int b = (off >= 0x24) ? 1 : 0;
    u32 r = off - (b ? 0x24 : 0x04);
    switch (r) {
    case 0x00: sai.blk[b].cr1 = val; if (b == 0) sai_update_rate(); return;
    case 0x04: sai.blk[b].cr2 = val; return;
    case 0x08: sai.blk[b].frcr = val; if (b == 0) sai_update_rate(); return;
    case 0x0C: sai.blk[b].slotr = val; return;
    case 0x10: sai.blk[b].im = val; return;
    case 0x18: sai.blk[b].clrfr = val; return;
    case 0x1C: audio_push((s16)(val & 0xFFFF)); return;
    }
}

void sai_init(void)
{
    memset(&sai, 0, sizeof sai);
    audio_wr = audio_rd = 0;
    periph_register(0x40015800, 0x400, "SAI1", sai_read, sai_write);
    periph_register(0x40015C00, 0x400, "SAI2", sai_read, sai_write);
    periph_register(0x58005400, 0x400, "SAI4", sai_read, sai_write);
}

void sai_tick(u32 cycles)
{
    extern u32 opt_core_hz;
    /* The DMA moves one block per interrupt; find out how big that block is. */
    u32 block = 0;
    for (int c = 0; c < 2 && !block; c++)
        for (int n = 0; n < 8; n++)
            if ((dma[c].s[n].cr & 1) && dma[c].s[n].par == 0x40015820u) { block = dma[c].s[n].ndtr; break; }
    if (!block) return;

    u32 period = (u32)(((u64)opt_core_hz * block) / opt_audio_hz);
    if (period == 0) return;

    sai.sample_acc += cycles;
    while (sai.sample_acc >= period) {
        sai.sample_acc -= period;
        dma_service_par(0x40015820u);
    }
}

/* ================================================================== */
/* assorted stubs                                                      */
/* ================================================================== */

static u32 rng_read(u32 off, int size)
{
    (void)size;
    static u32 state = 0x12345678;
    if (off == 0x00) return 0;
    if (off == 0x04) return 1;                     /* SR: DRDY */
    if (off == 0x08) { state = state * 1103515245u + 12345u; return state; }
    return 0;
}
static void rng_write(u32 off, u32 val, int size) { (void)off; (void)val; (void)size; }

/* ------------------------------------------------------------------ */
/* RTC - the clock face polls TR/DR/SSR, so this has to actually run.   */
/* ------------------------------------------------------------------ */

#define IRQ_RTC_WKUP   3
#define IRQ_RTC_ALARM 41
#define LSE_HZ 32768u

static struct {
    u32 tr, dr, prer, wutr, cr, calr, shiftr, alrmar, alrmassr, alrmbr, alrmbssr;
    u32 icsr, sr;
    u32 wpr_state;
    bool unlocked;
    /* running time */
    int  hour, min, sec;
    int  day, month, year, wday;              /* year 0..99, month 1..12 */
    u32  subsec;                              /* counts up to (PREDIV_S+1) */
    u64  lse_frac;                            /* fractional LSE ticks      */
    u32  wut_count;
} rtc;

static u32 bcd(int v) { return (u32)(((v / 10) << 4) | (v % 10)); }
static int  unbcd(u32 v) { return (int)((((v >> 4) & 0xF) * 10) + (v & 0xF)); }

static void rtc_encode(void)
{
    int h = rtc.hour;
    u32 pm = 0;
    if (rtc.cr & (1u << 6)) {                 /* FMT: 12-hour with AM/PM */
        pm = (h >= 12) ? 1 : 0;
        h = h % 12;
        if (h == 0) h = 12;
    }
    rtc.tr = (bcd(rtc.sec) & 0x7F) | ((bcd(rtc.min) & 0x7F) << 8) |
             ((bcd(h) & 0x3F) << 16) | (pm << 22);
    rtc.dr = (bcd(rtc.day) & 0x3F) | ((bcd(rtc.month) & 0x1F) << 8) |
             ((u32)(rtc.wday ? rtc.wday : 7) << 13) | ((bcd(rtc.year) & 0xFF) << 16);
}

/* Take a TR/DR pair the firmware wrote in init mode and adopt it as the
   running time, otherwise the next tick would just overwrite it. */
static void rtc_decode(void)
{
    int h = unbcd((rtc.tr >> 16) & 0x3F);
    if (rtc.cr & (1u << 6)) {                 /* 12-hour format with PM flag */
        if (h == 12) h = 0;
        if ((rtc.tr >> 22) & 1) h += 12;
    }
    rtc.hour  = (h >= 0 && h < 24) ? h : 0;
    rtc.min   = unbcd((rtc.tr >> 8) & 0x7F) % 60;
    rtc.sec   = unbcd(rtc.tr & 0x7F) % 60;

    int d = unbcd(rtc.dr & 0x3F);
    int mo = unbcd((rtc.dr >> 8) & 0x1F);
    rtc.day   = (d >= 1 && d <= 31) ? d : 1;
    rtc.month = (mo >= 1 && mo <= 12) ? mo : 1;
    rtc.year  = unbcd((rtc.dr >> 16) & 0xFF) % 100;
    rtc.wday  = (rtc.dr >> 13) & 7;
    gwlog("[rtc] firmware set the clock to %02d:%02d:%02d %02d/%02d/%02d (pc=%08x lr=%08x)\n",
          rtc.hour, rtc.min, rtc.sec, rtc.day, rtc.month, rtc.year, cpu.pc, cpu.r[14]);
    { static int once; if (!once && opt_trace) { once = 1; cpu_dump_history("rtc set"); } }
}

static void rtc_advance_second(void)
{
    static const int mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (++rtc.sec < 60) return;
    rtc.sec = 0;
    if (++rtc.min < 60) return;
    rtc.min = 0;
    if (++rtc.hour < 24) return;
    rtc.hour = 0;
    rtc.wday = rtc.wday % 7 + 1;
    int dim = mdays[rtc.month];
    if (rtc.month == 2 && (rtc.year % 4) == 0) dim = 29;
    if (++rtc.day <= dim) return;
    rtc.day = 1;
    if (++rtc.month <= 12) return;
    rtc.month = 1;
    rtc.year = (rtc.year + 1) % 100;
}

/* Alarm A match (the firmware uses it to wake once a second/minute). */
static bool rtc_alarm_matches(u32 alrm)
{
    u32 t = rtc.tr;
    if (!(alrm & (1u << 7))  && (alrm & 0x7F)         != (t & 0x7F))         return false;
    if (!(alrm & (1u << 15)) && ((alrm >> 8) & 0x7F)  != ((t >> 8) & 0x7F))  return false;
    if (!(alrm & (1u << 23)) && ((alrm >> 16) & 0x3F) != ((t >> 16) & 0x3F)) return false;
    return true;
}

void rtc_tick(u32 cycles)
{
    extern u32 opt_core_hz;
    u32 pred_a = ((rtc.prer >> 16) & 0x7F) + 1;
    u32 pred_s = (rtc.prer & 0x7FFF) + 1;
    if (pred_a < 1 || pred_s < 1) return;

    if (rtc.icsr & (1u << 7)) return;         /* held in init mode: calendar stopped */

    /* Convert core cycles into LSE ticks without drifting. */
    rtc.lse_frac += (u64)cycles * LSE_HZ;
    u64 ticks = rtc.lse_frac / opt_core_hz;
    if (!ticks) return;
    rtc.lse_frac -= ticks * opt_core_hz;

    u32 per_sub = pred_a;                     /* LSE ticks per ck_apre pulse */
    while (ticks--) {
        static u32 apre;
        if (++apre < per_sub) continue;
        apre = 0;
        if (++rtc.subsec < pred_s) continue;
        rtc.subsec = 0;

        rtc_advance_second();
        rtc_encode();

        if ((rtc.cr & (1u << 8)) && rtc_alarm_matches(rtc.alrmar)) {   /* ALRAE */
            rtc.sr |= (1u << 0);
            if (rtc.cr & (1u << 12)) cpu_raise_irq(IRQ_RTC_ALARM, true);
        }
        if (rtc.cr & (1u << 10)) {                                     /* WUTE */
            if (rtc.wut_count == 0) rtc.wut_count = (rtc.wutr & 0xFFFF) + 1;
            if (--rtc.wut_count == 0) {
                rtc.sr |= (1u << 2);                                   /* WUTF */
                if (rtc.cr & (1u << 14)) cpu_raise_irq(IRQ_RTC_WKUP, true);
            }
        }
    }
}

static u32 rtc_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x00: return rtc.tr;
    case 0x04: return rtc.dr;
    case 0x08: {                              /* SSR counts down within a second */
        u32 pred_s = (rtc.prer & 0x7FFF) + 1;
        return (pred_s - 1 - rtc.subsec) & 0xFFFF;
    }
    case 0x0C:
        return rtc.icsr | (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5)
             | ((rtc.icsr & (1u << 7)) ? (1u << 6) : 0);
    case 0x10: return rtc.prer;
    case 0x14: return rtc.wutr;
    case 0x18: return rtc.cr;
    case 0x28: return rtc.calr;
    case 0x40: return rtc.alrmar;
    case 0x44: return rtc.alrmassr;
    case 0x48: return rtc.alrmbr;
    case 0x4C: return rtc.alrmbssr;
    case 0x50: return rtc.sr;
    case 0x54: return rtc.sr & ((rtc.cr >> 12) & 1 ? 0xFFFFFFFFu : ~1u);
    case 0x5C: return rtc.sr;
    }
    return 0;
}

static void rtc_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    /* Hardware only accepts these in init mode; accept them regardless so a
       firmware path that skips INIT still gets the clock it asked for. */
    case 0x00:
        if (!(rtc.icsr & (1u << 7))) gwlog("[rtc] TR written outside init mode\n");
        rtc.tr = val; rtc_decode(); return;
    case 0x04:
        if (!(rtc.icsr & (1u << 7))) gwlog("[rtc] DR written outside init mode\n");
        rtc.dr = val; rtc_decode(); return;
    case 0x0C: {
        bool was_init = (rtc.icsr & (1u << 7)) != 0;
        rtc.icsr = val;
        if (was_init && !(val & (1u << 7))) {
            rtc.subsec = 0;                   /* leaving init restarts the second */
            rtc_encode();
        }
        return;
    }
    case 0x10: rtc.prer = val; return;
    case 0x14: rtc.wutr = val; rtc.wut_count = 0; return;
    case 0x18: rtc.cr = val; rtc_encode(); return;   /* FMT may have changed */
    case 0x24:                                                  /* WPR */
        if (val == 0xCA) rtc.wpr_state = 1;
        else if (val == 0x53 && rtc.wpr_state == 1) { rtc.unlocked = true; rtc.wpr_state = 0; }
        else { rtc.unlocked = false; rtc.wpr_state = 0; }
        return;
    case 0x28: rtc.calr = val; return;
    case 0x2C: rtc.shiftr = val; return;
    case 0x40: rtc.alrmar = val; return;
    case 0x44: rtc.alrmassr = val; return;
    case 0x48: rtc.alrmbr = val; return;
    case 0x4C: rtc.alrmbssr = val; return;
    case 0x5C: rtc.sr &= ~val; return;                          /* SCR: write 1 to clear */
    }
}

int opt_rtc_host;                             /* 1 = start from the host clock */

/* ------------------------------------------------------------------ */
/* TAMP - holds the backup registers on this part. The firmware keeps    */
/* its "clock has been set" marker here, so these must persist.          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* TAMP - carries the backup registers on this part. The firmware keeps  */
/* its "the clock has already been set" marker here, so these have to    */
/* survive both the Standby reset and a restart of the emulator.         */
/* ------------------------------------------------------------------ */

u32 tamp_bkp[32];
static u32 tamp_regs[0x40];
static u32 tamp_reads, tamp_writes;

static u32 tamp_read(u32 off, int size)
{
    (void)size;
    if (off >= 0x100 && off < 0x180) { tamp_reads++; return tamp_bkp[(off - 0x100) / 4]; }
    return off / 4 < 0x40 ? tamp_regs[off / 4] : 0;
}

static void tamp_write(u32 off, u32 val, int size)
{
    (void)size;
    if (off >= 0x100 && off < 0x180) { tamp_writes++; tamp_bkp[(off - 0x100) / 4] = val; return; }
    if (off / 4 < 0x40) tamp_regs[off / 4] = val;
}

/* localtime_r is POSIX; Windows has localtime_s. */
static void local_time_now(struct tm *out)
{
    time_t now = time(NULL);
#ifdef _WIN32
    localtime_s(out, &now);
#else
    localtime_r(&now, out);
#endif
}

/* Sets the calendar from the host clock. */
void rtc_sync_host(void)
{
    struct tm tm;
    local_time_now(&tm);
    rtc.hour = tm.tm_hour; rtc.min = tm.tm_min; rtc.sec = tm.tm_sec;
    rtc.day = tm.tm_mday; rtc.month = tm.tm_mon + 1;
    rtc.year = (tm.tm_year + 1900) % 100;
    rtc.wday = tm.tm_wday == 0 ? 7 : tm.tm_wday;
    rtc.subsec = 0;
    rtc_encode();
}

static void rtc_init_state(void)
{
    /* The RTC lives in the backup domain: it survives the system reset that
       leaving Standby performs, so only initialise it on a cold start. */
    static bool initialised;
    if (initialised) return;
    initialised = true;

    memset(&rtc, 0, sizeof rtc);
    rtc.prer = 0x007F00FF;                    /* 32768 / 128 / 256 = 1 Hz */
    if (opt_rtc_host) {
        rtc_sync_host();
    } else {
        /* Factory state, same as a unit that has just had its cell fitted. */
        rtc.hour = 12; rtc.min = 0; rtc.sec = 0;
        rtc.day = 1; rtc.month = 1; rtc.year = 21; rtc.wday = 5;
        rtc_encode();
    }
}

/* A backup-domain reset returns the clock and backup registers to the state of
   a unit that has just had its cell fitted. */
void rtc_backup_reset(void)
{
    memset(tamp_bkp, 0, sizeof tamp_bkp);
    rtc.hour = 12; rtc.min = 0; rtc.sec = 0;
    rtc.day = 1; rtc.month = 1; rtc.year = 21; rtc.wday = 5;
    rtc.subsec = 0;
    rtc_encode();
}

/* Battery-backed state: the clock keeps running while the unit is "off". */
void rtc_save_state(RtcState *st)
{
    st->hour = rtc.hour; st->min = rtc.min; st->sec = rtc.sec;
    st->day = rtc.day; st->month = rtc.month; st->year = rtc.year; st->wday = rtc.wday;
}

void rtc_restore_state(const RtcState *st, u32 elapsed_secs)
{
    rtc.hour = st->hour; rtc.min = st->min; rtc.sec = st->sec;
    rtc.day = st->day; rtc.month = st->month; rtc.year = st->year;
    rtc.wday = st->wday ? st->wday : 1;
    while (elapsed_secs--) rtc_advance_second();
    rtc_encode();
}

/* ================================================================== */
/* SPI - used to send the initialisation command stream to the LCD     */
/* controller before the LTDC starts scanning out pixels.              */
/* ================================================================== */

typedef struct {
    u32 cr1, cr2, cfg1, cfg2, ier, ifcr, crcpoly, txcrc, rxcrc, udrdr;
    u32 last_tx;
    u32 nbytes;
} Spi;

static Spi spis[4];

/* SPI_SR: the emulated peripheral is always ready and never stalls. */
#define SPI_SR_RXP  (1u << 0)
#define SPI_SR_TXP  (1u << 1)
#define SPI_SR_DXP  (1u << 2)
#define SPI_SR_EOT  (1u << 3)
#define SPI_SR_TXTF (1u << 4)
#define SPI_SR_TXC  (1u << 12)

static u32 spi_read_n(int n, u32 off)
{
    Spi *s = &spis[n];
    switch (off) {
    case 0x00: return s->cr1;
    case 0x04: return s->cr2;
    case 0x08: return s->cfg1;
    case 0x0C: return s->cfg2;
    case 0x10: return s->ier;
    case 0x14: return SPI_SR_TXP | SPI_SR_RXP | SPI_SR_DXP | SPI_SR_TXC |
                      ((s->cr1 & (1u << 9)) ? (SPI_SR_EOT | SPI_SR_TXTF) : 0);
    case 0x20: return 0;                        /* TXDR reads as 0 */
    case 0x30: return s->last_tx;               /* RXDR: loop the last byte back */
    case 0x40: return s->crcpoly;
    }
    return 0;
}

static void spi_write_n(int n, u32 off, u32 val)
{
    Spi *s = &spis[n];
    switch (off) {
    case 0x00:
        s->cr1 = val;
        if (val & (1u << 11)) { s->cr1 &= ~(1u << 9); s->nbytes = 0; }   /* CSUSP */
        return;
    case 0x04: s->cr2 = val; s->nbytes = 0; return;
    case 0x08: s->cfg1 = val; return;
    case 0x0C: s->cfg2 = val; return;
    case 0x10: s->ier = val; return;
    case 0x18: return;                          /* IFCR: flags are recomputed */
    case 0x20: s->last_tx = val; s->nbytes++; return;   /* TXDR */
    case 0x40: s->crcpoly = val; return;
    }
}

#define SPI_ACCESSOR(N) \
    static u32 spi##N##_read(u32 off, int size) { (void)size; return spi_read_n(N, off); } \
    static void spi##N##_write(u32 off, u32 val, int size) { (void)size; spi_write_n(N, off, val); }
SPI_ACCESSOR(0) SPI_ACCESSOR(1) SPI_ACCESSOR(2) SPI_ACCESSOR(3)

/* ------------------------------------------------------------------ */
/* ADC - the firmware measures the battery here and refuses to power on  */
/* if the reading is below its lowest threshold.                        */
/* ------------------------------------------------------------------ */

u32 opt_battery_raw = 45000;          /* ~4.1 V on a full cell: level 5 of 5 */

static struct { u32 isr, ier, cr, cfgr, cfgr2, pcsel, sqr[4], smpr[2]; } adc;

static u32 adc_read(u32 off, int size)
{
    (void)size;
    switch (off) {
    case 0x00:                                    /* ADC_ISR */
        return adc.isr | ((adc.cr & 1) ? 1u : 0u) /* ADRDY */
             | ((adc.cr & 4) ? 0x1Eu : 0u);       /* EOSMP|EOC|EOS|OVR once started */
    case 0x04: return adc.ier;
    case 0x08: return adc.cr & ~0x80000000u;      /* ADCAL always reads back complete */
    case 0x0C: return adc.cfgr;
    case 0x10: return adc.cfgr2;
    case 0x1C: return adc.pcsel;
    case 0x30: return adc.sqr[0];
    case 0x34: return adc.sqr[1];
    case 0x38: return adc.sqr[2];
    case 0x3C: return adc.sqr[3];
    case 0x40: return opt_battery_raw;            /* ADC_DR */
    }
    return 0;
}

static void adc_write(u32 off, u32 val, int size)
{
    (void)size;
    switch (off) {
    case 0x00: adc.isr &= ~val; return;
    case 0x04: adc.ier = val; return;
    case 0x08:
        adc.cr = val & ~0x80000000u;              /* calibration finishes at once */
        if (val & 2) adc.cr &= ~1u;               /* ADDIS clears ADEN */
        if (val & 0x10) adc.cr &= ~4u;            /* ADSTP clears ADSTART */
        adc.cr &= ~0x1Au;
        return;
    case 0x0C: adc.cfgr = val; return;
    case 0x10: adc.cfgr2 = val; return;
    case 0x14: adc.smpr[0] = val; return;
    case 0x18: adc.smpr[1] = val; return;
    case 0x1C: adc.pcsel = val; return;
    case 0x30: adc.sqr[0] = val; return;
    case 0x34: adc.sqr[1] = val; return;
    case 0x38: adc.sqr[2] = val; return;
    case 0x3C: adc.sqr[3] = val; return;
    }
}

static u32 adccom_read(u32 off, int size) { (void)off; (void)size; return 0; }
static void adccom_write(u32 off, u32 val, int size) { (void)off; (void)val; (void)size; }

static u32 generic_regs[16][0x400 / 4];
static int generic_count;

#define GENERIC(N) \
    static u32 gen##N##_read(u32 off, int size) { (void)size; return off < 0x400 ? generic_regs[N][off / 4] : 0; } \
    static void gen##N##_write(u32 off, u32 val, int size) { (void)size; if (off < 0x400) generic_regs[N][off / 4] = val; }
GENERIC(0) GENERIC(1) GENERIC(2) GENERIC(3) GENERIC(4) GENERIC(5) GENERIC(6) GENERIC(7)
GENERIC(8) GENERIC(9) GENERIC(10) GENERIC(11) GENERIC(12) GENERIC(13) GENERIC(14) GENERIC(15)

static const periph_read_fn gen_readers[16] = {
    gen0_read, gen1_read, gen2_read, gen3_read, gen4_read, gen5_read, gen6_read, gen7_read,
    gen8_read, gen9_read, gen10_read, gen11_read, gen12_read, gen13_read, gen14_read, gen15_read };
static const periph_write_fn gen_writers[16] = {
    gen0_write, gen1_write, gen2_write, gen3_write, gen4_write, gen5_write, gen6_write, gen7_write,
    gen8_write, gen9_write, gen10_write, gen11_write, gen12_write, gen13_write, gen14_write, gen15_write };

static void register_generic(u32 base, const char *name)
{
    if (generic_count >= 16) return;
    periph_register(base, 0x400, name, gen_readers[generic_count], gen_writers[generic_count]);
    generic_count++;
}

void misc_init(void)
{
    memset(generic_regs, 0, sizeof generic_regs);
    generic_count = 0;
    periph_register(0x48021800, 0x400, "RNG", rng_read, rng_write);
    rtc_init_state();
    periph_register(0x58004000, 0x400, "RTC", rtc_read, rtc_write);
    periph_register(0x58025000, 0x400, "TAMP", tamp_read, tamp_write);
    periph_register(0x58025000, 0x400, "TAMP", tamp_read, tamp_write);
    memset(spis, 0, sizeof spis);
    periph_register(0x40013000, 0x400, "SPI1", spi0_read, spi0_write);
    periph_register(0x40003800, 0x400, "SPI2", spi1_read, spi1_write);
    periph_register(0x40003C00, 0x400, "SPI3", spi2_read, spi2_write);
    periph_register(0x58001400, 0x400, "SPI6", spi3_read, spi3_write);
    register_generic(0x40005400, "I2C1");
    register_generic(0x40005800, "I2C2");
    register_generic(0x58001C00, "I2C4");
    register_generic(0x40007400, "DAC1");
    memset(&adc, 0, sizeof adc);
    periph_register(0x40022000, 0x100, "ADC1", adc_read, adc_write);
    periph_register(0x40022100, 0x100, "ADC2", adc_read, adc_write);
    periph_register(0x40022300, 0x100, "ADC12_COMMON", adccom_read, adccom_write);
    periph_register(0x58026000, 0x100, "ADC3", adc_read, adc_write);
    periph_register(0x58026300, 0x100, "ADC3_COMMON", adccom_read, adccom_write);
    register_generic(0x58003400, "LPTIM2");
    register_generic(0x40002400, "LPTIM1");
    register_generic(0x48022000, "HASH");
    register_generic(0x58026400, "SAI4B");
    register_generic(0x52000000, "MDMA");
    register_generic(0x51000000, "GPV");
}

/* ================================================================== */
/* aggregate init / tick                                               */
/* ================================================================== */

void periph_init(void)
{
    rcc_init();
    pwr_init();
    flashif_init();
    gpio_init();
    exti_init();
    syscfg_init();
    tim_init();
    dma_init();
    octospi_init();
    ltdc_init();
    dma2d_init();
    sai_init();
    cryp_init();
    misc_init();
}

void periph_tick(u32 cycles)
{
    if (cpu.sleeping) pwr_check_wakeup();
    tim_tick(cycles);
    ltdc_tick(cycles);
    sai_tick(cycles);
    rtc_tick(cycles);
    dma_tick(cycles);
    cpu_tick_systick(cycles);
}

/* ================================================================== */
/* diagnostics                                                         */
/* ================================================================== */

void periph_dump_state(void)
{
    gwlog("---- peripheral state ----\n");
    gwlog("RCC_CR=%08x CFGR=%08x  AHB4ENR=%08x APB4ENR=%08x APB3ENR=%08x AHB3ENR=%08x\n",
          rcc_regs[0x00 / 4], rcc_regs[0x10 / 4], rcc_regs[0x140 / 4],
          rcc_regs[0x144 / 4], rcc_regs[0x13C / 4], rcc_regs[0x134 / 4]);
    gwlog("audio: %llu samples pushed, range %d..%d, rate %u Hz\n",
          (unsigned long long)audio_pushed, audio_min, audio_max, opt_audio_hz);
    gwlog("RCC_BDCR = %08x (%u reads, %u writes)\n", rcc_regs[0x70 / 4], bdcr_r, bdcr_w);
    gwlog("TAMP backup: %u reads, %u writes:", tamp_reads, tamp_writes);
    for (int i = 0; i < 32; i++) if (tamp_bkp[i]) gwlog(" [%d]=%08x", i, tamp_bkp[i]);
    gwlog("\n");
    gwlog("RTC: %02d:%02d:%02d %02d/%02d/%02d  TR=%08x CR=%08x SR=%08x\n",
          rtc.hour, rtc.min, rtc.sec, rtc.day, rtc.month, rtc.year, rtc.tr, rtc.cr, rtc.sr);
    gwlog("audio: %llu samples pushed, range %d..%d, rate %u Hz\n",
          (unsigned long long)audio_pushed, audio_min, audio_max, opt_audio_hz);
    gwlog("SAI1 ACR1=%08x AFRCR=%08x ASLOTR=%08x  CDCCIPR=%08x CDCCIP1R=%08x CDCCIP2R=%08x\n",
          sai.blk[0].cr1, sai.blk[0].frcr, sai.blk[0].slotr,
          rcc_regs[0x4C / 4], rcc_regs[0x50 / 4], rcc_regs[0x54 / 4]);
    gwlog("PLL1Q=%u PLL2P=%u PLL3P=%u -> SAI1 kernel %u Hz\n",
          rcc_pll_out_hz(1, 'Q'), rcc_pll_out_hz(2, 'P'), rcc_pll_out_hz(3, 'P'), sai1_kernel_hz());
    gwlog("RCC PLL2DIVR=%08x PLL3DIVR=%08x PLLCFGR=%08x\n",
          rcc_regs[0x38 / 4], rcc_regs[0x40 / 4], rcc_regs[0x2C / 4]);
    gwlog("RCC PLLCKSELR=%08x PLLCFGR=%08x PLL1DIVR=%08x CDCFGR1=%08x CDCFGR2=%08x D1CFGR=%08x\n",
          rcc_regs[0x28/4], rcc_regs[0x2C/4], rcc_regs[0x30/4],
          rcc_regs[0x18/4], rcc_regs[0x1C/4], rcc_regs[0x18/4]);
    gwlog("PWR: CR1=%08x CR2=%08x CR3=%08x CPUCR=%08x SRDCR=%08x WKUPCR=%08x WKUPFR=%08x WKUPEPR=%08x\n",
          pwr_regs[0x00/4], pwr_regs[0x08/4], pwr_regs[0x0C/4], pwr_regs[0x10/4],
          pwr_regs[0x18/4], pwr_regs[0x20/4], pwr_regs[0x24/4], pwr_regs[0x28/4]);
    gwlog("EXTI: IMR=%08x EMR=%08x RTSR=%08x FTSR=%08x PR=%08x\n", exti.imr, exti.emr, exti.rtsr, exti.ftsr, exti.pr);
    for (int i = 0; i < 4; i++)
        gwlog("SYSCFG_EXTICR%d=%08x\n", i + 1, syscfg_regs[(0x08 + i * 4) / 4]);
    for (int i = 0; i < NGPIO; i++)
        if (gpios[i].moder)
            gwlog("GPIO%c MODER=%08x PUPDR=%08x ODR=%04x IDR=%04x\n",
                  'A' + i, gpios[i].moder, gpios[i].pupdr, gpios[i].odr, gpio_idr(&gpios[i]));
    gwlog("LTDC GCR=%08x AWCR=%08x BPCR=%08x IER=%08x  L1 CR=%08x PFCR=%08x CFBAR=%08x CFBLR=%08x CFBLNR=%08x\n",
          ltdc.gcr, ltdc.awcr, ltdc.bpcr, ltdc.ier, ltdc.layer[0].cr,
          ltdc.layer[0].pfcr, ltdc.layer[0].cfbar, ltdc.layer[0].cfblr, ltdc.layer[0].cfblnr);
    for (int i = 0; i < NTIMERS; i++)
        if (timers[i].cr1 & 1)
            gwlog("%s running: PSC=%u ARR=%u DIER=%08x CNT=%u\n",
                  timers[i].name, timers[i].psc, timers[i].arr, timers[i].dier, timers[i].cnt);
    for (int c = 0; c < 2; c++)
        for (int n = 0; n < 8; n++)
            if (dma[c].s[n].cr & 1)
                gwlog("DMA%d stream%d CR=%08x NDTR=%u PAR=%08x M0AR=%08x\n",
                      c + 1, n, dma[c].s[n].cr, dma[c].s[n].ndtr, dma[c].s[n].par, dma[c].s[n].m0ar);
    for (int li = 0; li < 2; li++) {
        int n = 0;
        for (int i = 0; i < 256; i++) if (ltdc.layer[li].clut[i]) n++;
        if (n) {
            gwlog("LTDC L%d CLUT (%d non-zero):", li + 1, n);
            for (int i = 0; i < 256; i++)
                if (ltdc.layer[li].clut[i]) gwlog(" [%02x]=%06x", i, ltdc.layer[li].clut[i]);
            gwlog("\n");
        }
    }
    for (int i = 0; i < NGPIO; i++)
        if (idr_reads[i]) gwlog("GPIO%c IDR read %llu times\n", 'A' + i, (unsigned long long)idr_reads[i]);
    for (int i = 0; i < n_idr_sites; i++)
        gwlog("IDR read site: pc=%08x port=GPIO%c\n", idr_sites[i].pc, 'A' + idr_sites[i].port);
    gwlog("enabled IRQs:");
    for (int i = 0; i < NUM_IRQ; i++)
        if (cpu.exc_enabled[EXC_EXTERNAL + i]) gwlog(" %d", i);
    gwlog("\n");
    gwlog("SysTick CTRL=%08x LOAD=%u  SCR-sleepdeep-state: sleeping=%d\n",
          cpu.systick_ctrl, cpu.systick_load, cpu.sleeping);
    gwlog("---- end peripheral state ----\n");
}

/* ================================================================== */
/* Save-state support                                                  */
/* ================================================================== */

/* Plain data only. Instrumentation counters are left out: they describe the
   run, not the machine. */
void periph_state_blocks(StateBlockFn fn, void *ctx)
{
    fn(ctx, "rcc",     rcc_regs,          sizeof rcc_regs);
    fn(ctx, "bdcr_r",  &bdcr_r,           sizeof bdcr_r);
    fn(ctx, "bdcr_w",  &bdcr_w,           sizeof bdcr_w);
    fn(ctx, "pwr",     pwr_regs,          sizeof pwr_regs);
    fn(ctx, "flashif", flash_regs,        sizeof flash_regs);
    fn(ctx, "gpio",    gpios,             sizeof gpios);
    fn(ctx, "exti",    &exti,             sizeof exti);
    fn(ctx, "syscfg",  syscfg_regs,       sizeof syscfg_regs);
    fn(ctx, "tim",     timers,            sizeof timers);
    fn(ctx, "dma",     dma,               sizeof dma);
    fn(ctx, "dmamux",  dmamux_regs,       sizeof dmamux_regs);
    fn(ctx, "ltdc",    &ltdc,             sizeof ltdc);
    fn(ctx, "ltdc_w",  &ltdc_width,       sizeof ltdc_width);
    fn(ctx, "ltdc_h",  &ltdc_height,      sizeof ltdc_height);
    fn(ctx, "fb",      ltdc_framebuffer,  sizeof ltdc_framebuffer);
    fn(ctx, "dma2d",   &d2d,              sizeof d2d);
    fn(ctx, "sai",     &sai,              sizeof sai);
    fn(ctx, "saihz",   &opt_audio_hz,     sizeof opt_audio_hz);
    fn(ctx, "rtc",     &rtc,              sizeof rtc);
    fn(ctx, "tamp",    tamp_regs,         sizeof tamp_regs);
    fn(ctx, "bkpreg",  tamp_bkp,          sizeof tamp_bkp);
    fn(ctx, "spi",     spis,              sizeof spis);
    fn(ctx, "adc",     &adc,              sizeof adc);
    fn(ctx, "generic", generic_regs,      sizeof generic_regs);
    fn(ctx, "genn",    &generic_count,    sizeof generic_count);
}

void periph_state_fixup(void)
{
    for (int i = 0; i < NTIMERS; i++) timers[i].name = tim_defs[i].name;
    dma[0].name = "DMA1";
    dma[1].name = "DMA2";
}
