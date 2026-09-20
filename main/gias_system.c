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
#include "led.h"
#include "power.h"

static const char *TAG = "GIAS";

// Configuración de grabación
#define SAMPLES_PER_READ    1024
#define PORCENTAJE_ADELANTO 5

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

// Tipos
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

// Contexto de grabación: buffer PSRAM y sus parámetros de ciclado
typedef struct {
    uint8_t *buffer;
    size_t   buffer_size;
    uint32_t max_reads;
    uint32_t ciclo_inicio;
    size_t   bytes_por_ciclo;
} record_ctx_t;

// Estado compartido entre grabación y SD task
static TaskHandle_t sd_task_handle = NULL;

static const char *g_filename    = NULL;
static uint8_t    *g_buffer      = NULL;
static uint32_t    g_total_bytes = 0;

volatile bool sd_busy = false;

// Contadores acumulados del archivo en curso
static uint32_t buffers_escritos  = 0;
static uint32_t total_bytes_final = 0;

// Prototipo interno.
void gias_create_sd_task(void);

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

// Prepara todo lo necesario para grabar: buffer PSRAM, task SD e I2S.
// Devuelve true si quedó todo listo.
static bool gias_record_setup(record_ctx_t *ctx)
{
    size_t bytes_per_read = SAMPLES_PER_READ * sizeof(int16_t);

    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_to_use = psram_free - PSRAM_RESERVE_BYTES;
    psram_to_use -= (psram_to_use % bytes_per_read);

    if (psram_to_use == 0) {
        ////ESP_LOGE(TAG, "PSRAM insuficiente");
        return false;
    }

    ctx->max_reads     = psram_to_use / bytes_per_read;
    ctx->ciclo_inicio  = ctx->max_reads - (ctx->max_reads * PORCENTAJE_ADELANTO / 100);
    ctx->bytes_por_ciclo = ctx->max_reads * bytes_per_read;
    ctx->buffer_size   = psram_to_use;

    /*ESP_LOGI(TAG,
             "Configuración PSRAM: %u bytes por ciclo, %u lecturas, notificar en %u (%.1f%%)",
             ctx->bytes_por_ciclo, ctx->max_reads, ctx->ciclo_inicio,
             100.0f - PORCENTAJE_ADELANTO);*/

    ctx->buffer = heap_caps_malloc(psram_to_use, MALLOC_CAP_SPIRAM);

    if (!ctx->buffer) {
        //ESP_LOGE(TAG, "No se pudo reservar PSRAM");
        return false;
    }

    if (sd_task_handle == NULL) {
        gias_create_sd_task();
    }

    power_i2s_on();
    i2s_init();

    return true;
}

// Tarea que escribe el buffer de PSRAM a la SD cuando se la notifica
static void sd_task(void *pvParameters)
{
    while (1) {

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        sd_busy = true;

        uint64_t t_inicio_sd = esp_timer_get_time();

        //ESP_LOGI(TAG, "SD Task: Escribiendo buffer %u", buffers_escritos + 1);

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
                        //ESP_LOGE(TAG, "ERROR fwrite: %u/%u bytes", (unsigned)escritos, (unsigned)bloque);
                        error_escritura = true;
                        break;
                    }

                    total_escrito += escritos;
                }

                if (!error_escritura) {
                    //ESP_LOGI(TAG, "fwrite OK: %u bytes", (unsigned)total_escrito);

                    buffers_escritos++;
                    total_bytes_final += total_escrito;

                    /*ESP_LOGI(TAG,
                             "SD Task: Buffer %u escrito (%u bytes totales)",
                             buffers_escritos, total_bytes_final);*/

                    write_wav_header(g_filename, total_bytes_final);

                    /*ESP_LOGI(TAG,
                             "SD Task: Header WAV actualizado (%u bytes)",
                             total_bytes_final);*/
                }

                fclose(f);
            }
        }

        sd_deinit();

        power_sd_rtc_off();

        uint64_t t_fin_sd = esp_timer_get_time();

        //ESP_LOGI(TAG, "Ciclo SD: %.3f segundos", (t_fin_sd - t_inicio_sd) / 1000000.0);
        vTaskDelay(pdMS_TO_TICKS(100));
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

    //ESP_LOGI(TAG, "SD Task creada en Core 1");
}

// Calcula cuántos minutos corresponden a este bloque.
// Devuelve 0 si hay que saltar al día siguiente.
static int minutos_del_bloque(struct tm *t, int minutos_restantes)
{
    int minutos_hasta_medianoche =
        (24 - t->tm_hour - 1) * 60 + (60 - t->tm_min);

    if (minutos_hasta_medianoche < 0) {
        minutos_hasta_medianoche = 0;
    }

    int minutos_este_archivo =
        (minutos_hasta_medianoche < minutos_restantes)
        ? minutos_hasta_medianoche
        : minutos_restantes;

    if (minutos_este_archivo <= 0) {
        t->tm_hour = 0;
        t->tm_min  = 0;
        t->tm_sec  = 0;
        t->tm_mday++;
        mktime(t);
        return 0;
    }

    return minutos_este_archivo;
}

