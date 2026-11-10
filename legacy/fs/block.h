#pragma once
#include <vantage/base.h>

#define BLOCK_SIZE 512
#define RAMDISK_BLOCKS 1024

struct device_block {
  char name[32];
  unsigned int block_size;
  unsigned int total_blocks;
  int (*read)(struct device_block *dev, unsigned int block_num, void *buf);
  int (*write)(struct device_block *dev, unsigned int block_num,
               const void *buf);
};

static unsigned char ramdisk_mem[BLOCK_SIZE * RAMDISK_BLOCKS];

int ramdisk_read(struct device_block *dev, unsigned int block_num, void *buf);
int ramdisk_write(struct device_block *dev, unsigned int block_num,
                  const void *buf);
void ramdisk_init(struct device_block *dev);
