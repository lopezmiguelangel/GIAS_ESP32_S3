#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "gias_system.h"
#include "sdmmc_manager.h"
#include "rtc_wifi.h"
#include "i2s_audio.h"

// ============================================================
// LED
// ============================================================

#define PIN_LED GPIO_NUM_48
#define LED_ON  0
#define LED_OFF 1

static TaskHandle_t led_task_handle = NULL;

// ============================================================
// PINES DE ALIMENTACIÓN
// ============================================================

#define POWER_SD_RTC_PIN GPIO_NUM_45
#define POWER_I2S_PIN    GPIO_NUM_46

#define POWER_ON  0
#define POWER_OFF 1

// ============================================================
// GRABACIÓN
// ============================================================

#define SAMPLES_PER_READ 1024
#define BLOCK_SD 3072
#define NUM_GRABACIONES 5
#define PORCENTAJE_ADELANTO 10

static const char *TAG = "GIAS";

// ============================================================
// VARIABLES SD
// ============================================================

static const char *g_filename = NULL;
static uint8_t *g_buffer = NULL;
static uint32_t g_total_bytes = 0;
static TaskHandle_t sd_task_handle = NULL;

static bool write_wav_header(const char *filename, uint32_t data_size);
static bool create_wav_header(const char *filename);

volatile bool sd_busy = false;

// ============================================================
// CONTROL DE ALIMENTACIÓN SD + RTC
// ============================================================

static void power_sd_rtc_on(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_SD_RTC_PIN);
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void power_sd_rtc_off(void)
{
    sd_deinit();

    // Liberar pines I2C del RTC
    gpio_reset_pin(RTC_I2C_SDA_PIN);
    gpio_reset_pin(RTC_I2C_SCL_PIN);

    gpio_set_direction(RTC_I2C_SDA_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(RTC_I2C_SCL_PIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(RTC_I2C_SDA_PIN, GPIO_FLOATING);
    gpio_set_pull_mode(RTC_I2C_SCL_PIN, GPIO_FLOATING);

    // Apagar alimentación SD + RTC
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);

    vTaskDelay(pdMS_TO_TICKS(5));
}

// ============================================================
// CONTROL DE ALIMENTACIÓN I2S
// ============================================================

static void power_i2s_on(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_I2S_PIN);
    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_ON);
}

static void power_i2s_off(void)
{
    i2s_deinit();

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);
}

// ============================================================
// DEEP SLEEP
// ============================================================

void gias_deep_sleep(int minutos)
{
    ESP_LOGI(TAG, "Deep sleep por %d minutos", minutos);

    // La SD debe estar apagada antes de entrar en sleep
    sd_deinit();

    // Apagar I2S
    i2s_deinit();

    // Liberar pines I2C del RTC
    gpio_reset_pin(RTC_I2C_SDA_PIN);
    gpio_reset_pin(RTC_I2C_SCL_PIN);

    gpio_set_direction(RTC_I2C_SDA_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(RTC_I2C_SCL_PIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(RTC_I2C_SDA_PIN, GPIO_FLOATING);
    gpio_set_pull_mode(RTC_I2C_SCL_PIN, GPIO_FLOATING);

    // GPIO45 -> HIGH durante Deep Sleep
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, 1);
    gpio_hold_en(POWER_SD_RTC_PIN);

    // GPIO46 -> HIGH durante Deep Sleep
    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, 1);
    gpio_hold_en(POWER_I2S_PIN);

    // Mantener estados durante Deep Sleep
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup(minutos * 60 * 1000000ULL);

    ESP_LOGI(TAG, "POWER_SD_RTC_PIN=%d", gpio_get_level(POWER_SD_RTC_PIN));
    ESP_LOGI(TAG, "POWER_I2S_PIN=%d", gpio_get_level(POWER_I2S_PIN));

    esp_deep_sleep_start();
}

// ============================================================
// SD TASK
// ============================================================

static uint32_t buffers_escritos = 0;
static uint32_t total_bytes_final = 0;

