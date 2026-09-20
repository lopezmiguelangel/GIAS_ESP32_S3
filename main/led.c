// Control del LED de estado y parpadeo de códigos de error

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

// Proyecto
#include "led.h"

static const char *TAG = "LED";

// LED
#define PIN_LED GPIO_NUM_48
#define LED_ON  0
#define LED_OFF 1

TaskHandle_t led_task_handle = NULL;

// Parpadeo lento del LED para indicar que el sistema está vivo
void led_status_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Led status task creado");
    while(1) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(1950));
    }
    vTaskDelete(NULL);
}

void led_blink_count(int cantidad)
{
    for (int i = 0; i < cantidad; i++) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(1000));

        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Configura el LED y crea la tarea de parpadeo
void led_init(void)
{
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, LED_OFF);
    //xTaskCreate(led_status_task, "led_status", 2048, NULL, 1, &led_task_handle);
    //ESP_LOGI(TAG, "LED inicializado");
}

// Detiene la tarea LED y parpadea un código antes de reiniciar
void gias_error_handler(int titileos)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    //ESP_LOGE(TAG, "Error handler: %d titileos", titileos);
    if (led_task_handle != NULL) {
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
    }

    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);

    // Preámbulo fijo
    for (int i = 0; i < 20; i++) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Código: "titileos" destellos lentos
    for (int i = 0; i < titileos; i++) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(1500));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    // Reinicio
    gpio_set_level(PIN_LED, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(10000));

    esp_restart();
}

