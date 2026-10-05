#include "supply.h"

#include "freertos/FreeRTOS.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "supply";

#define SUPPLY_GPIO        34
#define SUPPLY_SAMPLES     16      // averaged per reading
// VIN / pin: (R1 + R2) / R2 with R1 = R2 (both measured 99.7 kOhm on board 2).
#define DIVIDER_NUM        2
#define DIVIDER_DEN        1
// Clear "low" only once VIN is this far back above the threshold.
#define RECOVER_MARGIN_MV  50

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static const char *s_cal_name = "none";
static bool s_ok;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static supply_state_t s_state;

int supply_init(void)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(SUPPLY_GPIO, &unit, &s_chan);
    if (err == ESP_OK) {
        adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
        err = adc_oneshot_new_unit(&ucfg, &s_adc);
    }
    if (err == ESP_OK) {
        // 12 dB attenuation: the widest input range (to about 3.1 V).
        adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        err = adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC setup failed: %s; no supply readings", esp_err_to_name(err));
        return -1;
    }

    // Calibration from the reference voltage burnt into this chip's eFuse
    // (esptool reported "Vref calibration in eFuse" for board 2). Without it
    // the conversion falls back to a nominal reference: less accurate, still
    // usable.
    adc_cali_line_fitting_config_t cal = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = 1100,
    };
    adc_cali_line_fitting_efuse_val_t src = ADC_CALI_LINE_FITTING_EFUSE_VAL_DEFAULT_VREF;
    if (adc_cali_scheme_line_fitting_check_efuse(&src) == ESP_OK &&
        adc_cali_create_scheme_line_fitting(&cal, &s_cali) == ESP_OK) {
        s_cal_name = src == ADC_CALI_LINE_FITTING_EFUSE_VAL_EFUSE_TP   ? "eFuse two-point"
                   : src == ADC_CALI_LINE_FITTING_EFUSE_VAL_EFUSE_VREF ? "eFuse Vref"
                                                                         : "default Vref";
    } else {
        ESP_LOGW(TAG, "no ADC calibration; supply readings will be rough");
        s_cali = NULL;
    }
    s_ok = true;
    return 0;
}

// Pin voltage in mV, averaged over SUPPLY_SAMPLES reads.
static int read_pin_mv(int *mv)
{
    int sum = 0;
    for (int i = 0; i < SUPPLY_SAMPLES; i++) {
        int raw;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    int raw = sum / SUPPLY_SAMPLES;
    if (s_cali) {
        return adc_cali_raw_to_voltage(s_cali, raw, mv) == ESP_OK ? 0 : -1;
    }
    *mv = raw * 3100 / 4095;    // uncalibrated: nominal full scale ~3.1 V
    return 0;
}

int supply_check(uint32_t low_mv, uint32_t *vin_mv)
{
    int pin_mv;
    if (!s_ok || read_pin_mv(&pin_mv) != 0) {
        return -1;
    }
    uint32_t vin = (uint32_t)pin_mv * DIVIDER_NUM / DIVIDER_DEN;

    portENTER_CRITICAL(&s_mux);
    bool was_low = s_state.low;
    s_state.valid = true;
    s_state.vin_mv = vin;
    if (!was_low && vin < low_mv) {
        s_state.low = true;
        s_state.n_low++;
    } else if (was_low && vin >= low_mv + RECOVER_MARGIN_MV) {
        s_state.low = false;
    }
    bool now_low = s_state.low;
    portEXIT_CRITICAL(&s_mux);

    // Shown regardless of quiet mode: these are problems (and their end).
    // The SD card's problem log takes these entries from stage 21.
    if (now_low && !was_low) {
        ESP_LOGW(TAG, "supply low: %lu mV (threshold %lu mV)", (unsigned long)vin, (unsigned long)low_mv);
    } else if (!now_low && was_low) {
        ESP_LOGW(TAG, "supply recovered: %lu mV", (unsigned long)vin);
    }
    *vin_mv = vin;
    return 0;
}

void supply_get(supply_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
}

const char *supply_cal_name(void)
{
    return s_cal_name;
}