static void sd_task(void *pvParameters)
{
    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sd_busy = true;

        uint64_t t_inicio_sd = esp_timer_get_time();

        ESP_LOGI(TAG, "SD Task: Escribiendo buffer %u",
                 buffers_escritos + 1);

        power_sd_rtc_on();

        sd_init(false);

        if (is_sd_mounted()) {

            // ------------------------------------------------
            // Escribir audio
            // ------------------------------------------------

            FILE *f = fopen(g_filename, "ab");

            if (f) {

                fwrite(g_buffer, 1, g_total_bytes, f);

                fclose(f);

                buffers_escritos++;

                total_bytes_final += g_total_bytes;

                ESP_LOGI(TAG,
                         "SD Task: Buffer %u escrito (%u bytes totales)",
                         buffers_escritos,
                         total_bytes_final);

                write_wav_header(g_filename, total_bytes_final);

                ESP_LOGI(TAG,
                         "SD Task: Header WAV actualizado (%u bytes)",
                         total_bytes_final);
            }
        }

        sd_deinit();

        power_sd_rtc_off();

        uint64_t t_fin_sd = esp_timer_get_time();

        ESP_LOGI(TAG,
                 "Ciclo SD: %.3f segundos",
                 (t_fin_sd - t_inicio_sd) / 1000000.0);

        sd_busy = false;
    }
}

// ============================================================
// CREAR SD TASK
// ============================================================

void gias_create_sd_task(void)
{
    xTaskCreatePinnedToCore(
        sd_task,
        "sd_task",
        8192,
        NULL,
        configMAX_PRIORITIES - 2,
        &sd_task_handle,
        1
    );

    ESP_LOGI(TAG, "SD Task creada en Core 1");
}

// ============================================================
// WAV HEADER
// ============================================================

typedef struct {

    char ChunkID[4];
    uint32_t ChunkSize;

    char Format[4];

    char Subchunk1ID[4];
    uint32_t Subchunk1Size;

    uint16_t AudioFormat;
    uint16_t NumChannels;

    uint32_t SampleRate;
    uint32_t ByteRate;

    uint16_t BlockAlign;
    uint16_t BitsPerSample;

    char Subchunk2ID[4];
    uint32_t Subchunk2Size;

} wav_header_t;

// ============================================================
// ACTUALIZAR HEADER WAV
// ============================================================

static bool write_wav_header(const char *filename, uint32_t data_size)
{
    FILE *f = fopen(filename, "r+");

    if (!f) {
        return false;
    }

    wav_header_t header = {

        .ChunkID = {'R', 'I', 'F', 'F'},

        .Format = {'W', 'A', 'V', 'E'},

        .Subchunk1ID = {'f', 'm', 't', ' '},
        .Subchunk1Size = 16,

        .AudioFormat = 1,
        .NumChannels = 1,

        .SampleRate = 44100,
        .ByteRate = 44100 * 1 * 16 / 8,

        .BlockAlign = 1 * 16 / 8,
        .BitsPerSample = 16,

        .Subchunk2ID = {'d', 'a', 't', 'a'},
        .Subchunk2Size = data_size
    };

    header.ChunkSize = data_size + 36;

    fseek(f, 0, SEEK_SET);

    fwrite(&header, 1, sizeof(header), f);

    fclose(f);

    return true;
}

// ============================================================
// CREAR HEADER WAV
// ============================================================

static bool create_wav_header(const char *filename)
{
    FILE *f = fopen(filename, "w");

    if (!f) {
        return false;
    }

    wav_header_t header = {

        .ChunkID = {'R', 'I', 'F', 'F'},
        .ChunkSize = 36,

        .Format = {'W', 'A', 'V', 'E'},

        .Subchunk1ID = {'f', 'm', 't', ' '},
        .Subchunk1Size = 16,

        .AudioFormat = 1,
        .NumChannels = 1,

        .SampleRate = 44100,
        .ByteRate = 44100 * 1 * 16 / 8,

        .BlockAlign = 1 * 16 / 8,
        .BitsPerSample = 16,

        .Subchunk2ID = {'d', 'a', 't', 'a'},
        .Subchunk2Size = 0
    };

    fwrite(&header, 1, sizeof(header), f);

    fclose(f);

    return true;
}

