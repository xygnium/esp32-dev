#include "fault.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "ambient";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fault_src_t s_src = FAULT_NONE;
static const char *s_reason = "";

void fault_raise(fault_src_t src, const char *reason)
{
    bool first = false;
    portENTER_CRITICAL(&s_mux);
    if (s_src == FAULT_NONE) {
        s_src = src;
        s_reason = reason;
        first = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (first) {
        ESP_LOGE(TAG, "HARD FAULT %s: %s. Normal work stopped; fix it and reboot",
                 fault_src_name(src), reason);
    } else {
        ESP_LOGE(TAG, "also failed: %s: %s", fault_src_name(src), reason);
    }
}

bool fault_get(fault_src_t *src, const char **reason)
{
    portENTER_CRITICAL(&s_mux);
    fault_src_t s = s_src;
    const char *r = s_reason;
    portEXIT_CRITICAL(&s_mux);
    if (src) *src = s;
    if (reason) *reason = r;
    return s != FAULT_NONE;
}

const char *fault_src_name(fault_src_t src)
{
    switch (src) {
    case FAULT_SD:   return "sd";
    case FAULT_NONE: break;
    }
    return "none";
}
