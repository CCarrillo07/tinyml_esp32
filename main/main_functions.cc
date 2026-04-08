#include "main_functions.h"
#include "model.h"
#include "output_handler.h"
#include "mfcc.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "TinyML";

namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input = nullptr;
TfLiteTensor* output = nullptr;

constexpr int kTensorArenaSize = 70 * 1024;
uint8_t* tensor_arena = nullptr;

float mfcc_buffer[MAX_FRAMES][MFCC_COUNT];
int frame_index = 0;

volatile int last_prediction = -1;
volatile bool new_prediction_available = false;
}

// =========================
void setup() {
    model = tflite::GetModel(g_model);

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema mismatch!");
        return;
    }

    if (!tensor_arena) {
        tensor_arena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_8BIT);
        if (!tensor_arena) {
            ESP_LOGE(TAG, "Failed to allocate tensor arena");
            return;
        }

        memset(tensor_arena, 0, kTensorArenaSize);
        ESP_LOGI(TAG, "Tensor arena allocated on heap: %d bytes", kTensorArenaSize);
    }

    static tflite::MicroMutableOpResolver<10> resolver;
    resolver.AddConv2D();
    resolver.AddFullyConnected();
    resolver.AddMaxPool2D();
    resolver.AddPack();
    resolver.AddReshape();
    resolver.AddShape();
    resolver.AddSoftmax();
    resolver.AddStridedSlice();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);

    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return;
    }

    input = interpreter->input(0);
    output = interpreter->output(0);

    if (!input || !output) {
        ESP_LOGE(TAG, "Input/Output tensor NULL!");
        return;
    }

    ESP_LOGI(TAG, "Model initialized OK");

    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));
    frame_index = 0;
}

// =========================
void loop(float* mfcc) {
    if (frame_index < MAX_FRAMES) {
        memcpy(mfcc_buffer[frame_index], mfcc, sizeof(float) * MFCC_COUNT);
        frame_index++;
    }
}

// =========================
#include "esp_task_wdt.h"

extern "C" void run_inference_on_speech() {

    if (frame_index == 0) return;

    if (!input || !output) {
        ESP_LOGE(TAG, "Model not ready!");
        return;
    }

    ESP_LOGI(TAG, "Frames: %d", frame_index);

    int input_size = input->bytes / sizeof(float);
    int expected_size = MAX_FRAMES * MFCC_COUNT;

    if (input_size < expected_size) {
        ESP_LOGE(TAG, "Input tensor too small! %d < %d", input_size, expected_size);
        return;
    }

    for (int i = 0; i < MAX_FRAMES; i++) {
        for (int j = 0; j < MFCC_COUNT; j++) {

            float value = 0.0f;

            if (i < frame_index) {
                value = mfcc_buffer[i][j];
            }

            int index = i * MFCC_COUNT + j;

            if (index < input_size) {
                input->data.f[index] = value;
            }
        }
    }

    if (interpreter->Invoke() == kTfLiteOk) {

        int count = output->dims->data[1];
        int max_idx = 0;
        float max_score = output->data.f[0];

        for (int i = 1; i < count; i++) {
            if (output->data.f[i] > max_score) {
                max_score = output->data.f[i];
                max_idx = i;
            }
        }

        ESP_LOGI(TAG, "Prediction: %s", kLabels[max_idx]);

        last_prediction = max_idx;
        new_prediction_available = true;

    } else {
        ESP_LOGE(TAG, "Inference failed");
    }

    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));
    frame_index = 0;
}

// =========================
extern "C" void reset_mfcc_buffer() {
    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));
    frame_index = 0;
}

extern "C" int get_frame_count() {
    return frame_index;
}

extern "C" void display_send_text(const char *text);

extern "C" void update_display_if_needed() {

    if (new_prediction_available) {
        new_prediction_available = false;

        if (last_prediction >= 0) {
            ESP_LOGI(TAG, "Detected label: %s", kLabels[last_prediction]);
            display_send_text(kLabels[last_prediction]);
        }
    }
}