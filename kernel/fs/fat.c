// https://github.com/bztsrc/raspi3-tutorial/blob/master/0C_directory
// (Credits to bztsrc)

#include "include/fat.h"
#include "../../includes/sd.h"
#include "../../includes/io.h"
#include "../../includes/std.h"

int memcmp(void* s1, void* s2, int n) {
    unsigned char *a = s1, *b = s2;
    while (n-->0) {
        if (*a != *b) {
            return *a - *b;
        }
        a++;
        b++;
    }

    return 0;
}

extern unsigned char _end;

static unsigned int partitionlba = 0;

typedef struct {
    char jmp[3];
    char oem[8];
    unsigned char bps0;
    unsigned char bps1;
    unsigned char spc;
    unsigned short rsc;
    unsigned char nf;
    unsigned char nr0;
    unsigned char nr1;
    unsigned short ts16;
    unsigned char media;
    unsigned short spf16;
    unsigned short spt;
    unsigned short nh;
    unsigned int hs;
    unsigned int ts32;
    unsigned int spf32;
    unsigned int flg;
    unsigned int rc;
    char vol[6];
    char fst[8];
    char dmy[20];
    char fst2[8];
} __attribute__((packed)) bpb_t;

typedef struct {
    char name[8];
    char ext[3];
    char attr[9];
    unsigned short ch;
    unsigned int attr2;
    unsigned short cl;
    unsigned int size;
} __attribute__((packed)) fatdir_t;

static bpb_t g_bpb;

int fat_getPartition(void) {
    unsigned char* mbr = &_end;
    bpb_t* bpb = (bpb_t*)&_end;

    if (sd_readBlock(0, &_end, 1)) {
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
            uart_writeText("[ERR] Bad thing happened in MBR \n");
            return 0;
        }

        if (mbr[0x1c2] != 0xe && mbr[0x1c2] != 0xc) {
            uart_writeText("[ERR] Wrong partition type \n");
            return 0;
        }

        uart_writeText("MBR disk identifier: ");
        uart_writeHex(*((unsigned int*)((unsigned long) &_end + 0x1b8)));
        uart_writeText("\nFAT partition starts from: ");

        partitionlba = mbr[0x1c6] + (mbr[0x1c7] << 8) + (mbr[0x1c8] << 16) + (mbr[0x1c9] << 24);
        uart_writeHex(partitionlba);

        uart_writeText("\n");

        if (!sd_readBlock(partitionlba, &_end, 1)) {
            uart_writeText("[ERR] Unable to read boot record\n");
            return 0;
        }

        if (!(bpb->fst[0] == 'F' && bpb->fst[1] == 'A' && bpb->fst[2] == 'T') && !(bpb->fst2[0] == 'F' && bpb->fst2[1] == 'A' && bpb->fst2[2] == 'T')) {
            uart_writeText("[ERR] Unknown fs type"); // fs = filesystem
            return 0;
        }

        uart_writeText("FAT type: ");
        uart_writeText(bpb->spf16 > 0? "FAT16" : "FAT32");
        uart_writeText("\n");

        g_bpb = *bpb;

        return 1;
    }

    return 0;
}

void fat_listDirectory(void) {
    bpb_t* bpb = &g_bpb;
    fatdir_t *dir = (fatdir_t*)(&_end + 512);
    
    unsigned int root_sec, s;
    root_sec = ((bpb->spf16 ? bpb->spf16 : bpb->spf32)* bpb->nf) + bpb->rsc;
    s = (bpb->nr0 + (bpb->nr1 + 0));

    uart_writeText("FAT number of directory entries: ");
    uart_writeHex(s);
    s *= sizeof(fatdir_t);

    if (bpb->spf16 == 0) {
        root_sec += (bpb->rc-2)* bpb->spc;
    }

    root_sec += partitionlba;
    
    uart_writeText("\nFAT root directory LBA: ");
    uart_writeHex(root_sec);

    if (sd_readBlock(root_sec, (unsigned char*)dir, bpb->spc)) {
        uart_writeText("\nAttrib Cluster   Size    Name\n");

        for (;dir->name[0] != 0; dir++) {
            if (dir->name[0] == 0xE5 || dir->attr[0] == 0xF) continue;

            uart_writeText(uart_convertHexToString(dir->attr[0] & 1 ? 'R' : '.')); // read-only
            uart_writeText(uart_convertHexToString(dir->attr[0] & 2 ?'H' : '.')); // hidden
            uart_writeText(uart_convertHexToString(dir->attr[0] & 4 ?'S' : '.')); // system
            uart_writeText(uart_convertHexToString(dir->attr[0] & 8 ? 'L' : '.')); // volume label
            uart_writeText(uart_convertHexToString(dir->attr[0] & 16 ? 'D' : '.')); // directory
            uart_writeText(uart_convertHexToString(dir->attr[0] & 32 ? 'A' : '.')); // archive

            uart_writeText(" ");

            uart_writeHex(((unsigned int)dir->ch) << 16 | dir->cl);
            uart_writeText(" ");

            uart_writeHex(dir->size);
            uart_writeText(" ");

            dir->attr[0] = 0;
            uart_writeText(dir->name);
            uart_writeText("\n");
        }
    } else {
        uart_writeText("[ERR] Unable to load root dir\n");
    }
}