// Arma el nombre del archivo según fecha/hora.
static void armar_nombre_archivo(struct tm *t, char *out, size_t size)
{
    strftime(out, size, "/sdcard/%y%m%d_%H%M%S.wav", t);
}

// Loguea hasta qué hora graba este bloque.
static void log_hora_fin(struct tm *t, int minutos)
{
    int hora_fin = t->tm_hour + (minutos / 60);
    int min_fin  = t->tm_min  + (minutos % 60);

    if (min_fin >= 60) {
        min_fin -= 60;
        hora_fin++;
    }

    //ESP_LOGI(TAG, "Grabando %d minutos (hasta %02d:%02d)", minutos, hora_fin % 24, min_fin);
}

// Abre el archivo: enciende SD, monta, crea cabecera.
static bool abrir_archivo(const char *filename)
{
    power_sd_rtc_on();
    sd_init(false);

    bool ok = create_wav_header(filename);

    sd_deinit();
    power_sd_rtc_off();

    if (!ok) {
        //ESP_LOGE(TAG, "Error creando header WAV para %s", filename);
    }

    return ok;
}

// Graba los ciclos dentro de un archivo. Devuelve bytes escritos.
static uint32_t grabar_ciclos(record_ctx_t *ctx, const char *filename,
                              int minutos, int16_t *sample_buffer)
{
    uint32_t total_bytes = 0;
    uint32_t ciclo_actual = 0;

    uint64_t start_time  = esp_timer_get_time();
    uint64_t duration_us = (uint64_t)minutos * 60 * 1000000ULL;

    while ((esp_timer_get_time() - start_time) < duration_us) {

        ciclo_actual++;

        //ESP_LOGI(TAG, "Ciclo %d", ciclo_actual);

        uint32_t reads_done = 0;
        bool esperando_escritura = false;

        uint64_t t_inicio_ciclo = esp_timer_get_time();

        while (reads_done < ctx->max_reads) {

            size_t samples = i2s_read_samples(
                sample_buffer, SAMPLES_PER_READ, pdMS_TO_TICKS(1000)
            );

            if (samples > 0) {

                size_t bytes_to_copy = samples * sizeof(int16_t);

                memcpy(ctx->buffer + (reads_done * SAMPLES_PER_READ * sizeof(int16_t)),
                       sample_buffer, bytes_to_copy);

                reads_done++;

                if (reads_done == ctx->ciclo_inicio && !esperando_escritura) {

                    //ESP_LOGI(TAG, "Ciclo %d: 90%% lleno, notificando SD task", ciclo_actual);

                    esperando_escritura = true;

                    g_filename    = filename;
                    g_buffer      = ctx->buffer;
                    g_total_bytes = ctx->bytes_por_ciclo;

                    if (sd_task_handle) {
                        xTaskNotifyGive(sd_task_handle);
                    }
                }
            }
        }

        uint64_t t_fin = esp_timer_get_time();

        //ESP_LOGI(TAG, "Ciclo grabación: %.3f segundos", (t_fin - t_inicio_ciclo) / 1000000.0);

        while (sd_busy) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        total_bytes += ctx->bytes_por_ciclo;

        //ESP_LOGI(TAG, "Ciclo %d completado. Archivo acumula: %u bytes", ciclo_actual, total_bytes);
    }

    return total_bytes;
}

// Avanza el reloj interno del bloque.
static void avanzar_reloj(struct tm *t, int minutos)
{
    t->tm_min += minutos;
    mktime(t);
}

