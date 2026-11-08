/* Sources:
    https://rpi4os.com/part3-helloworld/
*/

#pragma once
#include "std.h"

#define MMIO_BASE 0xFE000000

#define GPFSEL0 ((volatile unsigned int*)(MMIO_BASE+0x00200000))
#define GPFSEL1 ((volatile unsigned int*)(MMIO_BASE+0x00200004))
#define GPFSEL2 ((volatile unsigned int*)(MMIO_BASE+0x00200008))
#define GPFSEL3 ((volatile unsigned int*)(MMIO_BASE+0x0020000C))
#define GPFSEL4 ((volatile unsigned int*)(MMIO_BASE+0x00200010))
#define GPFSEL5 ((volatile unsigned int*)(MMIO_BASE+0x00200014))
#define GPSET0 ((volatile unsigned int*)(MMIO_BASE+0x0020001C))
#define GPSET1 ((volatile unsigned int*)(MMIO_BASE+0x00200020))
#define GPCLR0 ((volatile unsigned int*)(MMIO_BASE+0x00200028))
#define GPLEV0 ((volatile unsigned int*)(MMIO_BASE+0x00200034))
#define GPLEV1 ((volatile unsigned int*)(MMIO_BASE+0x00200038))
#define GPEDS0 ((volatile unsigned int*)(MMIO_BASE+0x00200040))
#define GPEDS1 ((volatile unsigned int*)(MMIO_BASE+0x00200044))
#define GPHEN0 ((volatile unsigned int*)(MMIO_BASE+0x00200064))
#define GPHEN1 ((volatile unsigned int*)(MMIO_BASE+0x00200068))
#define GPPUD ((volatile unsigned int*)(MMIO_BASE+0x00200094))
#define GPPUDCLK0 ((volatile unsigned int*)(MMIO_BASE+0x00200098))
#define GPPUDCLK1 ((volatile unsigned int*)(MMIO_BASE+0x0020009C))

void mmio_write(long reg, unsigned int val);
unsigned int mmio_read(long reg) ;

unsigned int gpio_set(unsigned int pin_number, unsigned int value);
unsigned int gpio_clear(unsigned int pin_number, unsigned int value);
unsigned int gpio_pull(unsigned int pin_number, unsigned int value);
unsigned int gpio_function(unsigned int pin_number, unsigned int value);

void uart_init();
void uart_writeText(const char* buffer);
void uart_writeHex(unsigned int d);
char* uart_convertHexToString(unsigned int hex);
void uart_write_bytes(const char* buffer, size_t length);
/* Nonblocking: 0 means empty, 1 a byte, 2 receive overrun (discard the line). */
uint32_t uart_read_byte(uint8_t* byte);
