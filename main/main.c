// C estándar
#include <stdio.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "esp_log.h"

// Proyecto
#include "gias_system.h"

#define GRABAR  1
#define REPOSO  0

static const char *TAG = "MAIN";

// MAIN
void app_main(void)
{
    // Silenciar logs de WiFi/PHY
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("phy", ESP_LOG_WARN);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_WARN);

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Iniciando sistema GIAS");

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Iniciando sistema GIAS");

    gias_init(); // Inicialización de hardware y tareas

    while (1) {    // Loop principal: consulta estado y actúa (grabar / reposo / error)

        ESP_LOGI(TAG, "Verificando estado del sistema");

        gias_status_t status = gias_get_status();

        if (status.estado == GRABAR) {

            ESP_LOGI(TAG, "GRABAR por %d minutos", status.minutos);

            gias_record_start(status.minutos, &status.hora);

        } else if (status.estado == REPOSO) {

            ESP_LOGI(TAG, "REPOSO por %d minutos", status.minutos);

            gias_deep_sleep(status.minutos);

        } else {

            ESP_LOGE(TAG, "Error en inicialización");

            gias_error_handler(2); // Debug.
        }
    }
}