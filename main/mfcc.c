#include <math.h>
#include <string.h>
#include "mfcc.h"
#include "esp_dsp.h"
#include "esp_log.h"

static const char *TAG = "MFCC";  // Logging tag

// Buffers used during MFCC computation
static float fft_buffer[FRAME_SIZE * 2];     // Complex FFT buffer (real + imaginary)
static float power[FRAME_SIZE / 2];          // Power spectrum (half of FFT bins)
static float mel_energy[MEL_FILTERS];        // Mel filterbank energies

// Precomputed matrices (initialized once)
static float mel_filterbank[MEL_FILTERS][FRAME_SIZE / 2];  // Mel filterbank weights
static float dct_matrix[MFCC_COUNT][MEL_FILTERS];          // DCT transform matrix

static int initialized = 0;  // Flag to ensure one-time initialization

/* =========================
   MEL SCALE CONVERSIONS
   ========================= */
static float hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + hz / 700.0f);  // Convert frequency from Hz to Mel
}

static float mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);  // Convert Mel back to Hz
}

/* =========================
   BUILD MEL FILTERBANK (ONCE)
   ========================= */
static void init_mel_filterbank(void) {
    float mel_min = hz_to_mel(0);                    // Lower bound in Mel scale
    float mel_max = hz_to_mel(SAMPLE_RATE / 2);      // Nyquist frequency in Mel

    float mel_points[MEL_FILTERS + 2];               // Mel-spaced points
    float hz_points[MEL_FILTERS + 2];                // Corresponding Hz values
    int bin[MEL_FILTERS + 2];                        // FFT bin indices

    // Compute Mel → Hz → FFT bin mapping
    for (int i = 0; i < MEL_FILTERS + 2; i++) {
        mel_points[i] = mel_min + (mel_max - mel_min) * i / (MEL_FILTERS + 1);
        hz_points[i] = mel_to_hz(mel_points[i]);
        bin[i] = (int)((FRAME_SIZE + 1) * hz_points[i] / SAMPLE_RATE);
    }

    // Clear filterbank matrix
    memset(mel_filterbank, 0, sizeof(mel_filterbank));

    // Build triangular filters
    for (int m = 1; m <= MEL_FILTERS; m++) {
        int f_m_minus = bin[m - 1];  // Left bin
        int f_m = bin[m];            // Center bin
        int f_m_plus = bin[m + 1];   // Right bin

        // Rising edge of triangle
        for (int k = f_m_minus; k < f_m && k < FRAME_SIZE / 2; k++) {
            int denom = (f_m - f_m_minus);
            if (denom > 0) {
                mel_filterbank[m - 1][k] =
                    (float)(k - f_m_minus) / (float)denom;
            }
        }

        // Falling edge of triangle
        for (int k = f_m; k < f_m_plus && k < FRAME_SIZE / 2; k++) {
            int denom = (f_m_plus - f_m);
            if (denom > 0) {
                mel_filterbank[m - 1][k] =
                    (float)(f_m_plus - k) / (float)denom;
            }
        }
    }
}

/* =========================
   BUILD DCT MATRIX (ONCE)
   ========================= */
static void init_dct(void) {
    for (int i = 0; i < MFCC_COUNT; i++) {
        for (int j = 0; j < MEL_FILTERS; j++) {
            // Orthonormal scaling factor
            float scale = (i == 0)
                ? sqrtf(1.0f / MEL_FILTERS)
                : sqrtf(2.0f / MEL_FILTERS);

            // DCT-II basis function
            dct_matrix[i][j] =
                scale * cosf(i * (j + 0.5f) * (float)M_PI / MEL_FILTERS);
        }
    }
}

/* =========================
   INITIALIZATION (RUN ONCE)
   ========================= */
static void mfcc_init(void) {
    if (initialized) {
        return;  // Skip if already initialized
    }

    // Initialize FFT (ESP-DSP optimized)
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, FRAME_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %d", ret);
        return;
    }

    // Precompute filterbank and DCT
    init_mel_filterbank();
    init_dct();

    initialized = 1;
    ESP_LOGI(TAG, "MFCC init OK");
}

/* =========================
   MFCC COMPUTATION (PER FRAME)
   ========================= */
void mfcc_compute(float *input, float *mfcc_out) {
    mfcc_init();  // Ensure initialization

    // If initialization failed, return zeroed output
    if (!initialized) {
        memset(mfcc_out, 0, MFCC_COUNT * sizeof(float));
        return;
    }

    // Apply Hann window and prepare complex FFT input
    for (int i = 0; i < FRAME_SIZE; i++) {
        float window = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FRAME_SIZE - 1));
        fft_buffer[2 * i] = input[i] * window;  // Real part
        fft_buffer[2 * i + 1] = 0.0f;           // Imaginary part
    }

    // Perform FFT (real → complex)
    dsps_fft2r_fc32(fft_buffer, FRAME_SIZE);
    dsps_bit_rev_fc32(fft_buffer, FRAME_SIZE);
    dsps_cplx2reC_fc32(fft_buffer, FRAME_SIZE);

    // Compute power spectrum
    for (int i = 0; i < FRAME_SIZE / 2; i++) {
        power[i] =
            (fft_buffer[2 * i] * fft_buffer[2 * i] +
             fft_buffer[2 * i + 1] * fft_buffer[2 * i + 1]) / FRAME_SIZE;
    }

    // Reset Mel energy buffer
    memset(mel_energy, 0, sizeof(mel_energy));

    // Apply Mel filterbank
    for (int m = 0; m < MEL_FILTERS; m++) {
        for (int k = 0; k < FRAME_SIZE / 2; k++) {
            mel_energy[m] += power[k] * mel_filterbank[m][k];
        }

        // Log compression for dynamic range
        mel_energy[m] = log10f(mel_energy[m] + 1e-6f);
    }

    // Apply DCT to obtain MFCC coefficients
    for (int i = 0; i < MFCC_COUNT; i++) {
        mfcc_out[i] = 0.0f;

        for (int j = 0; j < MEL_FILTERS; j++) {
            mfcc_out[i] += dct_matrix[i][j] * mel_energy[j];
        }
    }
}