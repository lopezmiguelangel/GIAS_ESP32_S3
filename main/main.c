// C estándar
#include <stdio.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "esp_log.h"
#include "driver/gpio.h"

// Proyecto
#include "gias_system.h"
#include "led.h"
#include "power.h"

#define GRABAR  1
#define REPOSO  0

static const char *TAG = "MAIN";

// MAIN
void app_main(void)
{
    gpio_deep_sleep_hold_dis();
    // LED
    led_init();

    vTaskDelay(pdMS_TO_TICKS(1000));
    
    //ESP_LOGI(TAG, "Iniciando sistema GIAS");

    power_init();

    while (1) {

        gias_status_t status = gias_get_status();

        if (status.estado == GRABAR) {

            //ESP_LOGI(TAG, "GRABAR por %d minutos", status.minutos);

            gias_record_start(status.minutos, &status.hora);

            //ESP_LOGI(TAG, "Grabación finalizada");

        } else if (status.estado == REPOSO) {

            //ESP_LOGI(TAG, "REPOSO por %d minutos", status.minutos);

            power_deep_sleep(status.minutos);

        } else {

            //ESP_LOGE(TAG, "Error en inicialización");

            gias_error_handler(2);
        }

    }
}