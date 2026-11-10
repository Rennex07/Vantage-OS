#include "../fs/block.h"
#include <stdio.h>

void uart_writeText(const char* text) { (void)text; }

static int check_block(struct device_block* dev, unsigned int block) {
    unsigned char input[BLOCK_SIZE];
    unsigned char output[BLOCK_SIZE * 2];
    unsigned int i;

    for (i = 0; i < sizeof(input); i++) input[i] = (unsigned char)(i + block);
    memset(output, 0xcc, sizeof(output));
    if (dev->write(dev, block, input) || dev->read(dev, block, output) ||
        memcmp(input, output, BLOCK_SIZE)) return 0;
    for (i = BLOCK_SIZE; i < sizeof(output); i++) {
        if (output[i] != 0xcc) {
            fprintf(stderr, "Read of block %u overwrote byte %u beyond the block\n", block, i);
            return 0;
        }
    }
    return 1;
}

int main(void) {
    struct device_block dev = {0};
    unsigned char output[BLOCK_SIZE * 2];
    unsigned int i;
    ramdisk_init(&dev);
    if (!check_block(&dev, 0) || !check_block(&dev, dev.total_blocks - 1)) return 1;
    memset(output, 0xcc, sizeof(output));
    if (dev.read(&dev, dev.total_blocks, output) != -1) return 1;
    for (i = 0; i < sizeof(output); i++) if (output[i] != 0xcc) return 1;
    puts("PASS: first/last block round trips, read bounds and destination canaries");
    return 0;
}
