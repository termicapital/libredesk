#include "display_spike.h"

#include "display_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_spike";

void display_spike_run_forever(void)
{
    display_port_t port = {0};
    const esp_err_t error = display_port_start(&port, true);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "display port startup failed: %s", esp_err_to_name(error));
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
