#include <stdio.h>
#include "output_handler.h"
#include "tensorflow/lite/micro/micro_log.h"

#ifdef __cplusplus
extern "C" {
#endif

// List of class labels corresponding to model output indices
const char* kLabels[] = {
    "yes","no","on","off","up","down","left","right","stop","go"
};

/* =========================
   MODEL OUTPUT HANDLER
   ========================= */
/* Note: Avoids float printing to reduce overhead on embedded systems */
void HandleOutput(float* scores, int count) {

    MicroPrintf("Prediction done");  // Log inference completion

    // Find index of the highest score (argmax)
    int max_idx = 0;
    float max_score = scores[0];  // Initialize with first score

    for (int i = 1; i < count; i++) {
        if (scores[i] > max_score) {
            max_score = scores[i];  // Update max score
            max_idx = i;            // Track corresponding index
        }
    }

    // Print only the predicted class index (avoids float formatting)
    MicroPrintf("Top index: %d", max_idx);
}

#ifdef __cplusplus
}
#endif