/* Sources:
    https://rpi4os.com/part3-helloworld/
*/

#include "../includes/io.h"

enum {
    PERIPHERAL_BASE = MMIO_BASE,
    IO_GPFSEL0 = PERIPHERAL_BASE + 0x200000, // GPIO FUNCTION SELECT 0 is a 32 bit reg to configure GPIO 0 to 9
    IO_GPSET0 = PERIPHERAL_BASE + 0x20001C, // GPIO PIN OUTPUT SET 0 is used to map the first 32 GPIO pins
    IO_GPCLR0 = PERIPHERAL_BASE + 0x200028, // GPIO PIN OUTPUT CLEAR 0 is used to turn GPIO pins to 0
    GPPUPPDN0 = PERIPHERAL_BASE + 0x2000E4, // GPIO PIN PULL UP/DOWN REGISTER 0 (too long to explain)
};

enum {
    GPIO_MAX_PIN = 57,
    GPIO_FUNC_ALT5 = 2,
};

enum {
    pull_none = 0,
};

#ifndef VANTAGE_SIM
// Host driver tests supply MMIO functions without accessing physical registers.
void mmio_write(long reg, unsigned int val) {
    *(volatile unsigned int *)reg = val;
}

unsigned int mmio_read(long reg) {
    return *(volatile unsigned int *)reg;
}
#endif

unsigned int gpio_call(unsigned int pin_number, unsigned int value, unsigned int base, unsigned int field_size, unsigned int field_max) {
    unsigned int field_mask = (1 << field_size) - 1;

    if (pin_number > field_max) return 0;
    if (value > field_mask) return 0;

    unsigned int num_fields = 32 / field_size;
    unsigned int reg = base + ((pin_number /num_fields) * 4);
    unsigned int shift = (pin_number % num_fields) * field_size;

    unsigned int curval = mmio_read(reg);
    curval &= ~(field_mask << shift);
    curval |= value << shift;
    mmio_write(reg, curval);

    return 0;
}

unsigned int gpio_set(unsigned int pin_number, unsigned int value) {
    return gpio_call(pin_number, value, (unsigned int)IO_GPSET0, 1, GPIO_MAX_PIN);
}

unsigned int gpio_clear(unsigned int pin_number, unsigned int value) {
    return gpio_call(pin_number, value, (unsigned int)IO_GPCLR0, 1, GPIO_MAX_PIN);

}
unsigned int gpio_pull(unsigned int pin_number, unsigned int value) {
    return gpio_call(pin_number, value, (unsigned int)GPPUPPDN0, 2, GPIO_MAX_PIN);
}

unsigned int gpio_function(unsigned int pin_number, unsigned int value) {
    return gpio_call(pin_number, value, (unsigned int)IO_GPFSEL0, 3, GPIO_MAX_PIN);
}

void gpio_useAsAlt5(unsigned int pin_number) {
    gpio_pull(pin_number, pull_none);
    gpio_function(pin_number, GPIO_FUNC_ALT5);
}

// UART

enum {
    AUX_BASE = PERIPHERAL_BASE + 0x215000,
    AUX_ENABLES = AUX_BASE + 4,
    AUX_MU_IO_REG = AUX_BASE + 64,
    AUX_MU_IER_REG = AUX_BASE + 68,
    AUX_MU_IIR_REG = AUX_BASE + 72,
    AUX_MU_LCR_REG = AUX_BASE + 76,
    AUX_MU_MCR_REG = AUX_BASE + 80,
    AUX_MU_LSR_REG = AUX_BASE + 84,
    AUX_MU_CNTL_REG = AUX_BASE + 96,
    AUX_MU_BAUD_REG = AUX_BASE + 104,
    AUX_UART_CLOCK = 500000000,
    UART_MAX_QUEUE = 16 * 1024
};

#define AUX_MU_BAUD(baud) ((AUX_UART_CLOCK/(baud*8))-1)

void uart_init() {
    mmio_write(AUX_ENABLES, 1); // enable UART1
    mmio_write(AUX_MU_IER_REG, 0);
    mmio_write(AUX_MU_CNTL_REG, 0);
    mmio_write(AUX_MU_LCR_REG, 3); // 8 bits
    mmio_write(AUX_MU_MCR_REG, 0);
    mmio_write(AUX_MU_IER_REG, 0);
    mmio_write(AUX_MU_IIR_REG, 0xC6); // disable interrupts
    mmio_write(AUX_MU_BAUD_REG, AUX_MU_BAUD(115200));
    gpio_useAsAlt5(14);
    gpio_useAsAlt5(15);
    mmio_write(AUX_MU_CNTL_REG, 3); // enable RX/TX
}

unsigned int uart_isWriteByteReady() {
    return mmio_read(AUX_MU_LSR_REG) & 0x20; 
}

void uart_writeByteBlockingActual(unsigned char ch) {
    while (!uart_isWriteByteReady()); 
    mmio_write(AUX_MU_IO_REG, (unsigned int)ch);
}

void uart_writeText(const char *buffer) {
    while (*buffer) {
       if (*buffer == '\n') uart_writeByteBlockingActual('\r');
       uart_writeByteBlockingActual(*buffer++);
    }
}

void uart_write_bytes(const char* buffer, size_t length) {
    size_t i;
    for (i = 0; i < length; i++) {
        if (buffer[i] == '\n') uart_writeByteBlockingActual('\r');
        uart_writeByteBlockingActual((unsigned char)buffer[i]);
    }
}

uint32_t uart_read_byte(uint8_t* byte) {
    uint32_t status;
    if (!byte) return 0;
    status = mmio_read(AUX_MU_LSR_REG);
    if (status & 2) {
        if (status & 1) (void)mmio_read(AUX_MU_IO_REG);
        return 2;
    }
    if (!(status & 1)) return 0;
    *byte = (uint8_t)mmio_read(AUX_MU_IO_REG);
    return 1;
}