unsigned int fat_getCluster(char *fn) {
    bpb_t* bpb = &g_bpb;
    fatdir_t* dir = (fatdir_t*)(&_end + 512);
    unsigned int root_sec, s;

    root_sec = ((bpb->spf16 ? bpb->spf16 : bpb->spf32)* bpb->nf) + bpb->rsc;
    s = (bpb->nr0 + (bpb->nr1 << 8)) * sizeof(fatdir_t);
    if (bpb->spf16 == 0) {
        root_sec += (bpb->rc - 2)*bpb->spc;
    }

    root_sec += partitionlba;

    if (sd_readBlock(root_sec, (unsigned char*)dir, bpb->spf16 ? s/512+1 : bpb->spc)) {
        for (;dir->name[0] != 0; dir++) {
            if (dir->name[0] == 0xE5 || dir->attr[0] == 0xF) continue;

            if (!memcmp(dir->name, fn, 11)) {
                uart_writeText("FAT File ");
                uart_writeText(fn);
                uart_writeText(" start at cluster: ");
                uart_writeHex(((unsigned int)dir->ch) << 16 | dir->cl);
                uart_writeText("\n");

                return((unsigned int)dir->ch) << 16 | dir->cl;
            }
        }

        uart_writeText("[ERR] File not found!\n");
    } else {
        uart_writeText("[ERR] Unable to load root directory");
    }

    return 0;
}

char* fat_readFile(unsigned int cluster) {
    bpb_t* bpb = &g_bpb;
    unsigned int* fat32 = (unsigned int*)(&_end + bpb->rsc*512);
    unsigned short* fat16 = (unsigned short*)fat32;

    unsigned int data_sec, s;
    unsigned char *data, *ptr;

    data_sec = ((bpb->spf16 ? bpb->spf16 : bpb->spf32)*bpb->nf) + bpb->rsc;
    s = (bpb->nr0 + (bpb->nr1 << 8)) * sizeof(fatdir_t);
    if (bpb->spf16 > 0) {
        data_sec += (s + 511) >> 9;
    }

    data_sec += partitionlba;

    uart_writeText("FAT Bytes per Sector: ");
    uart_writeHex(bpb->bps0 + (bpb->bps1 << 8));
    uart_writeText("\nFAT Sectors per Cluster: ");
    uart_writeHex(bpb->spc);
    uart_writeText("\nFAT Number of FAT: ");
    uart_writeHex(bpb->nf);
    uart_writeText("\nFAT Sectors per FAT: ");
    uart_writeHex((bpb->spf16 ? bpb->spf16 : bpb->spf32));
    uart_writeText("\nFAT Reserved Sectors count: ");
    uart_writeHex(bpb->rsc);
    uart_writeText("\nFAT First data sector: ");
    uart_writeHex(data_sec);
    uart_writeText("\n");

    // load FAT table
    s = sd_readBlock(partitionlba + 1, (unsigned char*)&_end + 512, (bpb->spf16 ? bpb->spf16 : bpb->spf32) + bpb->rsc);
    // Fat ends here from memory
    data = ptr=&_end + 512 + s;

    // "(Yep, MS is full of lies. FAT32 is actually FAT28 only, no mistake, the upper 4 bits must be zero)" - bztsrc
    while (cluster > 1 && cluster < (bpb->spf16 > 0 ? 0xFFF8 : 0x0FFFFFF8)) {
        sd_readBlock((cluster - 2)*bpb->spc + data_sec, ptr, bpb->spc);
        ptr += bpb->spc*(bpb->bps0 + (bpb->bps1 << 8));
        cluster = bpb->spf16 > 0 ? fat16[cluster] : fat32[cluster];
    }
    return (char*)data;
}