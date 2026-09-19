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

// Velocidad de la SD
static int sd_speed_khz = 20000;

static sdmmc_card_t *card = NULL;
static bool is_initialized = false;

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
    ESP_LOGW(TAG, "config.txt creado");
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
    ESP_LOGW(TAG, "calendar.csv creado");
}

// Inicializa la SD. startup=true usa reintentos al arranque
bool sd_init(bool startup)
{
    if (is_initialized) {
        ESP_LOGI(TAG, "SD ya inicializada a %d MHz", sd_speed_khz / 1000);
        return true;
    }

    const int velocidades[] = {25000, 20000};

    for (int velocidad = 0; velocidad < 2; velocidad++) {

        int speed_khz = velocidades[velocidad];

        for (int intento = 1; intento <= 3; intento++) {

            ESP_LOGI(TAG, "Intento %d/3 montando SD a %d MHz", intento, speed_khz / 1000);

            vTaskDelay(pdMS_TO_TICKS(100));

            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.flags = SDMMC_HOST_FLAG_1BIT;
            host.max_freq_khz = speed_khz;

            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = 1;
            slot.clk = MMC_CLK;
            slot.cmd = MMC_CMD;
            slot.d0  = MMC_D0;
            slot.d1  = MMC_D1;
            slot.d2  = MMC_D2;
            slot.d3  = MMC_D3;
            slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

            esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 16 * 1024
            };

            esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);

            if (ret == ESP_OK) {

                is_initialized = true;
                sd_speed_khz = speed_khz;

                ESP_LOGI(TAG, "SD montada correctamente a %d MHz", sd_speed_khz / 1000);

                return true;
            }

            card = NULL;

            ESP_LOGW(TAG, "Falló montaje a %d MHz (intento %d/3)", speed_khz / 1000, intento);

            sdmmc_host_deinit();

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

    ESP_LOGE(TAG, "No se pudo montar la SD a 25 ni 20 MHz");

    gias_error_handler(6);

    return false;
}

// Desmonta la SD y libera los pines
void sd_deinit(void)
{
    ESP_LOGI(TAG, "SD DEINIT: inicio");

    if (!is_initialized) {
        ESP_LOGI(TAG, "SD DEINIT: no estaba inicializada");
        return;
    }

    if (card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
        card = NULL;
    }

    sdmmc_host_deinit();

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

    vTaskDelay(pdMS_TO_TICKS(50));
    
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

    FILE *f = fopen("/sdcard/config.txt", "r");

    if (!f) {
        ESP_LOGW(TAG, "config.txt no existe");
        sd_create_default_config();
        f = fopen("/sdcard/config.txt", "r");
        if (!f) return false;
    }

    char key[32], value[64];
    bool ok1 = false, ok2 = false, ok3 = false;

    while (fscanf(f, "%31s %63s", key, value) == 2) {
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

    fclose(f);
    return (ok1 && ok2 && ok3);
}

// Lee calendar.csv y decide: estado actual y minutos hasta el próximo cambio
void sd_check_calendar(struct tm *t, int *estado, int *min_out)
{
    if (!is_initialized) {
        *estado = -1;
        *min_out = 0;
        return;
    }

    FILE *f = fopen("/sdcard/calendar.csv", "r");
    if (!f) {
        *estado = -1;
        *min_out = 0;
        return;
    }

    char line[256];
    int calendar[24][7] = {0};

    // Descartar encabezado
    fgets(line, sizeof(line), f);

    // Cargar 24 filas (una por hora)
    for (int i = 0; i < 24; i++) {
        if (!fgets(line, sizeof(line), f)) break;

        // Normalizar separadores a ';'
        for (char *p = line; *p; p++) {
            if (*p == ',' || *p == '\t' || *p == '|') *p = ';';
        }

        int h, d0, d1, d2, d3, d4, d5, d6;
        if (sscanf(line, "%d;%d;%d;%d;%d;%d;%d;%d", 
                   &h, &d0, &d1, &d2, &d3, &d4, &d5, &d6) == 8) {
            if (h >= 0 && h < 24) {
                calendar[h][0] = d0;
                calendar[h][1] = d1;
                calendar[h][2] = d2;
                calendar[h][3] = d3;
                calendar[h][4] = d4;
                calendar[h][5] = d5;
                calendar[h][6] = d6;
            }
        }
    }
    fclose(f);

    int h = t->tm_hour;
    int m = t->tm_min;
    int d = t->tm_wday;

    int estado_actual = calendar[h][d];
    *estado = estado_actual;

    // Buscar próximo cambio
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
    
    // No hay cambios en toda la semana: usar minutos hasta medianoche
    int minutos_hasta_medianoche = (24 - h - 1) * 60 + (60 - m);
    *min_out = minutos_hasta_medianoche;
}

// Crea config.txt y calendar.csv si faltan
bool sd_check_and_create_files(void)
{
    bool config_ok = false;
    bool calendar_ok = false;
    
    FILE *f1 = fopen("/sdcard/config.txt", "r");
    if (f1) {
        config_ok = true;
        fclose(f1);
    } else {
        sd_create_default_config();
        config_ok = true;
    }
    
    FILE *f2 = fopen("/sdcard/calendar.csv", "r");
    if (f2) {
        calendar_ok = true;
        fclose(f2);
    } else {
        sd_create_default_calendar();
        calendar_ok = true;
    }
    
    return (config_ok && calendar_ok);
}