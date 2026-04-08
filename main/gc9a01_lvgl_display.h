#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void gc9a01_init(void);
void display_send_text(const char *text);

/* 🔥 ADD THIS */
void display_task(void);

#ifdef __cplusplus
}
#endif