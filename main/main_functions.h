#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void setup();
void loop(float* mfcc);
void reset_mfcc_buffer();
void run_inference_on_speech();
int get_frame_count();
void update_display_if_needed();

// LVGL display
void display_send_text(const char *text);

#ifdef __cplusplus
}
#endif