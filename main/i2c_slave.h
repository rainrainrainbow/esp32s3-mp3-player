#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdint.h>

void i2c_slave_init(void);
uint8_t i2c_slave_read_reg(uint8_t reg);
void i2c_slave_write_reg(uint8_t reg, uint8_t val);

#endif // I2C_SLAVE_H