// Sistema GIAS: grabación de audio, SD/RTC, deep sleep y control de energía

// C estándar
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

// Proyecto
#include "gias_system.h"
#include "sdmmc_manager.h"
#include "rtc_wifi.h"
#include "i2s_audio.h"

static const char *TAG = "GIAS";

// LED
#define PIN_LED GPIO_NUM_48
#define LED_ON  0
#define LED_OFF 1

// Pines de alimentación
#define POWER_SD_RTC_PIN GPIO_NUM_45
#define POWER_I2S_PIN    GPIO_NUM_46
#define POWER_ON  0
#define POWER_OFF 1

// Grabación
#define SAMPLES_PER_READ    1024
#define PORCENTAJE_ADELANTO 10

// Reserva de PSRAM que no se usa (margen libre)
#define PSRAM_RESERVE_BYTES (256 * 1024)

// Bloque máximo por fwrite
#define SD_WRITE_CHUNK (10 * 1024)

// Formato WAV fijo
#define WAV_SAMPLE_RATE 44100
#define WAV_CHANNELS    1
#define WAV_BITS        16

// Reintentos de montaje de SD al arrancar
#define SD_MOUNT_RETRIES 3
#define SD_POWER_OFF_MS  100
#define SD_POWER_ON_MS   100

static TaskHandle_t led_task_handle = NULL;
static TaskHandle_t sd_task_handle  = NULL;

// Buffer compartido entre grabación y SD task
static const char *g_filename    = NULL;
static uint8_t    *g_buffer      = NULL;
static uint32_t    g_total_bytes = 0;

volatile bool sd_busy = false;

static bool write_wav_header(const char *filename, uint32_t data_size);
static bool create_wav_header(const char *filename);

// Enciende la alimentación de SD y RTC
static void power_sd_rtc_on(void)
{
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_OFF_MS));
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_SD_RTC_PIN);
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_ON_MS));
}

// Apaga la alimentación de SD y RTC (desinicializa lo que esté activo)
static void power_sd_rtc_off(void)
{
    sd_deinit();
    rtc_i2c_deinit();

    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, POWER_OFF);
}

// Enciende la alimentación del I2S
static void power_i2s_on(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(POWER_I2S_PIN);
    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_ON);
}

// Apaga el I2S y su alimentación
static void power_i2s_off(void)
{
    i2s_deinit();

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, POWER_OFF);
}

