#pragma once

#include <vantage/base.h>

void i2c_init(void);
uint32_t i2c_write(uint8_t addr, const uint8_t* data, uint32_t len);
uint32_t i2c_read(uint8_t addr, uint8_t* data, uint32_t len);
uint32_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t* data, uint32_t len);
uint32_t i2c_probe(uint8_t addr);
