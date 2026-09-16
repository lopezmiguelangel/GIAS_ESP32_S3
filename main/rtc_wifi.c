// Sincronización de hora: WiFi + SNTP + RTC DS3231

// C estándar
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_system.h"

// Proyecto
#include "rtc_wifi.h"

static const char *TAG = "RTC";

// I2C
#define I2C_PORT    I2C_NUM_0
#define DS3231_ADDR 0x68

// Reintentos
#define WIFI_RETRIES 2
#define RTC_RETRIES  10
#define RTC_DELAY_MS 1000

// Convierte BCD <-> decimal (formato del DS3231)
static uint8_t bcd2dec(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static i2c_master_bus_handle_t g_bus = NULL;
static i2c_master_dev_handle_t g_dev = NULL;

// Lee fecha y hora del DS3231
static bool ds3231_get_time(i2c_master_dev_handle_t dev, struct tm *t)
{
    uint8_t reg = 0x00;
    uint8_t data[7];

    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, data, 7, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC READ ERROR: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "RTC RECIBIDO: %02X %02X %02X %02X %02X %02X %02X",
             data[0], data[1], data[2], data[3],
             data[4], data[5], data[6]);

    t->tm_sec  = bcd2dec(data[0]);
    t->tm_min  = bcd2dec(data[1]);
    t->tm_hour = bcd2dec(data[2]);
    t->tm_wday = bcd2dec(data[3]) - 1;
    t->tm_mday = bcd2dec(data[4]);
    t->tm_mon  = bcd2dec(data[5]) - 1;
    t->tm_year = bcd2dec(data[6]) + 100;

    return true;
}

// Escribe fecha y hora en el DS3231 y verifica la escritura
static bool ds3231_set_time(i2c_master_dev_handle_t dev, struct tm *t)
{
    uint8_t data[8] = {
        0x00,
        dec2bcd(t->tm_sec),
        dec2bcd(t->tm_min),
        dec2bcd(t->tm_hour),
        dec2bcd(t->tm_wday + 1),
        dec2bcd(t->tm_mday),
        dec2bcd(t->tm_mon + 1),
        dec2bcd(t->tm_year - 100)
    };

    ESP_LOGI(TAG, "RTC ENVIADO: %02X %02X %02X %02X %02X %02X %02X %02X",
             data[0], data[1], data[2], data[3],
             data[4], data[5], data[6], data[7]);

    esp_err_t err;

    // Escritura con reintentos
    for (int intento = 1; intento <= RTC_RETRIES; intento++) {

        err = i2c_master_transmit(dev, data, 8, pdMS_TO_TICKS(100));

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "RTC WRITE OK (intento %d)", intento);
            break;
        }

        ESP_LOGW(TAG, "RTC WRITE ERROR (intento %d): %s",
                 intento, esp_err_to_name(err));

        vTaskDelay(pdMS_TO_TICKS(RTC_DELAY_MS));
    }

    if (err != ESP_OK)
        return false;

    vTaskDelay(pdMS_TO_TICKS(500));

    // Verificación con reintentos
    uint8_t reg = 0x00;
    uint8_t received[7];

    for (int intento = 1; intento <= RTC_RETRIES; intento++) {

        err = i2c_master_transmit_receive(
            dev, &reg, 1, received, 7, pdMS_TO_TICKS(100)
        );

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "RTC READ OK (intento %d)", intento);
            break;
        }

        ESP_LOGW(TAG, "RTC READ ERROR (intento %d): %s",
                 intento, esp_err_to_name(err));

        vTaskDelay(pdMS_TO_TICKS(RTC_DELAY_MS));
    }

    if (err != ESP_OK)
        return false;

    ESP_LOGI(TAG, "RTC RECIBIDO: %02X %02X %02X %02X %02X %02X %02X",
             received[0], received[1], received[2],
             received[3], received[4], received[5],
             received[6]);

    ESP_LOGI(TAG, "RTC VERIFICADO: %02d/%02d/%04d %02d:%02d:%02d",
             bcd2dec(received[4]),
             bcd2dec(received[5]),
             bcd2dec(received[6]) + 2000,
             bcd2dec(received[2]),
             bcd2dec(received[1]),
             bcd2dec(received[0]));

    return true;
}

// Inicializa el bus I2C y agrega el DS3231
static bool i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = RTC_I2C_SDA_PIN,
        .scl_io_num = RTC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &g_bus);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C BUS INIT ERROR: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "I2C BUS OK");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = 10000,
    };

    err = i2c_master_bus_add_device(g_bus, &dev_config, &g_dev);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C DEVICE ERROR: %s", esp_err_to_name(err));
        i2c_del_master_bus(g_bus);
        g_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "I2C DEVICE OK: direccion 0x%02X", DS3231_ADDR);

    err = i2c_master_probe(g_bus, DS3231_ADDR, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC PROBE ERROR: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(g_dev);
        i2c_del_master_bus(g_bus);
        g_dev = NULL;
        g_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "RTC DETECTADO en 0x%02X", DS3231_ADDR);

    return true;
}

