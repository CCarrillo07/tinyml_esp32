#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wifi_upload_init(void);
bool upload_pcm_audio(const int16_t *samples, int sample_count);

#ifdef __cplusplus
}
#endif