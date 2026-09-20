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
    led_init();
    power_init();

    struct tm hora = {0};
    led_blink_count(2);

    while (1) {

        gias_status_t status = gias_get_status();

        if (status.estado == GRABAR) {   
            xTaskCreate(led_status_task, "led_status", 2048, NULL, 1, &led_task_handle);         
            gias_record_start(status.minutos, &status.hora);
        } else if (status.estado == REPOSO) {
            power_deep_sleep(status.minutos);

        } else {
            gias_error_handler(5);
        }

    }
}