// https://github.com/bztsrc/raspi3-tutorial/blob/master/0C_directory
// (Credits to bztsrc)

#pragma once

int fat_getPartition(void);
void fat_listDirectory(void);
unsigned int fat_getCluster(char* fn);
char* fat_readFile(unsigned int cluster);
