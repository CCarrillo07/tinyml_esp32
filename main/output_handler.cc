#include <stdio.h>
#include "output_handler.h"
#include "tensorflow/lite/micro/micro_log.h"

#ifdef __cplusplus
extern "C" {
#endif

const char* kLabels[] = {
    "yes","no","on","off","up","down","left","right","stop","go"
};

/* ✅ SAFE: no float formatting */
void HandleOutput(float* scores, int count) {

    MicroPrintf("Prediction done");

    /* OPTIONAL: print only index */
    int max_idx = 0;
    float max_score = scores[0];

    for (int i = 1; i < count; i++) {
        if (scores[i] > max_score) {
            max_score = scores[i];
            max_idx = i;
        }
    }

    MicroPrintf("Top index: %d", max_idx);
}

#ifdef __cplusplus
}
#endif