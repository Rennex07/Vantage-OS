// I had AI to assist me since there was lack of documentation and tutorials :/
/* There're governments funding billions on AI research
   and then theres people who dedicate their lives to hate on it.
   Don't listen to those peasants. They are nothing more than uneducated fools. 
   How dare one challenge a perfect immortal machine?                           */

#include "../../includes/display.h"
#include "../../includes/mmu.h"

static volatile uint32_t mbox[36] __attribute__((aligned(64)));

static uint32_t* fbBase;
static uint32_t fbPitch;
static uint32_t fbWidth;

static uint32_t* drawBase;
static uint32_t backPitch;
static uint32_t pageHeight;
static uint32_t drawPage;
static uint32_t lastSwap;

#define PAGE_COUNT 3
#define BACK_PIXELS (800 * 480)
#define FRAME_US 16667

static uint32_t backBuffer[BACK_PIXELS] __attribute__((aligned(64)));

static void cache_flush(volatile void* addr, uint32_t size) {
    uintptr_t start = (uintptr_t)addr & ~63ULL;
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t p = start; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t time_us(void) {
    return *(volatile uint32_t*)TIMER_CLO;
}

uint32_t mbox_call(uint8_t ch) {
    volatile uint32_t* status = (volatile uint32_t*)MBOX_STATUS;
    volatile uint32_t* read = (volatile uint32_t*)MBOX_READ;
    volatile uint32_t* write = (volatile uint32_t*)MBOX_WRITE;

    uint32_t msg = ((uint32_t)((uintptr_t)mbox) & ~0xF) | (ch & 0xF);

    cache_flush(mbox, sizeof(mbox));

    while (*status & MBOX_FULL);
    *write = msg;

    while (1) {
        while (*status & MBOX_EMPTY);
        if (*read == msg) {
            cache_flush(mbox, sizeof(mbox));
            return mbox[1] == MBOX_RESPONSE;
        }
    }
}

uint32_t* fb_init(uint32_t width, uint32_t height) {
    if (width * height > BACK_PIXELS || (width & 1)) {
        return 0;
    }

    mbox[0] = 35 * 4;
    mbox[1] = 0;

    mbox[2] = 0x00048003;
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = width;
    mbox[6] = height;

    mbox[7] = 0x00048004;
    mbox[8] = 8;
    mbox[9] = 0;
    mbox[10] = width;
    mbox[11] = height * 3;

    mbox[12] = 0x00048009;
    mbox[13] = 8;
    mbox[14] = 0;
    mbox[15] = 0;
    mbox[16] = 0;

    mbox[17] = 0x00048005;
    mbox[18] = 4;
    mbox[19] = 0;
    mbox[20] = 32;

    mbox[21] = 0x00048006;
    mbox[22] = 4;
    mbox[23] = 0;
    mbox[24] = 0;

    mbox[25] = 0x00040001;
    mbox[26] = 8;
    mbox[27] = 0;
    mbox[28] = 4096;
    mbox[29] = 0;

    mbox[30] = 0x00040008;
    mbox[31] = 4;
    mbox[32] = 0;
    mbox[33] = 0;

    mbox[34] = 0;

    if (!mbox_call(MBOX_CH_PROP) || mbox[20] != 32 || mbox[28] == 0) {
        return 0;
    }

    fbBase = (uint32_t*)(uintptr_t)(mbox[28] & 0x3FFFFFFF);
    fbPitch = mbox[33];
    fbWidth = width;
    backPitch = width * 4;
    pageHeight = height;
    drawPage = 1;
    drawBase = backBuffer;
    lastSwap = time_us();

    mmu_init((uintptr_t)fbBase, pageHeight * PAGE_COUNT * fbPitch);

    return fbBase;
}

void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t phys_x = y;
    uint32_t phys_y = 480 - 1 - x;

    volatile uint32_t* px = (volatile uint32_t*)((uintptr_t)drawBase + phys_y * backPitch + phys_x * 4);
    *px = color;
}

void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t px = y;
    uint32_t py = 480 - x - w;
    uint32_t pw = h;
    uint32_t ph = w;

    volatile uint32_t* row = (volatile uint32_t*)((uintptr_t)drawBase + py * backPitch) + px;

    for (uint32_t j = 0; j < ph; j++) {
        for (uint32_t i = 0; i < pw; i++) {
            row[i] = color;
        }
        row = (volatile uint32_t*)((uintptr_t)row + backPitch);
    }
}

void fb_swap(void) {
    while (time_us() - lastSwap < FRAME_US);
    lastSwap = time_us();

    uintptr_t page = (uintptr_t)fbBase + drawPage * pageHeight * fbPitch;

    for (uint32_t y = 0; y < pageHeight; y++) {
        const uint64_t* src = (const uint64_t*)((uintptr_t)backBuffer + y * backPitch);
        volatile uint64_t* dst = (volatile uint64_t*)(page + y * fbPitch);

        for (uint32_t i = 0; i < fbWidth / 2; i++) {
            dst[i] = src[i];
        }
    }

    __asm__ volatile("dsb sy" ::: "memory");

    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00048009;
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = 0;
    mbox[6] = drawPage * pageHeight;
    mbox[7] = 0;
    mbox_call(MBOX_CH_PROP);

    drawPage = (drawPage + 1) % PAGE_COUNT;
}