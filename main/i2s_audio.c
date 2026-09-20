// I2S en modo maestro, entrada stereo 16-bit @ 44.1 kHz

// ESP-IDF
#include "driver/i2s_std.h"
#include "driver/gpio.h"
//#include "esp_log.h"

// Proyecto
#include "i2s_audio.h"

static const char *TAG = "I2S_AUDIO";
static i2s_chan_handle_t rx_chan = NULL;

// Pines I2S
#define I2S_MCK  GPIO_NUM_18
#define I2S_BCK  GPIO_NUM_17
#define I2S_WS   GPIO_NUM_16
#define I2S_DIN  GPIO_NUM_15

// Configuración del canal
#define SAMPLE_RATE 44100
#define I2S_DMA_DESC_NUM  8
#define I2S_DMA_FRAME_NUM 256

// Buffer temporal para lectura stereo
#define STEREO_BUFFER_SIZE 4096
static int16_t stereo_buffer[STEREO_BUFFER_SIZE];

// Configura el canal I2S en modo maestro, stereo 16-bit
void i2s_init(void)
{
    //ESP_LOGI(TAG, "Inicializando I2S...");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_384,  // 256 genera ruido
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    //ESP_LOGI(TAG, "I2S inicializado");
}

// Libera el canal I2S y deja los pines en alta impedancia
void i2s_deinit(void)
{
    //ESP_LOGI(TAG, "Desinicializando I2S...");

    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    gpio_reset_pin(I2S_MCK);
    gpio_reset_pin(I2S_BCK);
    gpio_reset_pin(I2S_WS);
    gpio_reset_pin(I2S_DIN);

    gpio_set_direction(I2S_MCK, GPIO_MODE_INPUT);
    gpio_set_direction(I2S_BCK, GPIO_MODE_INPUT);
    gpio_set_direction(I2S_WS,  GPIO_MODE_INPUT);
    gpio_set_direction(I2S_DIN, GPIO_MODE_INPUT);

    gpio_set_pull_mode(I2S_MCK, GPIO_FLOATING);
    gpio_set_pull_mode(I2S_BCK, GPIO_FLOATING);
    gpio_set_pull_mode(I2S_WS,  GPIO_FLOATING);
    gpio_set_pull_mode(I2S_DIN, GPIO_FLOATING);

    //ESP_LOGI(TAG, "I2S desinicializado, pines en alta impedancia");
}

// Lee muestras del canal I2S y las convierte de stereo a mono (canal izquierdo)
size_t i2s_read_samples(int16_t *buffer, size_t samples_to_read, TickType_t timeout)
{
    if (!rx_chan || !buffer || samples_to_read == 0) {
        return 0;
    }

    size_t bytes_to_read = samples_to_read * sizeof(int16_t) * 2;  // stereo
    size_t max_bytes = sizeof(stereo_buffer);

    // Limitar al tamaño del buffer interno
    if (bytes_to_read > max_bytes) {
        bytes_to_read = max_bytes;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_chan, stereo_buffer, bytes_to_read, &bytes_read, timeout);

    if (err != ESP_OK || bytes_read == 0) {
        return 0;
    }

    // Convertir stereo a mono usando el canal izquierdo
    size_t samples_read = bytes_read / sizeof(int16_t);

    for (size_t i = 0; i < samples_read / 2; i++) {
        buffer[i] = stereo_buffer[i * 2];
    }

    return samples_read / 2;
}