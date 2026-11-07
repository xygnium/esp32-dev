#ifndef AMBIENT_SENSORS_H
#define AMBIENT_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

// One sample = one reading from each sensor, in this order: the thermistor,
// the SHT45 straight after (so the two temperatures are close in time), then
// the BMP388. Each reading has its own UTC stamp (us), taken when its
// measurement finished; a sensor's values share it.
//
// A failed sensor is a soft failure: the others carry on, and it is tried
// again at the next sample. Failures and recoveries are logged.

typedef enum { SENSOR_THERMISTOR = 0, SENSOR_SHT45, SENSOR_BMP388, SENSOR_COUNT } sensor_id_t;

#define SENSOR_MAX_VALUES 2

typedef struct {
    bool read;                      // a reading has been tried since boot
    bool ok;                        // the last reading succeeded
    const char *err;                // its failure reason, if not
    uint32_t fails;                 // failed readings in a row
    int64_t utc_us;                 // stamp of the last good reading; 0 none
    double value[SENSOR_MAX_VALUES];// the last good values
} sensor_reading_t;

const char *sensor_name(sensor_id_t id);              // thermistor, sht45, bmp388
int sensor_value_count(sensor_id_t id);
const char *sensor_data_type(sensor_id_t id, int i);  // temp_c, ratio, rh_pct, pressure_pa
int sensor_decimals(sensor_id_t id, int i);           // decimals kept when logged (FIRMWARE.md)

// One sensor's result from one sample. A failed reading has its reason in
// err, no values, and the time the failure was seen as its stamp.
typedef struct {
    const char *err;                // NULL when the reading succeeded
    int64_t utc_us;
    double value[SENSOR_MAX_VALUES];
} sensor_result_t;

// At boot, after the I2C bus exists (clock_init).
void sensors_init(void);

// Read all three sensors (about 0.6 s). Safe from the loop and the console.
// out, if not NULL, gets this sample's results (for the SD log).
void sensors_sample(sensor_result_t out[SENSOR_COUNT]);

void sensors_get(sensor_id_t id, sensor_reading_t *out);

#endif
