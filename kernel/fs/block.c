#include "include/block.h"
#include "../../includes/std.h"
#include "../../includes/io.h"

int ramdisk_read (struct device_block *dev, unsigned int block_num, void* buf) {
    if (block_num >= dev->total_blocks) {
        uart_writeText("[!] Block number out of range! \n");
        return -1;
    }

    unsigned char* src = ramdisk_mem + (block_num * dev->block_size);
    memcpy(buf, src, dev->block_size);
    return 0;
}

int ramdisk_write (struct device_block *dev, unsigned int block_num, const void* buf) {
    if (block_num >= dev->total_blocks) {
        uart_writeText("[!] Block number out of range! \n");
        return -1;
    }

    unsigned char *dst = ramdisk_mem + (block_num * dev->block_size);
    memcpy(dst, buf, dev->block_size);
    return 0;
}

void ramdisk_init(struct device_block* dev) {
    const char* name = "ramdisk0";

    for (int i = 0; name[i] != '\0'; i++) {
        dev->name[i] = name[i];
    }
    dev->name[8] = '\0';

    dev->block_size = BLOCK_SIZE;
    dev->total_blocks = RAMDISK_BLOCKS;
    dev->read = ramdisk_read;
    dev->write = ramdisk_write;

    uart_writeText("[OK] RAMDISK INITIALIZED! \n");
}