// ============================================================
// RECORDING
// ============================================================

void gias_record_start(int minutos, struct tm *current_time)
{
    if (minutos <= 0) {

        ESP_LOGW(TAG, "Minutos <= 0, no se graba nada");

        return;
    }

    struct tm tiempo_actual = *current_time;

    int minutos_restantes = minutos;

    ESP_LOGI(TAG,
             "Grabación solicitada: %d minutos desde %02d:%02d:%02d",
             minutos,
             tiempo_actual.tm_hour,
             tiempo_actual.tm_min,
             tiempo_actual.tm_sec);

    // --------------------------------------------------------
    // Calcular PSRAM
    // --------------------------------------------------------

    size_t bytes_per_read =
        SAMPLES_PER_READ * sizeof(int16_t);

    size_t psram_free =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    size_t psram_to_use =
        psram_free - (256 * 1024);

    psram_to_use -=
        (psram_to_use % bytes_per_read);

    if (psram_to_use == 0) {

        ESP_LOGE(TAG, "PSRAM insuficiente");

        return;
    }

    uint32_t max_reads =
        psram_to_use / bytes_per_read;

    uint32_t ciclo_inicio =
        max_reads -
        (max_reads * PORCENTAJE_ADELANTO / 100);

    size_t bytes_por_ciclo =
        max_reads * bytes_per_read;

    ESP_LOGI(TAG,
             "Configuración PSRAM: %u bytes por ciclo, %u lecturas, notificar en %u (%.1f%%)",
             bytes_por_ciclo,
             max_reads,
             ciclo_inicio,
             100.0f - PORCENTAJE_ADELANTO);

    // --------------------------------------------------------
    // Reservar buffer PSRAM
    // --------------------------------------------------------

    uint8_t *psram_buffer =
        heap_caps_malloc(psram_to_use, MALLOC_CAP_SPIRAM);

    if (!psram_buffer) {

        ESP_LOGE(TAG, "No se pudo reservar PSRAM");

        return;
    }

    // --------------------------------------------------------
    // Crear SD task
    // --------------------------------------------------------

    if (sd_task_handle == NULL) {
        gias_create_sd_task();
    }

    // --------------------------------------------------------
    // Iniciar I2S
    // --------------------------------------------------------

    power_i2s_on();

    i2s_init();

    vTaskDelay(pdMS_TO_TICKS(50));

    int16_t sample_buffer[SAMPLES_PER_READ];

    uint32_t total_bytes_escritos_general = 0;

    int archivo_num = 1;

    // ========================================================
    // BUCLE DE ARCHIVOS
    // ========================================================

    while (minutos_restantes > 0) {

        int minutos_hasta_medianoche =
            (24 - tiempo_actual.tm_hour - 1) * 60 +
            (60 - tiempo_actual.tm_min);

        if (minutos_hasta_medianoche < 0) {
            minutos_hasta_medianoche = 0;
        }

        int minutos_este_archivo =
            (minutos_hasta_medianoche < minutos_restantes)
            ? minutos_hasta_medianoche
            : minutos_restantes;

        if (minutos_este_archivo <= 0) {

            tiempo_actual.tm_hour = 0;
            tiempo_actual.tm_min = 0;
            tiempo_actual.tm_sec = 0;

            tiempo_actual.tm_mday++;

            mktime(&tiempo_actual);

            continue;
        }

        // ----------------------------------------------------
        // Nombre archivo
        // ----------------------------------------------------

        char filename[64];

        ESP_LOGI(TAG,
                 "DEBUG: tm_year=%d, tm_mon=%d, tm_mday=%d, tm_hour=%d, tm_min=%d, tm_sec=%d",
                 tiempo_actual.tm_year,
                 tiempo_actual.tm_mon,
                 tiempo_actual.tm_mday,
                 tiempo_actual.tm_hour,
                 tiempo_actual.tm_min,
                 tiempo_actual.tm_sec);

        strftime(
            filename,
            sizeof(filename),
            "/sdcard/%y%m%d_%H%M%S.wav",
            &tiempo_actual
        );

        ESP_LOGI(TAG,
                 "=== Archivo %d: %s ===",
                 archivo_num,
                 filename);

        int hora_fin =
            tiempo_actual.tm_hour +
            (minutos_este_archivo / 60);

        int min_fin =
            tiempo_actual.tm_min +
            (minutos_este_archivo % 60);

        if (min_fin >= 60) {

            min_fin -= 60;
            hora_fin++;
        }

        ESP_LOGI(TAG,
                 "Grabando %d minutos (hasta %02d:%02d)",
                 minutos_este_archivo,
                 hora_fin % 24,
                 min_fin);

        // ----------------------------------------------------
        // Crear header WAV
        // ----------------------------------------------------

        power_sd_rtc_on();

        sd_init(false);

        if (!create_wav_header(filename)) {

            ESP_LOGE(TAG,
                     "Error creando header WAV para %s",
                     filename);

            sd_deinit();

            power_sd_rtc_off();

            break;
        }

        sd_deinit();

        power_sd_rtc_off();

        // ----------------------------------------------------
        // Grabar archivo
        // ----------------------------------------------------

        uint32_t total_bytes_escritos_archivo = 0;

        uint32_t ciclo_actual = 0;

        uint64_t start_time =
            esp_timer_get_time();

        uint64_t duration_us =
            (uint64_t)minutos_este_archivo *
            60 *
            1000000ULL;

        // ====================================================
        // CICLOS
        // ====================================================

        while ((esp_timer_get_time() - start_time) < duration_us) {

            ciclo_actual++;

            ESP_LOGI(TAG,
                     "Ciclo %d del archivo %d",
                     ciclo_actual,
                     archivo_num);

            uint32_t reads_done = 0;

            bool esperando_escritura = false;

            uint64_t t_inicio_ciclo =
                esp_timer_get_time();

            while (reads_done < max_reads) {

                size_t samples =
                    i2s_read_samples(
                        sample_buffer,
                        SAMPLES_PER_READ,
                        pdMS_TO_TICKS(1000)
                    );

                if (samples > 0) {

                    size_t bytes_to_copy =
                        samples * sizeof(int16_t);

                    memcpy(
                        psram_buffer +
                        (reads_done * bytes_per_read),

                        sample_buffer,

                        bytes_to_copy
                    );

                    reads_done++;

                    // ----------------------------------------
                    // Notificar al 90%
                    // ----------------------------------------

                    if (reads_done == ciclo_inicio &&
                        !esperando_escritura) {

                        ESP_LOGI(TAG,
                                 "Ciclo %d: 90%% lleno, notificando SD task",
                                 ciclo_actual);

                        esperando_escritura = true;

                        g_filename = filename;
                        g_buffer = psram_buffer;
                        g_total_bytes = bytes_por_ciclo;

                        if (sd_task_handle) {
                            xTaskNotifyGive(sd_task_handle);
                        }
                    }
                }
            }

            uint64_t t_fin_grabacion =
                esp_timer_get_time();

            ESP_LOGI(TAG,
                     "Ciclo grabación: %.3f segundos",
                     (t_fin_grabacion - t_inicio_ciclo) /
                     1000000.0);

            // -----------------------------------------------
            // Esperar SD
            // -----------------------------------------------

            while (sd_busy) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            total_bytes_escritos_archivo +=
                bytes_por_ciclo;

            total_bytes_escritos_general +=
                bytes_por_ciclo;

            ESP_LOGI(TAG,
                     "Ciclo %d completado. Archivo acumula: %u bytes",
                     ciclo_actual,
                     total_bytes_escritos_archivo);
        }

        // ====================================================
        // HEADER FINAL
        // ====================================================

        power_sd_rtc_on();

        sd_init(false);

        if (write_wav_header(
                filename,
                total_bytes_escritos_archivo)) {

            ESP_LOGI(TAG,
                     "Header WAV finalizado: %s (%u bytes datos)",
                     filename,
                     total_bytes_escritos_archivo);

        } else {

            ESP_LOGE(TAG,
                     "Error actualizando header final de %s",
                     filename);
        }

        sd_deinit();

        power_sd_rtc_off();

        // ====================================================
        // ACTUALIZAR TIEMPO
        // ====================================================

        minutos_restantes -=
            minutos_este_archivo;

        tiempo_actual.tm_min +=
            minutos_este_archivo;

        mktime(&tiempo_actual);

        archivo_num++;
    }

    // ========================================================
    // FINALIZAR I2S
    // ========================================================

    i2s_deinit();

    power_i2s_off();

    // Esperar última escritura
    while (sd_busy) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(psram_buffer);

    ESP_LOGI(TAG,
             "Grabación completada. Total: %u bytes en %d archivos",
             total_bytes_escritos_general,
             archivo_num - 1);
}

