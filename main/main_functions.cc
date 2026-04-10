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

static const char* TAG = "TinyML";  // Logging tag for this module

namespace {
const tflite::Model* model = nullptr;                 // Pointer to the TFLite model
tflite::MicroInterpreter* interpreter = nullptr;     // TFLite Micro interpreter instance
TfLiteTensor* input = nullptr;                       // Model input tensor
TfLiteTensor* output = nullptr;                      // Model output tensor

constexpr int kTensorArenaSize = 70 * 1024;         // Memory reserved for tensors and intermediate buffers
uint8_t* tensor_arena = nullptr;                     // Heap-allocated tensor arena

float mfcc_buffer[MAX_FRAMES][MFCC_COUNT];           // Buffer storing MFCC frames for one utterance
int frame_index = 0;                                 // Number of MFCC frames collected so far

volatile int last_prediction = -1;                   // Last predicted class index
volatile bool new_prediction_available = false;      // Flag indicating a new prediction is ready
}

// =========================
void setup() {
    model = tflite::GetModel(g_model);  // Load model from embedded model data

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema mismatch!");
        return;
    }

    if (!tensor_arena) {
        tensor_arena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_8BIT);  // Allocate tensor arena from heap
        if (!tensor_arena) {
            ESP_LOGE(TAG, "Failed to allocate tensor arena");
            return;
        }

        memset(tensor_arena, 0, kTensorArenaSize);  // Clear allocated memory
        ESP_LOGI(TAG, "Tensor arena allocated on heap: %d bytes", kTensorArenaSize);
    }

    static tflite::MicroMutableOpResolver<10> resolver;  // Register only the operators used by the model
    resolver.AddConv2D();
    resolver.AddFullyConnected();
    resolver.AddMaxPool2D();
    resolver.AddPack();
    resolver.AddReshape();
    resolver.AddShape();
    resolver.AddSoftmax();
    resolver.AddStridedSlice();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);  // Create interpreter using model + resolver + arena

    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed");
        return;
    }

    input = interpreter->input(0);    // Get pointer to input tensor
    output = interpreter->output(0);  // Get pointer to output tensor

    if (!input || !output) {
        ESP_LOGE(TAG, "Input/Output tensor NULL!");
        return;
    }

    ESP_LOGI(TAG, "Model initialized OK");

    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));  // Clear MFCC frame buffer
    frame_index = 0;                              // Reset frame counter
}

// =========================
void loop(float* mfcc) {
    if (frame_index < MAX_FRAMES) {
        memcpy(mfcc_buffer[frame_index], mfcc, sizeof(float) * MFCC_COUNT);  // Store one MFCC frame
        frame_index++;
    }
}

// =========================
#include "esp_task_wdt.h"

extern "C" void run_inference_on_speech() {

    if (frame_index == 0) return;  // Nothing to infer if no frames were collected

    if (!input || !output) {
        ESP_LOGE(TAG, "Model not ready!");
        return;
    }

    ESP_LOGI(TAG, "Frames: %d", frame_index);

    int input_size = input->bytes / sizeof(float);       // Number of float elements in input tensor
    int expected_size = MAX_FRAMES * MFCC_COUNT;         // Expected flattened MFCC size

    if (input_size < expected_size) {
        ESP_LOGE(TAG, "Input tensor too small! %d < %d", input_size, expected_size);
        return;
    }

    for (int i = 0; i < MAX_FRAMES; i++) {
        for (int j = 0; j < MFCC_COUNT; j++) {

            float value = 0.0f;  // Default padding value

            if (i < frame_index) {
                value = mfcc_buffer[i][j];  // Use collected MFCC frame if available
            }

            int index = i * MFCC_COUNT + j;  // Flatten 2D MFCC buffer into 1D tensor input

            if (index < input_size) {
                input->data.f[index] = value;  // Copy value into model input tensor
            }
        }
    }

    if (interpreter->Invoke() == kTfLiteOk) {  // Run inference

        int count = output->dims->data[1];     // Number of output classes
        int max_idx = 0;                       // Predicted class index
        float max_score = output->data.f[0];   // Highest score found so far

        for (int i = 1; i < count; i++) {
            if (output->data.f[i] > max_score) {
                max_score = output->data.f[i];
                max_idx = i;                   // Track index of best score
            }
        }

        ESP_LOGI(TAG, "Prediction: %s", kLabels[max_idx]);  // Print predicted label

        last_prediction = max_idx;              // Save latest prediction
        new_prediction_available = true;        // Mark prediction as ready for display/update

    } else {
        ESP_LOGE(TAG, "Inference failed");
    }

    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));  // Clear MFCC buffer after inference
    frame_index = 0;                              // Reset frame counter for next utterance
}

// =========================
extern "C" void reset_mfcc_buffer() {
    memset(mfcc_buffer, 0, sizeof(mfcc_buffer));  // Clear stored MFCC frames
    frame_index = 0;                              // Reset frame count
}

extern "C" int get_frame_count() {
    return frame_index;  // Return number of collected MFCC frames
}

extern "C" void display_send_text(const char *text);

extern "C" void update_display_if_needed() {

    if (new_prediction_available) {
        new_prediction_available = false;  // Consume prediction flag

        if (last_prediction >= 0) {
            ESP_LOGI(TAG, "Detected label: %s", kLabels[last_prediction]);  // Log detected label
            display_send_text(kLabels[last_prediction]);                    // Send label to display
        }
    }
}