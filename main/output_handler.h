#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void HandleOutput(float* scores, int count);

/* ✅ ADD THIS */
extern const char* kLabels[];

#ifdef __cplusplus
}
#endif