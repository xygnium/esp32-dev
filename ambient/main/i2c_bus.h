#ifndef AMBIENT_I2C_BUS_H
#define AMBIENT_I2C_BUS_H

#include "driver/i2c_master.h"

// I2C bus 0: SDA 21, SCL 22 (WIRING.md). Shared by the DS3231 and, later,
// the sensors. Each device sets its own speed (100 kHz here).

// The bus, created on first use. NULL if it couldn't be created.
i2c_master_bus_handle_t i2c_bus_get(void);

#endif
