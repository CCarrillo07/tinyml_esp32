#include <math.h>
#include <string.h>
#include "mfcc.h"
#include "esp_dsp.h"
#include "esp_log.h"

static const char *TAG = "MFCC";

static float fft_buffer[FRAME_SIZE * 2];
static float power[FRAME_SIZE / 2];
static float mel_energy[MEL_FILTERS];

/* keep your current static allocation for now */
static float mel_filterbank[MEL_FILTERS][FRAME_SIZE / 2];
static float dct_matrix[MFCC_COUNT][MEL_FILTERS];

static int initialized = 0;

/* ========================= */
static float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

/* =========================
   BUILD FILTERBANK ONCE
   ========================= */
static void init_mel_filterbank(void) {
    float mel_min = hz_to_mel(0);
    float mel_max = hz_to_mel(SAMPLE_RATE / 2);

    float mel_points[MEL_FILTERS + 2];
    float hz_points[MEL_FILTERS + 2];
    int bin[MEL_FILTERS + 2];

    for (int i = 0; i < MEL_FILTERS + 2; i++) {
        mel_points[i] = mel_min + (mel_max - mel_min) * i / (MEL_FILTERS + 1);
        hz_points[i] = mel_to_hz(mel_points[i]);
        bin[i] = (int)((FRAME_SIZE + 1) * hz_points[i] / SAMPLE_RATE);
    }

    memset(mel_filterbank, 0, sizeof(mel_filterbank));

    for (int m = 1; m <= MEL_FILTERS; m++) {
        int f_m_minus = bin[m - 1];
        int f_m = bin[m];
        int f_m_plus = bin[m + 1];

        for (int k = f_m_minus; k < f_m && k < FRAME_SIZE / 2; k++) {
            int denom = (f_m - f_m_minus);
            if (denom > 0) {
                mel_filterbank[m - 1][k] =
                    (float)(k - f_m_minus) / (float)denom;
            }
        }

        for (int k = f_m; k < f_m_plus && k < FRAME_SIZE / 2; k++) {
            int denom = (f_m_plus - f_m);
            if (denom > 0) {
                mel_filterbank[m - 1][k] =
                    (float)(f_m_plus - k) / (float)denom;
            }
        }
    }
}

/* ========================= */
static void init_dct(void) {
    for (int i = 0; i < MFCC_COUNT; i++) {
        for (int j = 0; j < MEL_FILTERS; j++) {
            float scale = (i == 0)
                ? sqrtf(1.0f / MEL_FILTERS)
                : sqrtf(2.0f / MEL_FILTERS);

            dct_matrix[i][j] =
                scale * cosf(i * (j + 0.5f) * (float)M_PI / MEL_FILTERS);
        }
    }
}

/* ========================= */
static void mfcc_init(void) {
    if (initialized) {
        return;
    }

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, FRAME_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %d", ret);
        return;
    }

    init_mel_filterbank();
    init_dct();

    initialized = 1;
    ESP_LOGI(TAG, "MFCC init OK");
}

/* ========================= */
void mfcc_compute(float *input, float *mfcc_out) {
    mfcc_init();

    if (!initialized) {
        memset(mfcc_out, 0, MFCC_COUNT * sizeof(float));
        return;
    }

    for (int i = 0; i < FRAME_SIZE; i++) {
        float window = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FRAME_SIZE - 1));
        fft_buffer[2 * i] = input[i] * window;
        fft_buffer[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft_buffer, FRAME_SIZE);
    dsps_bit_rev_fc32(fft_buffer, FRAME_SIZE);
    dsps_cplx2reC_fc32(fft_buffer, FRAME_SIZE);

    for (int i = 0; i < FRAME_SIZE / 2; i++) {
        power[i] =
            (fft_buffer[2 * i] * fft_buffer[2 * i] +
             fft_buffer[2 * i + 1] * fft_buffer[2 * i + 1]) / FRAME_SIZE;
    }

    memset(mel_energy, 0, sizeof(mel_energy));

    for (int m = 0; m < MEL_FILTERS; m++) {
        for (int k = 0; k < FRAME_SIZE / 2; k++) {
            mel_energy[m] += power[k] * mel_filterbank[m][k];
        }

        mel_energy[m] = log10f(mel_energy[m] + 1e-6f);
    }

    for (int i = 0; i < MFCC_COUNT; i++) {
        mfcc_out[i] = 0.0f;

        for (int j = 0; j < MEL_FILTERS; j++) {
            mfcc_out[i] += dct_matrix[i][j] * mel_energy[j];
        }
    }
}