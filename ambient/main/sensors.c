#include "sensors.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "bmp388.h"
#include "sht45.h"
#include "thermistor.h"

static const char *TAG = "sensors";

static const char *const s_names[SENSOR_COUNT] = { "thermistor", "sht45", "bmp388" };
static const char *const s_types[SENSOR_COUNT][SENSOR_MAX_VALUES] = {
    { "temp_c", "ratio" },
    { "temp_c", "rh_pct" },
    { "temp_c", "pressure_pa" },
};

static SemaphoreHandle_t s_lock;          // one sample at a time
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sensor_reading_t s_last[SENSOR_COUNT];

const char *sensor_name(sensor_id_t id)
{
    return id < SENSOR_COUNT ? s_names[id] : "?";
}

int sensor_value_count(sensor_id_t id)
{
    return id < SENSOR_COUNT ? SENSOR_MAX_VALUES : 0;
}

const char *sensor_data_type(sensor_id_t id, int i)
{
    return id < SENSOR_COUNT && i >= 0 && i < SENSOR_MAX_VALUES ? s_types[id][i] : "?";
}

void sensors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (thermistor_init() != 0) {
        ESP_LOGE(TAG, "thermistor: ADS1115 device setup failed");
    }
    if (sht45_init() != 0) {
        ESP_LOGE(TAG, "sht45: device setup failed");
    }
    if (bmp388_init() != 0) {
        ESP_LOGE(TAG, "bmp388: device setup failed");
    }
}

// Store one result and log a change: first failure, a new reason, recovery.
static void record(sensor_id_t id, const char *why, const double *v, int64_t utc_us)
{
    portENTER_CRITICAL(&s_mux);
    sensor_reading_t *r = &s_last[id];
    bool was_ok = !r->read || r->ok;
    const char *was_err = r->err;
    uint32_t was_fails = r->fails;
    r->read = true;
    r->ok = (why == NULL);
    r->err = why;
    if (why) {
        r->fails++;
    } else {
        r->fails = 0;
        r->utc_us = utc_us;
        memcpy(r->value, v, sizeof r->value);
    }
    portEXIT_CRITICAL(&s_mux);

    if (why && (was_ok || strcmp(why, was_err) != 0)) {
        ESP_LOGW(TAG, "%s failed: %s; the other sensors carry on", s_names[id], why);
    } else if (!why && !was_ok) {
        ESP_LOGI(TAG, "%s reading again after %lu failed", s_names[id], (unsigned long)was_fails);
    }
}

void sensors_sample(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    double v[SENSOR_MAX_VALUES];
    int64_t utc_us = 0;
    const char *why;

    why = thermistor_read(&v[0], &v[1], &utc_us);
    record(SENSOR_THERMISTOR, why, v, utc_us);
    why = sht45_read(&v[0], &v[1], &utc_us);
    record(SENSOR_SHT45, why, v, utc_us);
    why = bmp388_read(&v[0], &v[1], &utc_us);
    record(SENSOR_BMP388, why, v, utc_us);

    xSemaphoreGive(s_lock);
}

void sensors_get(sensor_id_t id, sensor_reading_t *out)
{
    if (id >= SENSOR_COUNT) {
        memset(out, 0, sizeof *out);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_last[id];
    portEXIT_CRITICAL(&s_mux);
}