// ============================================================
// LED STATUS TASK
// ============================================================

static void led_status_task(void *pvParameters)
{
    while (1) {

        gpio_set_level(PIN_LED, LED_ON);

        vTaskDelay(pdMS_TO_TICKS(100));

        gpio_set_level(PIN_LED, LED_OFF);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ============================================================
// GIAS INIT
// ============================================================
void gias_error_handler(void)
{
    if (led_task_handle != NULL) {
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
    }

    gpio_set_level(PIN_LED, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_restart();
}

void gias_init(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_SD_RTC_PIN);
    gpio_hold_dis(POWER_I2S_PIN);

    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);

    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, LED_OFF);

    xTaskCreate(led_status_task, "led_status", 2048, NULL, 1, &led_task_handle);
}

// ============================================================
// GIAS STATUS
// ============================================================

gias_status_t gias_get_status(void)
{
    gias_status_t status = {0};

    char ssid[32] = {0};
    char password[64] = {0};

    int gmt = 0;

    ESP_LOGI(TAG, "1. Encendiendo SD+RTC");

    power_sd_rtc_on();

    sd_init(true);

    if (!is_sd_mounted()) {

        ESP_LOGE(TAG, "Error: SD no montada");

        sd_deinit();

        power_sd_rtc_off();

        status.estado = -1;

        return status;
    }

    ESP_LOGI(TAG, "2. SD montada OK");

    if (!sd_check_and_create_files()) {

        ESP_LOGE(TAG,
                 "Error: Archivos config.txt o calendar.csv no existen");

        sd_deinit();

        power_sd_rtc_off();

        status.estado = -1;

        return status;
    }

    ESP_LOGI(TAG, "3. Archivos verificados");

    if (!sd_get_config(ssid, password, &gmt)) {

        ESP_LOGE(TAG,
                 "Error: No se pudo leer config.txt");

        sd_deinit();

        power_sd_rtc_off();

        status.estado = -1;

        return status;
    }

    ESP_LOGI(TAG,
             "4. Config leída (SSID=%s, GMT=%d)",
             ssid,
             gmt);

    ESP_LOGI(TAG,
             "5. Sincronizando RTC con WiFi...");

    status.hora =
        rtc_wifi_sync(
            ssid,
            password,
            gmt
        );

    ESP_LOGI(TAG,
             "6. Hora obtenida: %02d:%02d:%02d",
             status.hora.tm_hour,
             status.hora.tm_min,
             status.hora.tm_sec);

    ESP_LOGI(TAG,
             "7. Leyendo calendario...");

    sd_check_calendar(
        &status.hora,
        &status.estado,
        &status.minutos
    );

    if (status.estado == -1) {

        ESP_LOGE(TAG,
                 "Error: No se pudo leer calendar.csv");

    } else {

        ESP_LOGI(TAG,
                 "8. Calendario: estado=%d minutos=%d",
                 status.estado,
                 status.minutos);
    }


    sd_deinit();

    power_sd_rtc_off();

    ESP_LOGI(TAG, "9. SD+RTC apagados");

    return status;
}