// Apaga periféricos y entra en deep sleep por N minutos
void gias_deep_sleep(int minutos)
{
    ESP_LOGI(TAG, "Deep sleep por %d minutos", minutos);

    sd_deinit();
    i2s_deinit();

    // Liberar pines I2C del RTC
    gpio_reset_pin(RTC_I2C_SDA_PIN);
    gpio_reset_pin(RTC_I2C_SCL_PIN);

    gpio_set_direction(RTC_I2C_SDA_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(RTC_I2C_SCL_PIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(RTC_I2C_SDA_PIN, GPIO_FLOATING);
    gpio_set_pull_mode(RTC_I2C_SCL_PIN, GPIO_FLOATING);

    // Mantener alimentaciones en HIGH durante deep sleep
    gpio_set_direction(POWER_SD_RTC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_SD_RTC_PIN, 1);
    gpio_hold_en(POWER_SD_RTC_PIN);

    gpio_set_direction(POWER_I2S_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_I2S_PIN, 1);
    gpio_hold_en(POWER_I2S_PIN);

    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup(minutos * 60 * 1000000ULL);

    ESP_LOGI(TAG, "POWER_SD_RTC_PIN=%d", gpio_get_level(POWER_SD_RTC_PIN));
    ESP_LOGI(TAG, "POWER_I2S_PIN=%d", gpio_get_level(POWER_I2S_PIN));

    esp_deep_sleep_start();
}

// Contadores acumulados del archivo en curso
static uint32_t buffers_escritos = 0;
static uint32_t total_bytes_final = 0;

// Tarea que escribe el buffer de PSRAM a la SD cuando se la notifica
static void sd_task(void *pvParameters)
{
    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sd_busy = true;

        uint64_t t_inicio_sd = esp_timer_get_time();

        ESP_LOGI(TAG, "SD Task: Escribiendo buffer %u", buffers_escritos + 1);

        power_sd_rtc_on();
        sd_init(false);

        if (is_sd_mounted()) {

            // Escribir audio en bloques
            FILE *f = fopen(g_filename, "ab");

            if (f) {

                size_t total_escrito = 0;
                bool error_escritura = false;

                while (total_escrito < g_total_bytes) {

                    size_t restante = g_total_bytes - total_escrito;
                    size_t bloque = restante > SD_WRITE_CHUNK ? SD_WRITE_CHUNK : restante;

                    size_t escritos = fwrite(g_buffer + total_escrito, 1, bloque, f);

                    if (escritos != bloque) {
                        ESP_LOGE(TAG, "ERROR fwrite: %u/%u bytes",
                                 (unsigned)escritos, (unsigned)bloque);
                        error_escritura = true;
                        break;
                    }

                    total_escrito += escritos;
                }

                if (!error_escritura) {
                    ESP_LOGI(TAG, "fwrite OK: %u bytes", (unsigned)total_escrito);

                    buffers_escritos++;
                    total_bytes_final += total_escrito;

                    ESP_LOGI(TAG,
                             "SD Task: Buffer %u escrito (%u bytes totales)",
                             buffers_escritos, total_bytes_final);

                    write_wav_header(g_filename, total_bytes_final);

                    ESP_LOGI(TAG,
                             "SD Task: Header WAV actualizado (%u bytes)",
                             total_bytes_final);
                }

                fclose(f);
            }
        }

        power_sd_rtc_off();

        uint64_t t_fin_sd = esp_timer_get_time();

        ESP_LOGI(TAG, "Ciclo SD: %.3f segundos",
                 (t_fin_sd - t_inicio_sd) / 1000000.0);

        sd_busy = false;
    }
}

// Crea la tarea de escritura a SD en Core 1
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

// Cabecera WAV (44 bytes estándar)
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

// Reescribe la cabecera WAV de un archivo existente con el tamaño de datos actual
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
        .NumChannels = WAV_CHANNELS,

        .SampleRate = WAV_SAMPLE_RATE,
        .ByteRate = WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS / 8,

        .BlockAlign = WAV_CHANNELS * WAV_BITS / 8,
        .BitsPerSample = WAV_BITS,

        .Subchunk2ID = {'d', 'a', 't', 'a'},
        .Subchunk2Size = data_size
    };

    header.ChunkSize = data_size + 36;

    fseek(f, 0, SEEK_SET);

    fwrite(&header, 1, sizeof(header), f);

    fclose(f);

    return true;
}

// Crea un archivo WAV vacío con cabecera inicial
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
        .NumChannels = WAV_CHANNELS,

        .SampleRate = WAV_SAMPLE_RATE,
        .ByteRate = WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS / 8,

        .BlockAlign = WAV_CHANNELS * WAV_BITS / 8,
        .BitsPerSample = WAV_BITS,

        .Subchunk2ID = {'d', 'a', 't', 'a'},
        .Subchunk2Size = 0
    };

    fwrite(&header, 1, sizeof(header), f);

    fclose(f);

    return true;
}

