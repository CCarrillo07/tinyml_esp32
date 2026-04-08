#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "audio_i2s.h"
#include "mfcc.h"
#include "main_functions.h"
#include "gc9a01_lvgl_display.h"
#include "wifi_upload.h"
#include "app_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#define FRAME_SIZE 512
#define HOP_LENGTH 160

static const char *TAG = "APP";

/* =========================
   SYSTEM STATE MACHINE
   ========================= */
typedef enum {
    STATE_LISTENING,
    STATE_RECORDING,
    STATE_PROCESSING
} system_state_t;

static system_state_t state = STATE_LISTENING;

/* =========================
   PARAMETERS
   ========================= */
#define VAD_THRESHOLD 700
#define VAD_MIN_ENERGY 120

#define SILENCE_FRAMES_END 12
#define SPEECH_START_FRAMES 2

#define PRE_SPEECH_SAMPLES (1600)
#define MAX_AUDIO_SAMPLES (16000)

/* =========================
   BUFFERS (ALLOCATED ON HEAP)
   ========================= */
static int16_t *circular_buffer = NULL;
static int16_t *pre_buffer = NULL;
static int16_t *speech_buffer = NULL;

/* MFCC work buffers moved off stack */
static float *mfcc_frame_buffer = NULL;
static float *mfcc_coeff_buffer = NULL;

static int pre_index = 0;
static int pre_filled = 0;
static int speech_index = 0;

static int initialized = 0;
static int speech_active = 0;
static int silence_count = 0;
static bool g_wifi_ok = false;

/* =========================
   DISPLAY / LVGL CONTROL
   ========================= */
static bool display_enabled(void)
{
#if APP_MODE_DEBUG_UPLOAD
    return false;
#else
    return true;
#endif
}

static bool inference_enabled(void)
{
#if APP_MODE_DEBUG_UPLOAD
    return false;
#else
    return true;
#endif
}

static bool upload_enabled(void)
{
#if APP_MODE_DEBUG_UPLOAD
    return true;
#else
    return false;
#endif
}

static void lvgl_tick_callback(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void init_display_if_needed(void)
{
    if (!display_enabled()) {
        return;
    }

    gc9a01_init();

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = lvgl_tick_callback,
        .name = "lvgl_tick"
    };

    esp_timer_handle_t lvgl_tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1000));
}

static void update_display_status(const char *text)
{
    if (!display_enabled()) {
        return;
    }
    display_send_text(text);
}

static void run_display_task_if_needed(void)
{
    if (!display_enabled()) {
        return;
    }
    display_task();
}

/* =========================
   BUFFER ALLOCATION
   ========================= */
static bool allocate_audio_buffers(void)
{
    circular_buffer = (int16_t *)heap_caps_malloc(
        FRAME_SIZE * sizeof(int16_t),
        MALLOC_CAP_8BIT
    );

    pre_buffer = (int16_t *)heap_caps_malloc(
        PRE_SPEECH_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_8BIT
    );

    speech_buffer = (int16_t *)heap_caps_malloc(
        MAX_AUDIO_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_8BIT
    );

    mfcc_frame_buffer = (float *)heap_caps_malloc(
        FRAME_SIZE * sizeof(float),
        MALLOC_CAP_8BIT
    );

    mfcc_coeff_buffer = (float *)heap_caps_malloc(
        MFCC_COUNT * sizeof(float),
        MALLOC_CAP_8BIT
    );

    if (!circular_buffer || !pre_buffer || !speech_buffer ||
        !mfcc_frame_buffer || !mfcc_coeff_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        return false;
    }

    memset(circular_buffer, 0, FRAME_SIZE * sizeof(int16_t));
    memset(pre_buffer, 0, PRE_SPEECH_SAMPLES * sizeof(int16_t));
    memset(speech_buffer, 0, MAX_AUDIO_SAMPLES * sizeof(int16_t));
    memset(mfcc_frame_buffer, 0, FRAME_SIZE * sizeof(float));
    memset(mfcc_coeff_buffer, 0, MFCC_COUNT * sizeof(float));

    ESP_LOGI(TAG, "Audio buffers allocated on heap");
    return true;
}

/* =========================
   VAD
   ========================= */
static int detect_speech(int16_t *buffer)
{
    long energy = 0;

    for (int i = 0; i < FRAME_SIZE; i++) {
        energy += abs(buffer[i]);
    }

    energy /= FRAME_SIZE;

    //ESP_LOGI(TAG, "Frame energy: %ld", energy); 

    if (energy < VAD_MIN_ENERGY) {
        return 0;
    }

    static int speech_frames = 0;

    if (energy > VAD_THRESHOLD) {
        speech_frames++;
    } else {
        speech_frames = 0;
    }

    return speech_frames >= SPEECH_START_FRAMES;
}

/* =========================
   PRE-BUFFER
   ========================= */
static void store_pre_buffer(int16_t *frame)
{
    for (int i = 0; i < HOP_LENGTH; i++) {
        pre_buffer[pre_index] = frame[i];
        pre_index = (pre_index + 1) % PRE_SPEECH_SAMPLES;

        if (pre_filled < PRE_SPEECH_SAMPLES) {
            pre_filled++;
        }
    }
}