// Graba audio en archivos WAV. Corta el archivo a las 00.00hs
void gias_record_start(int minutos, struct tm *current_time)
{
    if (minutos <= 0) {
        //ESP_LOGW(TAG, "Minutos <= 0, no se graba nada");
        return;
    }

    record_ctx_t ctx = {0};

    if (!gias_record_setup(&ctx)) {
        return;
    }

    struct tm tiempo_actual = *current_time;
    int minutos_restantes = minutos;

    /*ESP_LOGI(TAG, "Grabación solicitada: %d minutos desde %02d:%02d:%02d",
             minutos,
             tiempo_actual.tm_hour,
             tiempo_actual.tm_min,
             tiempo_actual.tm_sec);*/

    int16_t sample_buffer[SAMPLES_PER_READ];

    uint32_t total_bytes_escritos_general = 0;
    int archivo_num = 1;

    while (minutos_restantes > 0) {
        int minutos_este_archivo =
            minutos_del_bloque(&tiempo_actual, minutos_restantes);

        if (minutos_este_archivo == 0) {
            continue;
        }

        char filename[64];

        armar_nombre_archivo(&tiempo_actual, filename, sizeof(filename));

        //ESP_LOGI(TAG, "=== Archivo %d: %s ===", archivo_num, filename);

        log_hora_fin(&tiempo_actual, minutos_este_archivo);

        if (!abrir_archivo(filename)) {
            break;
        }

        uint32_t bytes = grabar_ciclos(&ctx, filename,
                                    minutos_este_archivo, sample_buffer);

        total_bytes_escritos_general += bytes;
        minutos_restantes -= minutos_este_archivo;
        avanzar_reloj(&tiempo_actual, minutos_este_archivo);
        archivo_num++;
    }

    // Apagar I2S y liberar buffer
    power_i2s_off();

    while (sd_busy) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (sd_task_handle) {
        vTaskDelete(sd_task_handle);
        sd_task_handle = NULL;
    }

    free(ctx.buffer);

    //ESP_LOGI(TAG, "Grabación completada. Total: %u bytes en %d archivos", total_bytes_escritos_general, archivo_num - 1);
}

// Monta la SD con reintentos. Devuelve true si quedó montada.
static bool gias_mount_sd(void)
{
    power_sd_rtc_on();
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int intento = 1; intento <= SD_MOUNT_RETRIES; intento++) {
        sd_init(false);

        if (is_sd_mounted()) {
            return true;
        }
    }
    return false;
}

// Verifica/crea los archivos necesarios.
static bool gias_ensure_files(void)
{
    if (!sd_check_and_create_files()) {
        //ESP_LOGE(TAG, "Error: Archivos config.txt o calendar.csv no existen");
        return false;
    }

    //ESP_LOGI(TAG, "Archivos verificados");
    return true;
}

// Lee config.txt (ssid, password, gmt).
static bool gias_read_config(char *ssid, char *password, int *gmt)
{
    if (!sd_get_config(ssid, password, gmt)) {
        //ESP_LOGE(TAG, "Error: No se pudo leer config.txt");
        return false;
    }

    //ESP_LOGI(TAG, "Config leída (SSID=%s, GMT=%d)", ssid, *gmt);
    return true;
}

// Sincroniza la hora por WiFi + SNTP + RTC.
static bool gias_sync_time(const char *ssid, const char *password, int gmt, struct tm *hora)
{
    //ESP_LOGI(TAG, "Sincronizando RTC con WiFi...");

    *hora = rtc_wifi_sync(ssid, password, gmt);

    //ESP_LOGI(TAG, "Hora obtenida: %02d:%02d:%02d", hora->tm_hour, hora->tm_min, hora->tm_sec);
    return true;
}

// Lee el calendario según la hora y devuelve estado/minutos.
static bool gias_read_calendar(struct tm *hora, int *estado, int *minutos)
{

    sd_check_calendar(hora, estado, minutos);

    if (*estado == -1) {
        return false;
    }

    return true;
}

// Apaga SD+RTC.
static void gias_power_off_sd_rtc(void)
{
    sd_deinit();
    power_sd_rtc_off();

    //ESP_LOGI(TAG, "SD+RTC apagados");
}

// Consulta el estado actual: SD, config, hora y calendario
gias_status_t gias_get_status(void)
{
    gias_status_t status = {0};
    bool error = false;

    char ssid[32] = {0};
    char password[64] = {0};
    int gmt = 0;

    ESP_LOGI(TAG, "gias_get_status(): inicio");

    if (!gias_mount_sd()) {
        ESP_LOGI(TAG, "gias_mount_sd(): FALLÓ");
        gias_power_off_sd_rtc();
        status.estado = -1;
        led_blink_count(2);
        return status;
    }

    ESP_LOGI(TAG, "gias_mount_sd(): OK");
    
    if (!gias_ensure_files()) {
        led_blink_count(2);
        error = true;
    }

    if (!error && !gias_read_config(ssid, password, &gmt)) {
        led_blink_count(2);
        error = true;
    }

    if (!error && !gias_sync_time(ssid, password, gmt, &status.hora)) {
        led_blink_count(2);
        error = true;
    }

    if (!error && !gias_read_calendar(&status.hora, &status.estado, &status.minutos)) {
        led_blink_count(2);
        error = true;
    }

    if (error) {
        gias_power_off_sd_rtc();
        status.estado = -1;
        return status;
    }
    gias_power_off_sd_rtc();

    ESP_LOGI(TAG, "gias_get_status(): OK - estado=%d, minutos=%d, hora=%02d:%02d:%02d",
         status.estado, status.minutos,
         status.hora.tm_hour, status.hora.tm_min, status.hora.tm_sec);
    
    led_blink_count(1);

    return status;
}