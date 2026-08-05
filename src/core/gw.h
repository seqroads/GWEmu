/* gw.h - shared types for the Game & Watch (STM32H7B0) emulator */
#ifndef GW_H
#define GW_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ------------------------------------------------------------------ */
/* Memory map (STM32H7B0, RM0455)                                      */
/* ------------------------------------------------------------------ */
#define ITCM_BASE     0x00000000u
#define ITCM_SIZE     (64u * 1024)
#define FLASH_BASE    0x08000000u
#define FLASH_SIZE    (128u * 1024)
#define DTCM_BASE     0x20000000u
#define DTCM_SIZE     (128u * 1024)
#define AXISRAM_BASE  0x24000000u
#define AXISRAM_SIZE  (1024u * 1024)
#define SRAM1_BASE    0x30000000u
#define SRAM1_SIZE    (64u * 1024)
#define SRAM2_BASE    0x30010000u
#define SRAM2_SIZE    (64u * 1024)
#define SRD_SRAM_BASE 0x38000000u
#define SRD_SRAM_SIZE (32u * 1024)
#define BKPSRAM_BASE  0x38800000u
#define BKPSRAM_SIZE  (4u * 1024)
#define EXTFLASH_BASE 0x90000000u
#define EXTFLASH_MAX  (4096u * 1024)
/* The fitted flash differs per title: 1 MiB on Mario, 4 MiB on Zelda. */
extern u32 extflash_size;

/* ------------------------------------------------------------------ */
/* CPU                                                                 */
/* ------------------------------------------------------------------ */

enum {
    EXC_RESET = 1, EXC_NMI = 2, EXC_HARDFAULT = 3, EXC_MEMMANAGE = 4,
    EXC_BUSFAULT = 5, EXC_USAGEFAULT = 6, EXC_SVCALL = 11, EXC_DEBUGMON = 12,
    EXC_PENDSV = 14, EXC_SYSTICK = 15, EXC_EXTERNAL = 16
};

#define NUM_IRQ    150
#define NUM_EXC    (EXC_EXTERNAL + NUM_IRQ)

typedef struct {
    u32 r[16];              /* r15 is a scratch copy of PC+4 while executing */
    u32 pc;                 /* address of the instruction being executed     */
    u32 sp_main, sp_process;

    u8  n, z, c, v, q;      /* APSR flags */
    u8  ge;                 /* APSR GE[3:0] */
    u8  itstate;            /* IT block state (ITSTATE encoding)  */
    bool itstate_written;   /* set when an instruction rewrote ITSTATE itself */
    u8  primask, faultmask;
    u8  basepri;
    u8  control;            /* bit0 nPRIV, bit1 SPSEL, bit2 FPCA  */
    bool handler_mode;
    u32 ipsr;               /* current exception number */

    /* FPU */
    u32 s[32];
    u32 fpscr;
    u32 cpacr;
    u32 fpccr, fpcar, fpdscr;

    /* System control block */
    u32 vtor;
    u32 ccr, shcsr, actlr, scr;
    u32 aircr_prigroup;
    u32 cfsr, hfsr, mmfar, bfar;

    /* Exception state, indexed by exception number (0 unused) */
    u8  exc_pending[NUM_EXC];
    u8  exc_active[NUM_EXC];
    u8  exc_enabled[NUM_EXC];
    u8  exc_prio[NUM_EXC];

    /* SysTick */
    u32 systick_ctrl, systick_load, systick_val, systick_calib;

    /* exclusive monitor */
    bool excl_valid;
    u32  excl_addr;

    /* execution bookkeeping */
    u64  cycles;
    bool sleeping;
    bool halted;            /* fatal error -> stop */
    int  cur_exc;           /* exception currently being handled (0 = thread) */
    bool pc_changed;
    bool pending_sysreset;   /* SYSRESETREQ seen; reboot at the next boundary */
} CPU;

extern CPU cpu;

void cpu_reset(void);
/* Execute up to `budget` instructions. Returns instructions actually run. */
u32  cpu_run(u32 budget);
void cpu_raise_irq(int irq, bool level);
void cpu_set_pending(int irq);
void cpu_check_exceptions(void);
void cpu_tick_systick(u32 cycles);
const char *cpu_exc_name(int exc);

/* ------------------------------------------------------------------ */
/* Bus                                                                 */
/* ------------------------------------------------------------------ */
u32  bus_read32(u32 addr);
u32  bus_read16(u32 addr);
u32  bus_read8(u32 addr);
void bus_write32(u32 addr, u32 val);
void bus_write16(u32 addr, u32 val);
void bus_write8(u32 addr, u32 val);
u16  bus_fetch16(u32 addr);          /* instruction fetch, fast path */
void bus_init_mem(const void *intflash, u32 intlen,
                  const void *extflash, u32 extlen);
