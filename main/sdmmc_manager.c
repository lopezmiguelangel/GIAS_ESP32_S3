// Gestión de la SD: montaje, archivos de configuración y calendario

// C estándar
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

// ESP-IDF
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_system.h"

// Proyecto
#include "sdmmc_manager.h"
#include "gias_system.h"
#include "led.h"

static const char *TAG = "SDMMC_MANAGER";

// Pines SDMMC (1-bit)
#define MMC_D2   GPIO_NUM_7
#define MMC_D3   GPIO_NUM_8
#define MMC_CMD  GPIO_NUM_9
#define MMC_CLK  GPIO_NUM_10
#define MMC_D0   GPIO_NUM_11
#define MMC_D1   GPIO_NUM_12

#define FILE_READ_RETRIES 3

// Velocidad de la SD
static int sd_speed_khz = 20000;

static sdmmc_card_t *card = NULL;
static bool is_initialized = false;
static bool host_initialized = false;

// Verifica que existan config.txt y calendar.csv
bool sd_validate_files(void)
{
    if (!is_initialized) return false;
    
    FILE *f1 = fopen("/sdcard/config.txt", "r");
    FILE *f2 = fopen("/sdcard/calendar.csv", "r");
    
    bool config_exists = (f1 != NULL);
    bool calendar_exists = (f2 != NULL);
    
    if (f1) fclose(f1);
    if (f2) fclose(f2);
    
    return (config_exists && calendar_exists);
}

// Crea config.txt con valores por defecto
static void sd_create_default_config(void)
{
    FILE *f = fopen("/sdcard/config.txt", "w");
    if (!f) return;

    fprintf(f, "ssid MI_WIFI\n");
    fprintf(f, "password MI_PASSWORD\n");
    fprintf(f, "gmt -3\n");

    fclose(f);
    //ESP_LOGW(TAG, "config.txt creado");
}

// Crea calendar.csv por defecto (grabar 8-20h, resto reposo)
static void sd_create_default_calendar(void)
{
    FILE *f = fopen("/sdcard/calendar.csv", "w");
    if (!f) return;

    fprintf(f, "hora;dom;lun;mar;mie;jue;vie;sab\n");

    for (int h = 0; h < 24; h++) {
        if (h >= 8 && h < 20) {
            fprintf(f, "%02d;1;1;1;1;1;1;1\n", h);
        } else {
            fprintf(f, "%02d;0;0;0;0;0;0;0\n", h);
        }
    }

    fclose(f);
    //ESP_LOGW(TAG, "calendar.csv creado");
}

// Inicializa la SD. startup=true usa reintentos al arranque
bool sd_init(bool startup)
{
    if (is_initialized) {
        ESP_LOGI(TAG, "SD ya inicializada a %d MHz", sd_speed_khz / 1000);
        return true;
    }

    const int velocidades[] = {20000, 18000, 16000};

    for (int velocidad = 0; velocidad < 3; velocidad++) {

        int speed_khz = velocidades[velocidad];

        for (int intento = 1; intento <= 3; intento++) {

            ESP_LOGI(TAG, "Intento %d/3 montando SD a %d MHz", intento, speed_khz / 1000);

            vTaskDelay(pdMS_TO_TICKS(250));

            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.flags = SDMMC_HOST_FLAG_1BIT;
            host.max_freq_khz = speed_khz;

            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = 1;
            slot.clk = MMC_CLK;
            slot.cmd = MMC_CMD;
            slot.d0 = MMC_D0;
            slot.d1 = MMC_D1;
            slot.d2 = MMC_D2;
            slot.d3 = MMC_D3;
            slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

            esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 16 * 1024
            };

            esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);

            if (ret == ESP_OK) {
                is_initialized = true;
                host_initialized = true;
                sd_speed_khz = speed_khz;
                return true;
            }

            card = NULL;

            ESP_LOGW(TAG, "Falló montaje a %d MHz (intento %d/3)", speed_khz / 1000, intento);

            gpio_reset_pin(MMC_CLK);
            gpio_reset_pin(MMC_CMD);
            gpio_reset_pin(MMC_D0);
            gpio_reset_pin(MMC_D1);
            gpio_reset_pin(MMC_D2);
            gpio_reset_pin(MMC_D3);

            gpio_set_direction(MMC_CLK, GPIO_MODE_INPUT);
            gpio_set_direction(MMC_CMD, GPIO_MODE_INPUT);
            gpio_set_direction(MMC_D0, GPIO_MODE_INPUT);
            gpio_set_direction(MMC_D1, GPIO_MODE_INPUT);
            gpio_set_direction(MMC_D2, GPIO_MODE_INPUT);
            gpio_set_direction(MMC_D3, GPIO_MODE_INPUT);

            gpio_set_pull_mode(MMC_CLK, GPIO_FLOATING);
            gpio_set_pull_mode(MMC_CMD, GPIO_FLOATING);
            gpio_set_pull_mode(MMC_D0, GPIO_FLOATING);
            gpio_set_pull_mode(MMC_D1, GPIO_FLOATING);
            gpio_set_pull_mode(MMC_D2, GPIO_FLOATING);
            gpio_set_pull_mode(MMC_D3, GPIO_FLOATING);
        }
    }

    gias_error_handler(2);
    return false;
}

