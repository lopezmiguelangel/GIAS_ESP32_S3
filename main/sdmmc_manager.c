#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_system.h"

#include "sdmmc_manager.h"
#include "gias_system.h"
// ================= PINS =================
#define MMC_D2   GPIO_NUM_7
#define MMC_D3   GPIO_NUM_8
#define MMC_CMD  GPIO_NUM_9
#define MMC_CLK  GPIO_NUM_10
#define MMC_D0   GPIO_NUM_11
#define MMC_D1   GPIO_NUM_12

static sdmmc_card_t *card = NULL;
static bool is_initialized = false;
static const char *TAG = "SDMMC_MANAGER";
static uint32_t sd_speed_khz = 40000;

// =====================================================
// DEFAULT FILES
// =====================================================

bool sd_validate_files(void)
{
    if (!is_initialized) return false;
    
    FILE *f1 = fopen("/sdcard/config.txt", "r");
    FILE *f2 = fopen("/sdcard/calendar.csv", "r");
    
    bool config_exists = (f1 != NULL);
    bool calendar_exists = (f2 != NULL);
    
    if (f1) fclose(f1);
    if (f2) fclose(f2);
    
    // Solo retorna true si AMBOS existen
    return (config_exists && calendar_exists);
}

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

// =====================================================
// INIT SD
// =====================================================
bool sd_init(bool startup)
{
    if (is_initialized) return true;

    const uint32_t velocidades[] = {40000, 30000, 20000};

    int velocidad_inicial = 0;

    if (!startup) {
        velocidad_inicial = -1;
    }

    // =====================================================
    // MONTAJE NORMAL: usar solamente la velocidad guardada
    // =====================================================
    if (!startup) {

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = SDMMC_HOST_FLAG_4BIT;
        host.max_freq_khz = sd_speed_khz;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
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

        ESP_LOGI(TAG, "Montando SD a %lu MHz", sd_speed_khz / 1000);

        esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &card);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error montando SD a %lu MHz", sd_speed_khz / 1000);
            card = NULL;
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(50));

        is_initialized = true;
        ESP_LOGI(TAG, "SD montada a %lu MHz", sd_speed_khz / 1000);

        return true;
    }

    // =====================================================
    // INICIO: probar 40, 30 y 20 MHz
    // =====================================================

    for (int v = velocidad_inicial; v < 3; v++) {

        uint32_t velocidad = velocidades[v];

        for (int intento = 1; intento <= 3; intento++) {

            ESP_LOGI(TAG, "Intento %d/3 montando SD a %lu MHz",
                     intento, velocidad / 1000);

            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.flags = SDMMC_HOST_FLAG_4BIT;
            host.max_freq_khz = velocidad;

            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = 4;
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

                vTaskDelay(pdMS_TO_TICKS(50));

                is_initialized = true;
                sd_speed_khz = velocidad;

                ESP_LOGI(TAG, "SD montada correctamente a %lu MHz", velocidad / 1000);

                // =================================================
                // Si fue menor a 40 MHz, intentar recuperar 40 MHz
                // =================================================
                if (velocidad < 40000) {

                    ESP_LOGW(TAG, "SD montada a %lu MHz. Probando nuevamente a 40 MHz...",
                             velocidad / 1000);

                    sd_deinit();

                    sdmmc_host_t host40 = SDMMC_HOST_DEFAULT();
                    host40.flags = SDMMC_HOST_FLAG_4BIT;
                    host40.max_freq_khz = 40000;

                    sdmmc_slot_config_t slot40 = SDMMC_SLOT_CONFIG_DEFAULT();
                    slot40.width = 4;
                    slot40.clk = MMC_CLK;
                    slot40.cmd = MMC_CMD;
                    slot40.d0  = MMC_D0;
                    slot40.d1  = MMC_D1;
                    slot40.d2  = MMC_D2;
                    slot40.d3  = MMC_D3;
                    slot40.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

                    esp_vfs_fat_sdmmc_mount_config_t mount_cfg40 = {
                        .format_if_mount_failed = false,
                        .max_files = 4,
                        .allocation_unit_size = 16 * 1024
                    };

                    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host40, &slot40, &mount_cfg40, &card);

                    if (ret == ESP_OK) {
                        vTaskDelay(pdMS_TO_TICKS(50));

                        is_initialized = true;
                        sd_speed_khz = 40000;

                        ESP_LOGI(TAG, "SD recuperada correctamente a 40 MHz");
                    } else {
                        card = NULL;
                        sd_speed_khz = velocidad;

                        ESP_LOGW(TAG, "40 MHz volvió a fallar. Se mantiene %lu MHz",
                                 velocidad / 1000);

                        return false;
                    }
                }

                return true;
            }

            card = NULL;

            ESP_LOGW(TAG, "Falló montaje a %lu MHz (intento %d/3)",
                     velocidad / 1000, intento);

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
        }
    }

    // =====================================================
    // FALLARON 40, 30 Y 20 MHz, 3 VECES CADA UNA
    // =====================================================

    ESP_LOGE(TAG, "No se pudo montar la SD a 40, 30 ni 20 MHz");
    ESP_LOGE(TAG, "Reiniciando el equipo...");

    gias_error_handler();

    return false;
}

// =====================================================
// DEINIT SD
// =====================================================
void sd_deinit(void)
{
    if (!is_initialized) return;

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

    is_initialized = false;
    vTaskDelay(pdMS_TO_TICKS(50));
}

// =====================================================
bool is_sd_mounted(void)
{
    return is_initialized;
}

// =====================================================
// CONFIG
// =====================================================
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

// =====================================================
// CALENDAR
// =====================================================
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

    fgets(line, sizeof(line), f);

    for (int i = 0; i < 24; i++) {
        if (!fgets(line, sizeof(line), f)) break;

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
    
    // No hay cambios en toda la semana. Devolver minutos hasta medianoche.
    int minutos_hasta_medianoche = (24 - h - 1) * 60 + (60 - m);
    *min_out = minutos_hasta_medianoche;
}

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
        config_ok = true;  // Se creó, está bien
    }
    
    FILE *f2 = fopen("/sdcard/calendar.csv", "r");
    if (f2) {
        calendar_ok = true;
        fclose(f2);
    } else {
        sd_create_default_calendar();
        calendar_ok = true;  // Se creó, está bien
    }
    
    return (config_ok && calendar_ok);
}