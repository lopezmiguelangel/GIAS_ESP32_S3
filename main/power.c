// Control de alimentación de periféricos (SD+RTC e I2S)

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "driver/gpio.h"
//#include "esp_log.h"

// Proyecto
#include "power.h"
#include "sdmmc_manager.h"
#include "i2s_audio.h"
#include "rtc_wifi.h"
#include "esp_sleep.h"
#include "led.h"

static const char *TAG = "POWER";

// Pines de alimentación
#define POWER_SD_RTC_PIN GPIO_NUM_45
#define POWER_I2S_PIN    GPIO_NUM_46

#define POWER_ON  0
#define POWER_OFF 1

// Tiempo de estabilización de la línea
#define POWER_DELAY_MS 200

// Inicializa pines
void power_init(void)
{
    // Liberar los holds de los pines de alimentación
    gpio_hold_dis(POWER_SD_RTC_PIN);
    gpio_hold_dis(POWER_I2S_PIN);

    // Deshabilitar el hold global de deep sleep
    gpio_deep_sleep_hold_dis();

    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);
}

// Enciende la alimentación SD+RTC
void power_sd_rtc_on(void)
{
    gpio_hold_dis(POWER_SD_RTC_PIN);

    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_ON);

    vTaskDelay(pdMS_TO_TICKS(POWER_DELAY_MS));
    //ESP_LOGI(TAG, "SD+RTC pin nivel: %d", gpio_get_level(POWER_SD_RTC_PIN));
    //ESP_LOGI(TAG, "SD+RTC power ON");
}

// Apaga la alimentación SD+RTC
void power_sd_rtc_off(void)
{
    gpio_hold_dis(POWER_SD_RTC_PIN);

    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);

    //ESP_LOGI(TAG, "SD+RTC pin nivel: %d", gpio_get_level(POWER_SD_RTC_PIN));
    //ESP_LOGI(TAG, "SD+RTC power OFF");
}

// Deja la línea SD+RTC en HIGH durante deep sleep
void power_sd_rtc_hold(void)
{
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);
    gpio_hold_en(POWER_SD_RTC_PIN);
}

// Enciende la alimentación del I2S
void power_i2s_on(void)
{
    gpio_hold_dis(POWER_I2S_PIN);

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_ON);

    vTaskDelay(pdMS_TO_TICKS(POWER_DELAY_MS));
    //ESP_LOGI(TAG, "I2S pin nivel: %d", gpio_get_level(POWER_I2S_PIN));
    //ESP_LOGI(TAG, "I2S power ON");
}

// Apaga la alimentación del I2S
void power_i2s_off(void)
{
    gpio_hold_dis(POWER_I2S_PIN);

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);

    //ESP_LOGI(TAG, "I2S pin nivel: %d", gpio_get_level(POWER_I2S_PIN));
    //ESP_LOGI(TAG, "I2S power OFF");
}

// Deja la línea I2S en HIGH durante deep sleep
void power_i2s_hold(void)
{
    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);
    gpio_hold_en(POWER_I2S_PIN);
}

void power_deep_sleep(int minutos)
{
    //ESP_LOGI(TAG, "Deep sleep por %d minutos", minutos);

    sd_deinit();
    i2s_deinit();
    rtc_i2c_deinit();

    // Mantener ambos pines en OFF durante deep sleep
    power_sd_rtc_hold();
    power_i2s_hold();

    // Habilitar los holds durante deep sleep
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup(minutos * 60 * 1000000ULL);

    esp_deep_sleep_start();
}