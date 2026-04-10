#include "audio_i2s.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"

#define I2S_NUM I2S_NUM_0       // I2S peripheral instance
#define FRAME_SIZE 512          // Number of samples per audio frame

/* =========================
   STATIC BUFFER
   ========================= */
static int32_t raw_buffer[FRAME_SIZE];  // Buffer to store raw 32-bit I2S samples

void audio_i2s_init(void)
{
    i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,   // Configure as master receiver (microphone input)
        .sample_rate = SAMPLE_RATE,              // Sampling rate (must match MFCC pipeline)
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // I2S reads 32-bit aligned data (typical for INMP441)
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Use single channel (mono audio)
        .communication_format = I2S_COMM_FORMAT_I2S,  // Standard I2S protocol
        .intr_alloc_flags = 0,                   // Default interrupt allocation
        .dma_buf_count = 4,                     // Number of DMA buffers
        .dma_buf_len = 512,                     // Size of each DMA buffer
        .use_apll = false                       // Use default clock (no APLL)
    };

    i2s_pin_config_t pins = {
        .bck_io_num = 26,   // Bit clock (SCK)
        .ws_io_num = 25,    // Word select (LRCLK / WS)
        .data_out_num = I2S_PIN_NO_CHANGE,  // Not used (input only)
        .data_in_num = 33   // Serial data input (SD from microphone)
    };

    i2s_driver_install(I2S_NUM, &config, 0, NULL);  // Install and configure I2S driver
    i2s_set_pin(I2S_NUM, &pins);                    // Assign GPIO pins for I2S
    i2s_zero_dma_buffer(I2S_NUM);                  // Clear DMA buffers before starting
}

void audio_i2s_read(int16_t *buffer, int samples)
{
    size_t bytes_read;

    i2s_read(
        I2S_NUM,
        raw_buffer,                         // Read into 32-bit raw buffer
        samples * sizeof(int32_t),         // Number of bytes to read
        &bytes_read,
        portMAX_DELAY                      // Block until data is available
    );

    int count = bytes_read / sizeof(int32_t);  // Number of valid samples read

    for (int i = 0; i < count; i++)
    {
        buffer[i] = raw_buffer[i] >> 13;  // Convert 24-bit microphone data to 16-bit (scaling down)
    }
}