// Graba audio en archivos WAV, rotando a medianoche
void gias_record_start(int minutos, struct tm *current_time)
{
    if (minutos <= 0) {
        ESP_LOGW(TAG, "Minutos <= 0, no se graba nada");
        return;
    }

    struct tm tiempo_actual = *current_time;
    int minutos_restantes = minutos;

    ESP_LOGI(TAG, "Grabación solicitada: %d minutos desde %02d:%02d:%02d",
             minutos,
             tiempo_actual.tm_hour,
             tiempo_actual.tm_min,
             tiempo_actual.tm_sec);

    // Calcular tamaño del buffer PSRAM
    size_t bytes_per_read = SAMPLES_PER_READ * sizeof(int16_t);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_to_use = psram_free - PSRAM_RESERVE_BYTES;
    psram_to_use -= (psram_to_use % bytes_per_read);

    if (psram_to_use == 0) {
        ESP_LOGE(TAG, "PSRAM insuficiente");
        return;
    }

    uint32_t max_reads = psram_to_use / bytes_per_read;
    uint32_t ciclo_inicio = max_reads - (max_reads * PORCENTAJE_ADELANTO / 100);
    size_t bytes_por_ciclo = max_reads * bytes_per_read;

    ESP_LOGI(TAG,
             "Configuración PSRAM: %u bytes por ciclo, %u lecturas, notificar en %u (%.1f%%)",
             bytes_por_ciclo, max_reads, ciclo_inicio,
             100.0f - PORCENTAJE_ADELANTO);

    // Reservar buffer en PSRAM
    uint8_t *psram_buffer = heap_caps_malloc(psram_to_use, MALLOC_CAP_SPIRAM);

    if (!psram_buffer) {
        ESP_LOGE(TAG, "No se pudo reservar PSRAM");
        return;
    }

    // Crear la tarea SD si aún no existe
    if (sd_task_handle == NULL) {
        gias_create_sd_task();
    }

    // Arrancar I2S
    power_i2s_on();
    i2s_init();

    int16_t sample_buffer[SAMPLES_PER_READ];

    uint32_t total_bytes_escritos_general = 0;
    int archivo_num = 1;

    // Bucle de archivos: uno por día hasta agotar minutos
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

        // Si ya es medianoche, saltar al día siguiente
        if (minutos_este_archivo <= 0) {

            tiempo_actual.tm_hour = 0;
            tiempo_actual.tm_min = 0;
            tiempo_actual.tm_sec = 0;
            tiempo_actual.tm_mday++;

            mktime(&tiempo_actual);
            continue;
        }

        // Nombre del archivo según fecha/hora
        char filename[64];

        strftime(filename, sizeof(filename),
                 "/sdcard/%y%m%d_%H%M%S.wav",
                 &tiempo_actual);

        ESP_LOGI(TAG, "=== Archivo %d: %s ===", archivo_num, filename);

        int hora_fin = tiempo_actual.tm_hour + (minutos_este_archivo / 60);
        int min_fin  = tiempo_actual.tm_min  + (minutos_este_archivo % 60);

        if (min_fin >= 60) {
            min_fin -= 60;
            hora_fin++;
        }

        ESP_LOGI(TAG, "Grabando %d minutos (hasta %02d:%02d)",
                 minutos_este_archivo, hora_fin % 24, min_fin);

        // Crear cabecera WAV
        power_sd_rtc_on();
        sd_init(false);

        if (!create_wav_header(filename)) {
            ESP_LOGE(TAG, "Error creando header WAV para %s", filename);
            power_sd_rtc_off();
            break;
        }

        power_sd_rtc_off();

        uint32_t total_bytes_escritos_archivo = 0;
        uint32_t ciclo_actual = 0;

        uint64_t start_time = esp_timer_get_time();
        uint64_t duration_us = (uint64_t)minutos_este_archivo * 60 * 1000000ULL;

        // Ciclos de grabación dentro del archivo
        while ((esp_timer_get_time() - start_time) < duration_us) {

            ciclo_actual++;

            ESP_LOGI(TAG, "Ciclo %d del archivo %d", ciclo_actual, archivo_num);

            uint32_t reads_done = 0;
            bool esperando_escritura = false;

            uint64_t t_inicio_ciclo = esp_timer_get_time();

            // Llenar el buffer PSRAM con muestras de I2S
            while (reads_done < max_reads) {

                size_t samples = i2s_read_samples(
                    sample_buffer, SAMPLES_PER_READ, pdMS_TO_TICKS(1000)
                );

                if (samples > 0) {

                    size_t bytes_to_copy = samples * sizeof(int16_t);

                    memcpy(psram_buffer + (reads_done * bytes_per_read),
                           sample_buffer,
                           bytes_to_copy);

                    reads_done++;

                    // Al llegar al 90%, notificar a la SD task
                    if (reads_done == ciclo_inicio && !esperando_escritura) {

                        ESP_LOGI(TAG,
                                 "Ciclo %d: 90%% lleno, notificando SD task",
                                 ciclo_actual);

                        esperando_escritura = true;

                        g_filename    = filename;
                        g_buffer      = psram_buffer;
                        g_total_bytes = bytes_por_ciclo;

                        if (sd_task_handle) {
                            xTaskNotifyGive(sd_task_handle);
                        }
                    }
                }
            }

            uint64_t t_fin_grabacion = esp_timer_get_time();

            ESP_LOGI(TAG, "Ciclo grabación: %.3f segundos",
                     (t_fin_grabacion - t_inicio_ciclo) / 1000000.0);

            // Esperar a que la SD termine de escribir
            while (sd_busy) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            total_bytes_escritos_archivo += bytes_por_ciclo;
            total_bytes_escritos_general += bytes_por_ciclo;

            ESP_LOGI(TAG,
                     "Ciclo %d completado. Archivo acumula: %u bytes",
                     ciclo_actual, total_bytes_escritos_archivo);
        }

        // Actualizar cabecera final con el tamaño real
        power_sd_rtc_on();
        sd_init(false);

        if (write_wav_header(filename, total_bytes_escritos_archivo)) {
            ESP_LOGI(TAG, "Header WAV finalizado: %s (%u bytes datos)",
                     filename, total_bytes_escritos_archivo);
        } else {
            ESP_LOGE(TAG, "Error actualizando header final de %s", filename);
        }

        power_sd_rtc_off();

        // Avanzar el reloj interno del bloque
        minutos_restantes -= minutos_este_archivo;
        tiempo_actual.tm_min += minutos_este_archivo;
        mktime(&tiempo_actual);

        archivo_num++;
    }

    // Apagar I2S y liberar buffer
    power_i2s_off();

    while (sd_busy) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(psram_buffer);

    ESP_LOGI(TAG, "Grabación completada. Total: %u bytes en %d archivos",
             total_bytes_escritos_general, archivo_num - 1);
}

