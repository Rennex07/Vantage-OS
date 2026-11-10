#include <vantage/platform/pi4/timer.h>
#include <vantage/platform/pi4/io.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint64_t clock_us;
static uint32_t step, reads, phase, rollover;

unsigned int mmio_read(long address) {
    uint32_t result;
    reads++;
    if (rollover) {
        /* First low read straddles the carry; the reader must sample again. */
        const uint32_t offsets[] = {0x3008, 0x3004, 0x3008, 0x3008, 0x3004, 0x3008};
        const uint32_t values[] = {5, 0xffffffffU, 6, 6, 7, 6};
        CHECK(phase < 6 && (uint32_t)address == 0xfe000000U + offsets[phase]);
        return values[phase++];
    }
    CHECK((uint32_t)address == 0xfe000000U + (phase == 1 ? 0x3004U : 0x3008U));
    result = phase == 1 ? (uint32_t)clock_us : (uint32_t)(clock_us >> 32);
    if (++phase == 3) { phase = 0; clock_us += step; }
    return result;
}

int main(void) {
    uint32_t before;
    rollover = 1;
    CHECK(get_sys_timer() == 0x600000007ULL && reads == 6);
    rollover = phase = 0; clock_us = 0x123456789abcdef0ULL;
    CHECK(get_sys_timer() == clock_us);
    before = reads;
    wait_usec(0); wait_msec(0); wait_msec_st(0); wait_cycles(0); wait_cycles(3);
    CHECK(reads == before);
    clock_us = 0; step = 1; wait_usec(5); CHECK(clock_us == 6);
    clock_us = 0; step = 100; wait_msec(2); CHECK(clock_us == 2100);
    clock_us = 0; wait_msec_st(2); CHECK(clock_us == 2100);
    clock_us = ~(uint64_t)0 - 2; step = 1; wait_usec(5); CHECK(clock_us == 3);
    clock_us = 0; step = 1000000000;
    wait_msec(0xffffffffU);
    CHECK(clock_us >= 4294967295000ULL && clock_us < 4296967295000ULL);
    puts("PASS: timer carry retry, full-width values, delay units, zero, wrap and millisecond multiplication");
    return 0;
}