// Libera el bus I2C y deja los pines en alta impedancia
void rtc_i2c_deinit(void)
{
    if (g_dev) {
        i2c_master_bus_rm_device(g_dev);
        g_dev = NULL;
    }
    if (g_bus) {
        i2c_del_master_bus(g_bus);
        g_bus = NULL;
    }

    gpio_reset_pin(RTC_I2C_SDA_PIN);
    gpio_reset_pin(RTC_I2C_SCL_PIN);

    gpio_set_direction(RTC_I2C_SDA_PIN, GPIO_MODE_INPUT);
    gpio_set_direction(RTC_I2C_SCL_PIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(RTC_I2C_SDA_PIN, GPIO_FLOATING);
    gpio_set_pull_mode(RTC_I2C_SCL_PIN, GPIO_FLOATING);
}

// Conecta a WiFi y espera asociación al AP
static bool wifi_connect(const char *ssid, const char *pass)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    wifi_config_t wc = {0};

    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strncpy((char*)wc.sta.password, pass, sizeof(wc.sta.password));

    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();

    for (int i = 0; i < 8; i++) {
        wifi_ap_record_t ap;

        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            return true;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return false;
}

// Libera WiFi, netif y NVS
static void wifi_deinit(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();

    esp_netif_destroy_default_wifi(
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")
    );

    esp_event_loop_delete_default();
    esp_netif_deinit();
    nvs_flash_deinit();
}

// Consulta hora por SNTP y aplica el offset GMT
static bool sntp_get_time(int gmt_offset_seconds, struct tm *t)
{
    char tz[32];

    snprintf(tz, sizeof(tz), "GMT%+d", -gmt_offset_seconds / 3600);

    setenv("TZ", tz, 1);
    tzset();

    esp_sntp_config_t config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    config.servers[1] = "time.google.com";
    config.servers[2] = "time.cloudflare.com";
    config.servers[3] = "time.nist.gov";
    config.num_of_servers = 4;

    esp_netif_sntp_init(&config);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK) {
        esp_netif_sntp_deinit();
        return false;
    }

    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);

    if (!timeinfo) {
        esp_netif_sntp_deinit();
        return false;
    }

    *t = *timeinfo;

    esp_netif_sntp_deinit();

    return true;
}

// Punto de entrada: obtiene hora (SNTP + RTC), decide cuál usar y actualiza el reloj interno
struct tm rtc_wifi_sync(const char *ssid, const char *password, int gmt)
{
    struct tm rtc_time = {0}, net_time = {0}, result = {0};

    bool sntp_ok = false;
    bool rtc_ok = false;

    // Obtener hora de internet
    for (int i = 0; i < WIFI_RETRIES; i++) {
        if (wifi_connect(ssid, password)) {
            if (sntp_get_time(gmt * 3600, &net_time)) {
                sntp_ok = true;
                break;
            }
        }

        wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Leer RTC
    if (i2c_init()) {

        for (int intento = 1; intento <= RTC_RETRIES; intento++) {

            ESP_LOGI(TAG, "RTC READ: intento %d", intento);

            if (ds3231_get_time(g_dev, &rtc_time)) {
                ESP_LOGI(TAG, "RTC READ OK (intento %d)", intento);
                rtc_ok = true;
                break;
            }

            ESP_LOGW(TAG, "RTC READ ERROR (intento %d)", intento);

            if (intento < RTC_RETRIES)
                vTaskDelay(pdMS_TO_TICKS(RTC_DELAY_MS));
        }

        if (!rtc_ok)
            ESP_LOGE(TAG, "RTC READ: fallo en los intentos");
    }

    // Decidir qué hora usar
    if (sntp_ok && rtc_ok) {

        bool fecha_ok =
            (rtc_time.tm_year == net_time.tm_year &&
             rtc_time.tm_mon  == net_time.tm_mon &&
             rtc_time.tm_mday == net_time.tm_mday);

        int diff_hora =
            abs((rtc_time.tm_hour * 3600 +
                 rtc_time.tm_min * 60 +
                 rtc_time.tm_sec) -
                (net_time.tm_hour * 3600 +
                 net_time.tm_min * 60 +
                 net_time.tm_sec));

        // Si difiere más de 60s o cambia la fecha, actualizar RTC
        if (!fecha_ok || diff_hora > 60) {

            if (ds3231_set_time(g_dev, &net_time)) {
                ESP_LOGI(TAG, "RTC actualizado y verificado");
                result = net_time;
            } else {
                ESP_LOGE(TAG, "NO SE PUDO ACTUALIZAR EL RTC");
                result = rtc_time;
            }

        } else {
            result = rtc_time;
        }

    } else if (rtc_ok) {

        ESP_LOGW(TAG, "Usando solo RTC");
        result = rtc_time;

    } else if (sntp_ok) {

        if (ds3231_set_time(g_dev, &net_time)) {
            ESP_LOGI(TAG, "RTC configurado con SNTP y verificado");
            result = net_time;
        } else {
            ESP_LOGE(TAG, "NO SE PUDO CONFIGURAR EL RTC CON SNTP");
            result = net_time;
        }

    } else {

        // Sin hora utilizable: reiniciar
        ESP_LOGE(TAG, "No hay hora, reiniciando...");

        rtc_i2c_deinit();

        wifi_deinit();
        esp_restart();
    }

    rtc_i2c_deinit();

    wifi_deinit();

    // Actualizar reloj interno del ESP32
    time_t t = mktime(&result);

    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0
    };

    settimeofday(&tv, NULL);

    return result;
}