#include <vantage/platform/pi4/i2c.h>
#include <vantage/platform/pi4/display.h>
#include <vantage/platform/pi4/io.h>

#define GPIO_BASE 0xFE200000
#define I2C_GPFSEL4 (GPIO_BASE + 0x10)
#define GPPUPPDN2 (GPIO_BASE + 0xEC)
#define GPIO_FUNC_ALT1 5

#define BSC0_BASE 0xFE205000
#define BSC_C (BSC0_BASE + 0x00)
#define BSC_S (BSC0_BASE + 0x04)
#define BSC_DLEN (BSC0_BASE + 0x08)
#define BSC_A (BSC0_BASE + 0x0C)
#define BSC_FIFO (BSC0_BASE + 0x10)
#define BSC_DIV (BSC0_BASE + 0x14)

#define C_I2CEN (1 << 15)
#define C_ST (1 << 7)
#define C_CLEAR (1 << 4)
#define C_READ (1 << 0)

#define S_CLKT (1 << 9)
#define S_ERR (1 << 8)
#define S_RXD (1 << 5)
#define S_TXD (1 << 4)
#define S_DONE (1 << 1)

#define I2C_DIV 5000
#define I2C_TIMEOUT_US 20000

static volatile uint32_t* reg(uint32_t addr) {
    return (volatile uint32_t*)(uintptr_t)addr;
}

static void begin(uint8_t addr, uint32_t len) {
    *reg(BSC_C) = C_I2CEN | C_CLEAR;
    *reg(BSC_S) = S_CLKT | S_ERR | S_DONE;
    *reg(BSC_A) = addr;
    *reg(BSC_DLEN) = len;
}

static uint32_t finish(uint32_t ok) {
    uint32_t start = time_us();

    while (!(*reg(BSC_S) & S_DONE)) {
        if (time_us() - start > I2C_TIMEOUT_US) {
            ok = 0;
            break;
        }
    }

    uint32_t status = *reg(BSC_S);
    *reg(BSC_S) = S_CLKT | S_ERR | S_DONE;
    *reg(BSC_C) = C_I2CEN | C_CLEAR;

    return ok && !(status & (S_CLKT | S_ERR));
}

void i2c_init(void) {
    uint32_t fsel = *reg(I2C_GPFSEL4);
    fsel &= ~((7 << 12) | (7 << 15));
    fsel |= (GPIO_FUNC_ALT1 << 12) | (GPIO_FUNC_ALT1 << 15);
    *reg(I2C_GPFSEL4) = fsel;

    uint32_t pull = *reg(GPPUPPDN2);
    pull &= ~((3 << 24) | (3 << 26));
    *reg(GPPUPPDN2) = pull;

    *reg(BSC_DIV) = I2C_DIV;
    *reg(BSC_S) = S_CLKT | S_ERR | S_DONE;
    *reg(BSC_C) = C_I2CEN | C_CLEAR;

    uart_writeText("[OK] Vantage I2C driver...\n");
}

uint32_t i2c_write(uint8_t addr, const uint8_t* data, uint32_t len) {
    begin(addr, len);
    *reg(BSC_C) = C_I2CEN | C_ST;

    uint32_t sent = 0;
    uint32_t start = time_us();

    while (sent < len) {
        uint32_t status = *reg(BSC_S);

        if (status & S_TXD) {
            *reg(BSC_FIFO) = data[sent++];
        } else if (status & (S_ERR | S_CLKT)) {
            break;
        } else if (time_us() - start > I2C_TIMEOUT_US) {
            break;
        }
    }

    return finish(sent == len);
}

uint32_t i2c_read(uint8_t addr, uint8_t* data, uint32_t len) {
    begin(addr, len);
    *reg(BSC_C) = C_I2CEN | C_ST | C_READ;

    uint32_t got = 0;
    uint32_t start = time_us();

    while (got < len) {
        uint32_t status = *reg(BSC_S);

        if (status & S_RXD) {
            data[got++] = (uint8_t)*reg(BSC_FIFO);
        } else if (status & (S_ERR | S_CLKT)) {
            break;
        } else if (time_us() - start > I2C_TIMEOUT_US) {
            break;
        }
    }

    return finish(got == len);
}

uint32_t i2c_read_reg(uint8_t addr, uint8_t regAddr, uint8_t* data, uint32_t len) {
    return i2c_write(addr, &regAddr, 1) && i2c_read(addr, data, len);
}

uint32_t i2c_probe(uint8_t addr) {
    uint8_t byte;
    return i2c_read(addr, &byte, 1);
}
