#include <vantage/drivers/hardware.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static PropertyMailbox mailbox;
typedef struct { uint32_t clock, token; int submitted, owner, mode, clean, invalidated; } Model;
static uint32_t read32(void* context, uintptr_t address) {
    Model* m = context;
    if (address == 0xfe00b898ULL) return m->mode == 1 ? 0x80000000U : (m->submitted && m->mode != 2 ? 0 : 0x40000000U);
    if (address == 0xfe00b880ULL) {
        CHECK(m->submitted && m->clean);
        mailbox.words[1] = 0x80000000U;
        mailbox.words[4] = 0x80000008U;
        mailbox.words[6] = 100000000;
        if (m->mode == 3) mailbox.words[4] = 8;
        return m->token;
    }
    return 0;
}
static void write32(void* context, uintptr_t address, uint32_t value) {
    Model* m = context; CHECK(address == 0xfe00b8a0ULL);
    CHECK(mailbox.words[0] == 32 && mailbox.words[2] == 0x30002 && !mailbox.words[4]);
    m->submitted = 1; m->token = value;
}
static uint32_t clock_us(void* context) { Model* m = context; m->clock += m->mode == 4 ? 0U : 1000U; return m->clock; }
static int owned(void* context) { return ((Model*)context)->owner; }
static uint64_t physical(void* context, const void* p) { (void)context; CHECK(p == mailbox.words); return 0x10000; }
static void cache(void* context, void* p, size_t n, enum HwCache op) {
    Model* m = context; CHECK(p == mailbox.words && n == 128);
    if (op == HW_CLEAN_INVALIDATE) m->clean++;
    else { CHECK(op == HW_INVALIDATE); m->invalidated++; }
}
int main(void) {
    Model m = {0}; m.owner = 1;
    HwOps io = {&m, read32, write32, clock_us, owned, cache, physical, NULL};
    uint32_t values[2] = {1, 0};
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_OK);
    CHECK(values[1] == 100000000 && m.clean == 1 && m.invalidated == 1);
    m.mode = 3; values[1] = 123;
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_PROTOCOL && values[1] == 123);
    m.mode = 1;
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_TIMEOUT && mailbox.state == 0);
    m.mode = 2;
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_TIMEOUT && mailbox.state == -1);
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_OFF);
    m.owner = 0;
    CHECK(property_call(&mailbox, &io, 0x30002, values, 2) == HW_INVALID);
    m.owner = 1; m.clock = 0xfffff000U;
    CHECK(hw_delay(&io, 10000) == HW_OK);
    m.mode = 4;
    CHECK(hw_wait(&io, 0x100, 1, 1, 100) == HW_TIMEOUT);
    puts("PASS: mailbox response validation, late-DMA quarantine, timer wrap and stopped timer");
    return 0;
}
