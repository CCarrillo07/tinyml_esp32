#include "audio_i2s.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"

#define I2S_NUM I2S_NUM_0
#define FRAME_SIZE 512

/* =========================
   STATIC BUFFER
   ========================= */
static int32_t raw_buffer[FRAME_SIZE];

void audio_i2s_init(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 512,
        .use_apll = false
    };

    i2s_pin_config_t pins = {
        .bck_io_num = 26,   // SCK
        .ws_io_num = 25,    // WS
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = 33   // SD
    };

    i2s_driver_install(I2S_NUM, &config, 0, NULL);
    i2s_set_pin(I2S_NUM, &pins);
    i2s_zero_dma_buffer(I2S_NUM);
}

void audio_i2s_read(int16_t *buffer, int samples)
{
    size_t bytes_read;

    i2s_read(
        I2S_NUM,
        raw_buffer,
        samples * sizeof(int32_t),
        &bytes_read,
        portMAX_DELAY
    );

    int count = bytes_read / sizeof(int32_t);

    for (int i = 0; i < count; i++)
    {
        buffer[i] = raw_buffer[i] >> 13;  // 24-bit → 16-bit
    }
}