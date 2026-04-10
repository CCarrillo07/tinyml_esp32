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

static const char *TAG = "APP";  // Logging tag

/* =========================
   SYSTEM STATE MACHINE
   ========================= */
typedef enum {
    STATE_LISTENING,   // Waiting for speech
    STATE_RECORDING,   // Capturing speech
    STATE_PROCESSING   // Running inference / upload
} system_state_t;

static system_state_t state = STATE_LISTENING;

/* =========================
   PARAMETERS
   ========================= */
#define VAD_THRESHOLD 700        // Energy threshold to detect speech
#define VAD_MIN_ENERGY 120       // Minimum energy to ignore noise

#define SILENCE_FRAMES_END 12    // Frames of silence to consider speech ended
#define SPEECH_START_FRAMES 2    // Frames required to confirm speech start

#define PRE_SPEECH_SAMPLES (1600)   // Buffer to store audio before speech detection
#define MAX_AUDIO_SAMPLES (16000)   // Max length of captured audio

/* =========================
   BUFFERS (HEAP ALLOCATED)
   ========================= */
static int16_t *circular_buffer = NULL;   // Sliding window buffer for audio frames
static int16_t *pre_buffer = NULL;        // Stores audio before speech starts
static int16_t *speech_buffer = NULL;     // Stores full detected speech segment

// MFCC working buffers (moved off stack to avoid stack overflow)
static float *mfcc_frame_buffer = NULL;   // Single frame (float normalized)
static float *mfcc_coeff_buffer = NULL;   // Output MFCC coefficients

static int pre_index = 0;     // Index for circular pre-buffer
static int pre_filled = 0;    // Number of valid samples in pre-buffer
static int speech_index = 0;  // Number of samples in speech buffer

static int initialized = 0;   // Flag to skip first invalid frame
static int speech_active = 0; // Indicates if speech is currently ongoing
static int silence_count = 0; // Counts consecutive silent frames
static bool g_wifi_ok = false;

/* =========================
   MODE HELPERS
   ========================= */
static bool display_enabled(void)
{
    return (APP_RUN_MODE == APP_RUN_MODE_DISPLAY);  // Check if display mode is active
}

static bool upload_enabled(void)
{
    return (APP_RUN_MODE == APP_RUN_MODE_UPLOAD);   // Check if upload mode is active
}

/* Inference is always enabled */
static bool inference_enabled(void)
{
    return true;
}

/* LVGL tick (required for UI updates) */
static void lvgl_tick_callback(void *arg)
{
    (void)arg;
    lv_tick_inc(1);  // Increment LVGL internal tick (1 ms)
}

/* Initialize display if running in display mode */
static void init_display_if_needed(void)
{
    if (!display_enabled()) {
        return;
    }

    gc9a01_init();  // Initialize LCD

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = lvgl_tick_callback,
        .name = "lvgl_tick"
    };

    esp_timer_handle_t lvgl_tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1000));  // 1 ms tick
}

/* Send text to display if enabled */
static void update_display_status(const char *text)
{
    if (!display_enabled()) {
        return;
    }

    display_send_text(text);
}

/* Run LVGL handler if display mode is active */
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
    // Allocate buffers in heap (avoids stack overflow)
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

    // Validate allocation
    if (!circular_buffer || !pre_buffer || !speech_buffer ||
        !mfcc_frame_buffer || !mfcc_coeff_buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffers");
        return false;
    }

    // Initialize buffers to zero
    memset(circular_buffer, 0, FRAME_SIZE * sizeof(int16_t));
    memset(pre_buffer, 0, PRE_SPEECH_SAMPLES * sizeof(int16_t));
    memset(speech_buffer, 0, MAX_AUDIO_SAMPLES * sizeof(int16_t));
    memset(mfcc_frame_buffer, 0, FRAME_SIZE * sizeof(float));
    memset(mfcc_coeff_buffer, 0, MFCC_COUNT * sizeof(float));

    ESP_LOGI(TAG, "Audio buffers allocated on heap");
    return true;
}

/* =========================
   VOICE ACTIVITY DETECTION
   ========================= */
static int detect_speech(int16_t *buffer)
{
    long energy = 0;

    // Compute average absolute energy of frame
    for (int i = 0; i < FRAME_SIZE; i++) {
        energy += abs(buffer[i]);
    }

    energy /= FRAME_SIZE;

    if (energy < VAD_MIN_ENERGY) {
        return 0;  // Ignore low-energy noise
    }

    static int speech_frames = 0;  // Tracks consecutive speech frames

    if (energy > VAD_THRESHOLD) {
        speech_frames++;
    } else {
        speech_frames = 0;
    }

    return (speech_frames >= SPEECH_START_FRAMES);  // Confirm speech start
}