// Parpadeo lento del LED para indicar que el sistema está vivo
static void led_status_task(void *pvParameters)
{
    while (1) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// Detiene la tarea LED y parpadea un código antes de reiniciar
void gias_error_handler(int titileos)
{
    if (led_task_handle != NULL) {
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
    }

    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);

    // Preámbulo fijo
    for (int i = 0; i < 10; i++) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    // Código: "titileos" destellos lentos
    for (int i = 0; i < titileos; i++) {
        gpio_set_level(PIN_LED, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(2500));
        gpio_set_level(PIN_LED, LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    // Reinicio
    gpio_set_level(PIN_LED, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(10000));

    esp_restart();
}

// Inicializa pines de alimentación, LED y tarea de parpadeo
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
    vTaskDelay(pdMS_TO_TICKS(50));
}

// Consulta el estado actual: SD, config, hora y calendario
gias_status_t gias_get_status(void)
{
    gias_status_t status = {0};

    char ssid[32] = {0};
    char password[64] = {0};
    int gmt = 0;

    bool sd_ok = false;

    // Ciclo de power + montaje con reintentos
    for (int intento = 1; intento <= SD_MOUNT_RETRIES; intento++) {

        ESP_LOGI(TAG, "Intento %d/%d montando SD", intento, SD_MOUNT_RETRIES);

        power_sd_rtc_off();

        power_sd_rtc_on();

        sd_init(false);

        if (is_sd_mounted()) {
            sd_ok = true;
            break;
        }

        ESP_LOGW(TAG, "Intento %d falló", intento);
    }

    if (!sd_ok) {
        ESP_LOGE(TAG, "Error: SD no montada tras %d intentos", SD_MOUNT_RETRIES);
        power_sd_rtc_off();
        status.estado = -1;
        return status;
    }

    ESP_LOGI(TAG, "SD montada OK");

    if (!sd_check_and_create_files()) {
        ESP_LOGE(TAG, "Error: Archivos config.txt o calendar.csv no existen");
        power_sd_rtc_off();
        status.estado = -1;
        return status;
    }

    ESP_LOGI(TAG, "Archivos verificados");

    if (!sd_get_config(ssid, password, &gmt)) {
        ESP_LOGE(TAG, "Error: No se pudo leer config.txt");
        power_sd_rtc_off();
        status.estado = -1;
        return status;
    }

    ESP_LOGI(TAG, "Config leída (SSID=%s, GMT=%d)", ssid, gmt);
    ESP_LOGI(TAG, "Sincronizando RTC con WiFi...");

    status.hora = rtc_wifi_sync(ssid, password, gmt);

    ESP_LOGI(TAG, "Hora obtenida: %02d:%02d:%02d",
             status.hora.tm_hour,
             status.hora.tm_min,
             status.hora.tm_sec);

    ESP_LOGI(TAG, "Leyendo calendario...");

    sd_check_calendar(&status.hora, &status.estado, &status.minutos);

    if (status.estado == -1) {
        ESP_LOGE(TAG, "Error: No se pudo leer calendar.csv");
    } else {
        ESP_LOGI(TAG, "Calendario: estado=%d minutos=%d",
                 status.estado, status.minutos);
    }

    power_sd_rtc_off();

    ESP_LOGI(TAG, "SD+RTC apagados");

    return status;
}