#include <vantage/arch/irq.h>

#define GIC_DISTRIBUTOR 0xff841000UL
#define GIC_CPU 0xff842000UL
#define GICD_CTLR (GIC_DISTRIBUTOR + 0x000)
#define GICD_ISENABLER0 (GIC_DISTRIBUTOR + 0x100)
#define GICD_IPRIORITYR (GIC_DISTRIBUTOR + 0x400)
#define GICC_CTLR (GIC_CPU + 0x00)
#define GICC_PMR (GIC_CPU + 0x04)
#define GICC_IAR (GIC_CPU + 0x0c)
#define GICC_EOIR (GIC_CPU + 0x10)

#define TIMER_PRIORITY 0xa0U
#define TIMER_ENABLE 1UL
#define TIMER_MASKED 2UL

static uint32_t read32(uintptr_t address) { return *(volatile uint32_t*)address; }
static void write32(uintptr_t address, uint32_t value) { *(volatile uint32_t*)address = value; }

void irq_timer_stop(void) {
    uint64_t control = TIMER_MASKED;
    __asm__ volatile("msr cntv_ctl_el0, %0\nisb" :: "r"(control) : "memory");
}

void irq_timer_start(uint32_t microseconds) {
    uint64_t frequency, control = TIMER_ENABLE;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    uint64_t ticks = (frequency & 0xffffffffU) * microseconds / 1000000U;
    if (!ticks) ticks = 1;
    if (ticks > 0x7fffffffU) ticks = 0x7fffffffU;
    __asm__ volatile("msr cntv_tval_el0, %0\nmsr cntv_ctl_el0, %1\nisb"
                     :: "r"(ticks), "r"(control) : "memory");
}

int irq_init_core(void) {
    uint64_t core, level, mask, frequency;
    __asm__ volatile("mrs %0, mpidr_el1\nmrs %1, CurrentEL\nmrs %2, daif"
                     : "=r"(core), "=r"(level), "=r"(mask));
    if (level != 4 || (mask & 0x3c0) != 0x3c0) return 0;
    irq_timer_stop();
    /* EL0 must not be able to reprogram the timer that preempts it. */
    __asm__ volatile("msr cntkctl_el1, xzr\nisb" ::: "memory");
    /* Non-secure view: bit 0 forwards Group 1. PPI state is banked per core. */
    if (!(core & 0xff)) write32(GICD_CTLR, read32(GICD_CTLR) | 1U);
    *(volatile uint8_t*)(GICD_IPRIORITYR + IRQ_TIMER) = TIMER_PRIORITY;
    write32(GICD_ISENABLER0, 1U << IRQ_TIMER);
    write32(GICC_PMR, 0xf0U);
    write32(GICC_CTLR, 1U);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    /* Secure Group 0 interrupts read as zero here and could never be taken. */
    return ((read32(GICD_ISENABLER0) >> IRQ_TIMER) & 1U) && (frequency & 0xffffffffU);
}

uint32_t irq_service(void) {
    uint32_t acknowledged = read32(GICC_IAR);
    uint32_t id = acknowledged & 0x3ffU;
    if (id >= 1020U) return IRQ_NONE;
    /* The level must drop before completion or the interrupt fires again. */
    if (id == IRQ_TIMER) irq_timer_stop();
    write32(GICC_EOIR, acknowledged);
    return id;
}
