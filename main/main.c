#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "gias_system.h"

static const char *TAG = "MAIN";


// ============================================================
// MAIN
// ============================================================

void app_main(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Iniciando sistema GIAS");

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Iniciando sistema GIAS");

    gias_init();

    while (1) {

        ESP_LOGI(TAG, "Verificando estado del sistema");

        gias_status_t status = gias_get_status();

        if (status.estado == 1) {

            ESP_LOGI(TAG, "GRABAR por %d minutos", status.minutos);

            gias_record_start(status.minutos, &status.hora);

        } else if (status.estado == 0) {

            ESP_LOGI(TAG, "REPOSO por %d minutos", status.minutos);

            gias_deep_sleep(status.minutos);

        } else {

            ESP_LOGE(TAG, "Error en inicialización");

            gias_error_handler();
        }
    }
}