// Desmonta la SD y libera los pines
void sd_deinit(void)
{
    if (card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        card = NULL;
    }
    if (host_initialized) {
        sdmmc_host_deinit();
        host_initialized = false;
    }
    
    gpio_reset_pin(MMC_CLK);
    gpio_reset_pin(MMC_CMD);
    gpio_reset_pin(MMC_D0);
    gpio_reset_pin(MMC_D1);
    gpio_reset_pin(MMC_D2);
    gpio_reset_pin(MMC_D3);

    gpio_set_direction(MMC_CLK, GPIO_MODE_INPUT);
    gpio_set_direction(MMC_CMD, GPIO_MODE_INPUT);
    gpio_set_direction(MMC_D0, GPIO_MODE_INPUT);
    gpio_set_direction(MMC_D1, GPIO_MODE_INPUT);
    gpio_set_direction(MMC_D2, GPIO_MODE_INPUT);
    gpio_set_direction(MMC_D3, GPIO_MODE_INPUT);

    gpio_set_pull_mode(MMC_CLK, GPIO_FLOATING);
    gpio_set_pull_mode(MMC_CMD, GPIO_FLOATING);
    gpio_set_pull_mode(MMC_D0, GPIO_FLOATING);
    gpio_set_pull_mode(MMC_D1, GPIO_FLOATING);
    gpio_set_pull_mode(MMC_D2, GPIO_FLOATING);
    gpio_set_pull_mode(MMC_D3, GPIO_FLOATING);
    
    is_initialized = false;
}

// Devuelve true si la SD está montada
bool is_sd_mounted(void)
{
    return is_initialized;
}

// Lee config.txt (ssid, password, gmt)
bool sd_get_config(char *ssid, char *password, int *gmt)
{
    if (!is_initialized) return false;

    for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
        FILE *f = fopen("/sdcard/config.txt", "r");

        if (!f) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char key[32], value[64];
        bool ok1 = false, ok2 = false, ok3 = false;
        bool lectura_ok = true;

        while (1) {
            int ret = fscanf(f, "%31s %63s", key, value);

            if (ret == EOF) break;

            if (ret != 2) {
                lectura_ok = false;
                break;
            }

            if (strcmp(key, "ssid") == 0) {
                strcpy(ssid, value);
                ok1 = true;
            } else if (strcmp(key, "password") == 0) {
                strcpy(password, value);
                ok2 = true;
            } else if (strcmp(key, "gmt") == 0) {
                *gmt = atoi(value);
                ok3 = true;
            }
        }

        if (ferror(f)) lectura_ok = false;

        fclose(f);

        if (lectura_ok && ok1 && ok2 && ok3) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return false;
}

// Lee calendar.csv y decide: estado actual y minutos hasta el próximo cambio
void sd_check_calendar(struct tm *t, int *estado, int *min_out)
{
    if (!is_initialized) {
        led_blink_count(3);
        *estado = -1;
        *min_out = 0;
        return;
    }

    for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
        FILE *f = fopen("/sdcard/calendar.csv", "r");

        if (!f) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char line[256];
        int calendar[24][7] = {0};
        bool lectura_ok = true;

        if (!fgets(line, sizeof(line), f)) {
            lectura_ok = false;
            led_blink_count(5);
        }

        for (int i = 0; lectura_ok && i < 24; i++) {
            if (!fgets(line, sizeof(line), f)) {
                lectura_ok = false;
                led_blink_count(5);
                break;
            }

            int h, d0, d1, d2, d3, d4, d5, d6;

            if (sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d", &h, &d0, &d1, &d2, &d3, &d4, &d5, &d6) != 8) {
                lectura_ok = false;
                led_blink_count(7);
                break;
            }

            if (h < 0 || h >= 24) {
                lectura_ok = false;
                led_blink_count(7);
                break;
            }

            calendar[h][0] = d0;
            calendar[h][1] = d1;
            calendar[h][2] = d2;
            calendar[h][3] = d3;
            calendar[h][4] = d4;
            calendar[h][5] = d5;
            calendar[h][6] = d6;
        }

        if (ferror(f)) {
            lectura_ok = false;
            led_blink_count(5);
        }

        fclose(f);

        if (!lectura_ok) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int h = t->tm_hour;
        int m = t->tm_min;
        int d = t->tm_wday;

        int estado_actual = calendar[h][d];
        *estado = estado_actual;

        int minutos = 60 - m;
        int h2 = (h + 1) % 24;
        int d2 = (h + 1 >= 24) ? (d + 1) % 7 : d;

        for (int i = 0; i < 24 * 7; i++) {
            if (calendar[h2][d2] != estado_actual) {
                *min_out = minutos;
                return;
            }

            minutos += 60;
            h2++;

            if (h2 >= 24) {
                h2 = 0;
                d2 = (d2 + 1) % 7;
            }
        }

        *min_out = (24 - h - 1) * 60 + (60 - m);
        return;
    }

    led_blink_count(3);
    *estado = -1;
    *min_out = 0;
}

// Crea config.txt y calendar.csv si faltan
bool sd_check_and_create_files(void)
{
    bool config_ok = false;
    bool calendar_ok = false;

    for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
        FILE *f1 = fopen("/sdcard/config.txt", "r");

        if (f1) {
            config_ok = true;
            fclose(f1);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!config_ok) {
        sd_create_default_config();

        for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
            FILE *f1 = fopen("/sdcard/config.txt", "r");

            if (f1) {
                config_ok = true;
                fclose(f1);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
        FILE *f2 = fopen("/sdcard/calendar.csv", "r");

        if (f2) {
            calendar_ok = true;
            fclose(f2);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!calendar_ok) {
        sd_create_default_calendar();

        for (int intento = 1; intento <= FILE_READ_RETRIES; intento++) {
            FILE *f2 = fopen("/sdcard/calendar.csv", "r");

            if (f2) {
                calendar_ok = true;
                fclose(f2);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    return (config_ok && calendar_ok);
}