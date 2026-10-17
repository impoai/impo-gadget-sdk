/*
 * Crash record that survives the reset: the exception hook stores the trap CSRs
 * and task name in .noinit SRAM, and the next boot prints them to the log.
 * The device has no serial console, so this is how crashes reach ADB.
 * Resolve with: riscv64-unknown-elf-addr2line -e build/impo-gadget -a <mepc> <ra>
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "core_feature_base.h"

#include "platform_arcs.h"

#define TAG "crash"
#include "lisa_log.h"

#define CRASH_MAGIC 0xC4A5D00Du
#define MCAUSE_STALL 0xFFFFFFFFu   /* not a trap: a supervised task stopped checking in */
#define MCAUSE_BOOT  0xFFFFFFFEu   /* not a trap: the boot step in progress, cleared once running */

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    uint32_t mdcause;
    uint32_t uptime_ms;
    char task[16];
    char where[24];
    uint32_t check;
} crash_rec_t;

static crash_rec_t s_rec __attribute__((section(".noinit.gadget_crash")));

static uint32_t rec_check(const crash_rec_t *r)
{
    return r->magic ^ r->count ^ r->mcause ^ r->mepc ^ r->mtval ^ r->mdcause ^ r->uptime_ms ^ 0x5A5A5A5Au;
}

extern void Exception_User_Register_EXC(unsigned long exc_handler);

static void begin_record(void)
{
    uint32_t count = (s_rec.magic == CRASH_MAGIC && s_rec.check == rec_check(&s_rec)) ? s_rec.count : 0;
    s_rec.magic = CRASH_MAGIC;
    s_rec.count = count + 1;
}

void plat_crash_note_stall(const char *task, const char *where, uint32_t silent_ms)
{
    begin_record();
    s_rec.mcause = MCAUSE_STALL;
    s_rec.mepc = 0;
    s_rec.mtval = silent_ms;
    s_rec.mdcause = 0;
    s_rec.uptime_ms = (uint32_t)plat_uptime_ms();
    strncpy(s_rec.task, task, sizeof(s_rec.task) - 1);
    s_rec.task[sizeof(s_rec.task) - 1] = '\0';
    strncpy(s_rec.where, where, sizeof(s_rec.where) - 1);
    s_rec.where[sizeof(s_rec.where) - 1] = '\0';
    s_rec.check = rec_check(&s_rec);
    __asm__ volatile("fence rw, rw");
}

void plat_crash_boot_stage(const char *stage)
{
    /* Overwrite only our own boot marker, never a real crash or stall record. */
    bool ours = !(s_rec.magic == CRASH_MAGIC && s_rec.check == rec_check(&s_rec)) || s_rec.mcause == MCAUSE_BOOT;
    if (!ours) {
        return;
    }
    s_rec.magic = CRASH_MAGIC;
    s_rec.mcause = MCAUSE_BOOT;
    s_rec.mepc = s_rec.mtval = s_rec.mdcause = 0;
    s_rec.uptime_ms = (uint32_t)plat_uptime_ms();
    strncpy(s_rec.task, "boot", sizeof(s_rec.task) - 1);
    strncpy(s_rec.where, stage, sizeof(s_rec.where) - 1);
    s_rec.where[sizeof(s_rec.where) - 1] = '\0';
    s_rec.check = rec_check(&s_rec);
    __asm__ volatile("fence rw, rw");
}

void plat_crash_boot_done(void)
{
    if (s_rec.magic == CRASH_MAGIC && s_rec.mcause == MCAUSE_BOOT) {
        s_rec.magic = 0;
    }
}

static void on_exception(void)
{
    begin_record();
    s_rec.mcause = (uint32_t)__RV_CSR_READ(CSR_MCAUSE);
    s_rec.mepc = (uint32_t)__RV_CSR_READ(CSR_MEPC);
    s_rec.mtval = (uint32_t)__RV_CSR_READ(CSR_MTVAL);
    s_rec.mdcause = (uint32_t)__RV_CSR_READ(CSR_MDCAUSE);
    s_rec.uptime_ms = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    const char *name = cur ? pcTaskGetName(cur) : "?";
    strncpy(s_rec.task, name ? name : "?", sizeof(s_rec.task) - 1);
    s_rec.task[sizeof(s_rec.task) - 1] = '\0';
    s_rec.check = rec_check(&s_rec);
    __asm__ volatile("fence rw, rw");
}

void plat_crash_init(void)
{
    if (s_rec.magic == CRASH_MAGIC && s_rec.check == rec_check(&s_rec)) {
        s_rec.task[sizeof(s_rec.task) - 1] = '\0';
        if (s_rec.mcause == MCAUSE_BOOT) {
            s_rec.where[sizeof(s_rec.where) - 1] = '\0';
            LISA_LOGE(TAG, "previous boot never finished: it hung in %s (%u ms after start), and the watchdog reset it",
                      s_rec.where, (unsigned)s_rec.uptime_ms);
            s_rec.magic = 0;
            Exception_User_Register_EXC((unsigned long)on_exception);
            return;
        }
        if (s_rec.mcause == MCAUSE_STALL) {
            s_rec.where[sizeof(s_rec.where) - 1] = '\0';
            LISA_LOGE(TAG, "previous reset #%u: watchdog, task %s silent for %u ms at %s, uptime=%u ms",
                      (unsigned)s_rec.count, s_rec.task, (unsigned)s_rec.mtval, s_rec.where,
                      (unsigned)s_rec.uptime_ms);
            s_rec.magic = 0;
            Exception_User_Register_EXC((unsigned long)on_exception);
            return;
        }
        LISA_LOGE(TAG, "previous crash #%u: task=%s mcause=0x%08x mepc=0x%08x mtval=0x%08x mdcause=0x%08x uptime=%u ms",
                  (unsigned)s_rec.count, s_rec.task, (unsigned)s_rec.mcause, (unsigned)s_rec.mepc,
                  (unsigned)s_rec.mtval, (unsigned)s_rec.mdcause, (unsigned)s_rec.uptime_ms);
        s_rec.magic = 0;  /* report once; count restarts */
    }
    Exception_User_Register_EXC((unsigned long)on_exception);
}
