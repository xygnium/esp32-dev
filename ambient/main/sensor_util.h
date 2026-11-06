#ifndef AMBIENT_SENSOR_UTIL_H
#define AMBIENT_SENSOR_UTIL_H

#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"

// Shared by the sensor drivers.

// A short failure reason for an I2C error.
static inline const char *sensor_err_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_RESPONSE: return "nack";    // no device answered
    case ESP_ERR_TIMEOUT:          return "timeout";
    default:                       return "bus";
    }
}

// The ESP32 clock (UTC) in microseconds.
static inline int64_t sensor_utc_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

#endif