void bus_free(void);
void *bus_host_ptr(u32 addr, u32 len); /* direct pointer for DMA/LTDC, or NULL */

extern u8 *mem_itcm, *mem_dtcm, *mem_axi, *mem_sram1, *mem_sram2;
extern u8 *mem_srd, *mem_bkp, *mem_flash, *mem_extflash, *extflash_plain;

/* ------------------------------------------------------------------ */
/* Peripherals                                                         */
/* ------------------------------------------------------------------ */
typedef u32  (*periph_read_fn)(u32 offset, int size);
typedef void (*periph_write_fn)(u32 offset, u32 val, int size);

void periph_register(u32 base, u32 size, const char *name,
                     periph_read_fn rd, periph_write_fn wr);
u32  periph_read(u32 addr, int size);
void periph_write(u32 addr, u32 val, int size);
void periph_init(void);
void periph_tick(u32 cycles);
void periph_dump_state(void);
void system_reset(void);        /* Standby wake: PWR state carried across */
void system_cold_reset(void);   /* power-on reset; backup domain survives */

void rcc_init(void);
void pwr_init(void);
void flashif_init(void);
void gpio_init(void);
void exti_init(void);
void syscfg_init(void);
void tim_init(void);
void dma_init(void);
void octospi_init(void);
void ltdc_init(void);
void dma2d_init(void);
void sai_init(void);
void cryp_init(void);
void misc_init(void);

void gpio_set_button(int port, int pin, bool pressed);
void pwr_check_wakeup(void);
bool pwr_in_standby(void);
void exti_trigger(int line);

void tim_tick(u32 cycles);
void ltdc_tick(u32 cycles);
void sai_tick(u32 cycles);
void rtc_tick(u32 cycles);

/* Battery-backed clock state, carried across runs in the save file. */
typedef struct { s32 hour, min, sec, day, month, year, wday; } RtcState;
void rtc_save_state(RtcState *st);
void rtc_backup_reset(void);
u32  rcc_get_bdcr(void);
void rcc_set_bdcr(u32 v);
void rtc_restore_state(const RtcState *st, u32 elapsed_secs);
void rtc_sync_host(void);       /* set the calendar from the host clock */
extern int opt_rtc_host;
extern u32 tamp_bkp[32];

/* Persistence: external flash writes plus the backup domain. */
bool state_load(const char *path);
void state_save(const char *path);

/* ------------------------------------------------------------------ */
/* Save states                                                         */
/* ------------------------------------------------------------------ */

/* Flat, pointer-free blocks with a tag, so a snapshot from another build is
   rejected rather than misread. One walk serves saving and restoring. */
typedef void (*StateBlockFn)(void *ctx, const char *tag, void *data, u32 size);
void bus_state_blocks(StateBlockFn fn, void *ctx);
void periph_state_blocks(StateBlockFn fn, void *ctx);
void octospi_state_blocks(StateBlockFn fn, void *ctx);

/* Re-points the name fields that a raw restore leaves dangling. */
void periph_state_fixup(void);
void dma_tick(u32 cycles);
bool dma_service_par(u32 par);
extern u32 opt_audio_hz;

/* LTDC exposes the composed frame to the frontend */
extern u32 ltdc_framebuffer[320 * 240];
extern int ltdc_width, ltdc_height;
extern volatile int ltdc_frame_counter;

/* Audio ring consumed by SDL */
#define AUDIO_RING 32768
extern s16 audio_ring[AUDIO_RING];
extern volatile u32 audio_wr, audio_rd;
extern u64 audio_pushed;
extern s16 audio_min, audio_max;

/* ------------------------------------------------------------------ */
/* Logging / options                                                   */
/* ------------------------------------------------------------------ */
extern int  opt_trace;          /* 1 = trace unknown accesses, 2 = trace exec */
extern int  opt_log_periph;
extern int  opt_profile;
extern u32  opt_watch_lo, opt_watch_hi;
extern u32  opt_battery_raw;
void cpu_dump_history(const char *why);
void cpu_dump_profile(int top);
void cpu_dump_exceptions(void);
void cpu_profile_init(void);
void cpu_add_logcall(u32 addr);

/* Instrumentation. */
typedef struct { u32 pc; u64 count; } ProfileEntry;
typedef struct { u32 pc; u32 r[16]; } HistEntry;
bool cpu_profile_active(void);
u64  cpu_profile_other(void);
int  cpu_profile_top(ProfileEntry *out, int max);   /* hottest first */
int  cpu_history(HistEntry *out, int max);          /* oldest first  */
extern u32 logcall[16];
extern int n_logcall;
extern u64  opt_trace_start;
extern FILE *gw_logfile;

void gwlog(const char *fmt, ...);
void gwfatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* GW_H */