/* =========================
   PRE-BUFFER MANAGEMENT
   ========================= */
static void store_pre_buffer(int16_t *frame)
{
    // Store recent samples before speech detection
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
    // Copy pre-buffer into speech buffer (to avoid cutting initial speech)
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
   INFERENCE PATH
   ========================= */
static void process_full_audio_for_inference(int16_t *audio, int length)
{
    ESP_LOGI(TAG, "Generating MFCC sequence...");

    reset_mfcc_buffer();

    int frame_count = 0;
    float max_val = 1e-6f;

    // Normalize audio (match training pipeline)
    for (int i = 0; i < length; i++) {
        float v = fabsf((float)audio[i]);
        if (v > max_val) {
            max_val = v;
        }
    }

    // Generate MFCC frames using sliding window
    for (int i = 0; i < length - FRAME_SIZE; i += HOP_LENGTH) {
        for (int j = 0; j < FRAME_SIZE; j++) {
            mfcc_frame_buffer[j] = audio[i + j] / max_val;
        }

        mfcc_compute(mfcc_frame_buffer, mfcc_coeff_buffer);  // Compute MFCC
        loop(mfcc_coeff_buffer);  // Feed frame into inference buffer
        frame_count++;

        // Prevent watchdog reset during long processing
        if (i % (HOP_LENGTH * 5) == 0) {
            esp_task_wdt_reset();
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Total MFCC frames: %d", frame_count);

    esp_task_wdt_reset();
    run_inference_on_speech();  // Run model inference
    esp_task_wdt_reset();
}

/* =========================
   UPLOAD PATH
   ========================= */
static void process_full_audio_for_upload(int16_t *audio, int length)
{
    ESP_LOGI(TAG, "Upload mode: uploading captured utterance");

    if (!g_wifi_ok) {
        g_wifi_ok = wifi_upload_init();  // Initialize WiFi if needed
    }

    if (!g_wifi_ok) {
        ESP_LOGE(TAG, "Upload mode: WiFi not ready, skipping upload");
        return;
    }

    esp_task_wdt_reset();

    bool ok = upload_pcm_audio(audio, length);  // Send raw PCM to server

    esp_task_wdt_reset();

    if (ok) {
        ESP_LOGI(TAG, "Upload success");
    } else {
        ESP_LOGE(TAG, "Upload failed");
    }
}

/* =========================
   MAIN LOOP
   ========================= */
void app_main(void)
{
    if (!allocate_audio_buffers()) {
        ESP_LOGE(TAG, "Stopping app because buffer allocation failed");
        return;
    }

    audio_i2s_init();  // Initialize microphone

    if (inference_enabled()) {
        setup();  // Initialize ML model
    }

    init_display_if_needed();

    if (upload_enabled()) {
        g_wifi_ok = wifi_upload_init();
    }

    esp_task_wdt_add(NULL);  // Add watchdog for main task

    while (1) {
        run_display_task_if_needed();

        if (state == STATE_PROCESSING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Slide buffer for streaming audio
        memmove(
            circular_buffer,
            circular_buffer + HOP_LENGTH,
            (FRAME_SIZE - HOP_LENGTH) * sizeof(int16_t)
        );

        // Read new audio chunk
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
            }

            speech_active = 1;
            silence_count = 0;

            // Store speech samples
            for (int i = 0; i < HOP_LENGTH; i++) {
                if (speech_index < MAX_AUDIO_SAMPLES) {
                    speech_buffer[speech_index++] = circular_buffer[i];
                }
            }
        } else {
            if (speech_active) {
                silence_count++;

                if (silence_count > SILENCE_FRAMES_END) {
                    ESP_LOGI(TAG, "Speech ended. Samples: %d", speech_index);

                    speech_active = 0;
                    silence_count = 0;

                    if (speech_index > 2500) {
                        state = STATE_PROCESSING;

                        process_full_audio_for_inference(speech_buffer, speech_index);

                        if (display_enabled()) {
                            update_display_if_needed();
                        } else if (upload_enabled()) {
                            process_full_audio_for_upload(speech_buffer, speech_index);
                        }
                    } else {
                        ESP_LOGW(TAG, "Speech too short, skipping");
                    }

                    pre_filled = 0;
                    pre_index = 0;
                    speech_index = 0;

                    state = STATE_LISTENING;
                }
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}