static void flush_pre_buffer_into_speech_buffer(void)
{
    int start = (pre_index - pre_filled + PRE_SPEECH_SAMPLES) % PRE_SPEECH_SAMPLES;

    for (int i = 0; i < pre_filled; i++) {
        int idx = (start + i) % PRE_SPEECH_SAMPLES;

        if (speech_index < MAX_AUDIO_SAMPLES) {
            speech_buffer[speech_index++] = pre_buffer[idx];
        }
    }

    ESP_LOGI(TAG, "Injected pre-buffer into speech buffer: %d samples", pre_filled);
}

/* =========================
   MODE A: NORMAL INFERENCE
   ========================= */
static void process_full_audio_for_inference(int16_t *audio, int length)
{
    ESP_LOGI(TAG, "Generating MFCC sequence...");

    reset_mfcc_buffer();

    int frame_count = 0;
    float max_val = 1e-6f;

    for (int i = 0; i < length; i++) {
        float v = fabsf((float)audio[i]);
        if (v > max_val) {
            max_val = v;
        }
    }

    for (int i = 0; i < length - FRAME_SIZE; i += HOP_LENGTH) {
        for (int j = 0; j < FRAME_SIZE; j++) {
            mfcc_frame_buffer[j] = audio[i + j] / max_val;
        }

        mfcc_compute(mfcc_frame_buffer, mfcc_coeff_buffer);
        loop(mfcc_coeff_buffer);
        frame_count++;

        if (i % (HOP_LENGTH * 5) == 0) {
            esp_task_wdt_reset();
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Total MFCC frames: %d", frame_count);

    esp_task_wdt_reset();
    run_inference_on_speech();
    esp_task_wdt_reset();
}

/* =========================
   MODE B: DEBUG UPLOAD
   ========================= */
static void process_full_audio_for_debug_upload(int16_t *audio, int length)
{
    ESP_LOGI(TAG, "Mode B: uploading captured utterance");
    ESP_LOGI(TAG, "Mode B: sample count = %d", length);

    if (!g_wifi_ok) {
        g_wifi_ok = wifi_upload_init();
    }

    if (!g_wifi_ok) {
        ESP_LOGE(TAG, "Mode B: WiFi still not ready, skipping upload");
        return;
    }

    esp_task_wdt_reset();

    bool ok = upload_pcm_audio(audio, length);

    esp_task_wdt_reset();

    if (ok) {
        ESP_LOGI(TAG, "Mode B: upload success");
    } else {
        ESP_LOGE(TAG, "Mode B: upload failed");
    }
}

/* =========================
   MAIN
   ========================= */
void app_main(void)
{
    if (!allocate_audio_buffers()) {
        ESP_LOGE(TAG, "Stopping app because buffer allocation failed");
        return;
    }

    audio_i2s_init();

    if (inference_enabled()) {
        setup();
    }

    init_display_if_needed();

    if (upload_enabled()) {
        g_wifi_ok = wifi_upload_init();
        if (!g_wifi_ok) {
            ESP_LOGE(TAG, "WiFi init failed in Mode B");
        }
    }

    esp_task_wdt_add(NULL);

    // update_display_status("Listening...");

    while (1) {
        run_display_task_if_needed();

        if (state == STATE_PROCESSING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        memmove(
            circular_buffer,
            circular_buffer + HOP_LENGTH,
            (FRAME_SIZE - HOP_LENGTH) * sizeof(int16_t)
        );

        audio_i2s_read(
            circular_buffer + (FRAME_SIZE - HOP_LENGTH),
            HOP_LENGTH
        );

        if (!initialized) {
            initialized = 1;
            continue;
        }

        store_pre_buffer(circular_buffer);

        int speech = detect_speech(circular_buffer);

        if (speech) {
            if (!speech_active) {
                ESP_LOGI(TAG, "Speech START detected");

                speech_index = 0;
                flush_pre_buffer_into_speech_buffer();

                state = STATE_RECORDING;
                // update_display_status("Recording...");
            }

            speech_active = 1;
            silence_count = 0;

            for (int i = 0; i < HOP_LENGTH; i++) {
                if (speech_index < MAX_AUDIO_SAMPLES) {
                    speech_buffer[speech_index++] = circular_buffer[i];
                }
            }
        } else {
            if (speech_active) {
                silence_count++;

                if (silence_count > SILENCE_FRAMES_END) {
                    ESP_LOGI(TAG, "Speech ended. Samples captured: %d", speech_index);

                    speech_active = 0;
                    silence_count = 0;

                    if (speech_index > 3500) {
                        state = STATE_PROCESSING;
                        // update_display_status("Processing...");

                        if (inference_enabled()) {
                            process_full_audio_for_inference(speech_buffer, speech_index);
                            update_display_if_needed();
                        } else if (upload_enabled()) {
                            process_full_audio_for_debug_upload(speech_buffer, speech_index);
                        }
                    } else {
                        ESP_LOGW(TAG, "Speech too short, skipping");
                    }

                    pre_filled = 0;
                    pre_index = 0;
                    speech_index = 0;

                    state = STATE_LISTENING;
                    // update_display_status("Listening...");
                }
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}