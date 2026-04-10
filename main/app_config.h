#pragma once

/* =========================================================
   APP RUN MODE
   =========================================================
   0 = Inference + Display
   1 = Inference + Audio Upload
   ========================================================= */
#define APP_RUN_MODE 0

#define APP_RUN_MODE_DISPLAY 0
#define APP_RUN_MODE_UPLOAD  1

/* =========================================================
   WIFI CONFIG (used only in Mode 1)
   ========================================================= */
//#define WIFI_SSID       "Totalplay-44AD"
//#define WIFI_PASSWORD   "44AD1347dXQ4SZm3"
#define WIFI_SSID       "IZZI-9F14"
#define WIFI_PASSWORD   "bTmzZHrmfZEzZcYzbs"


/* Use your PC/server IP reachable from the ESP32 */
#define UPLOAD_URL      "http://192.168.0.187:5000/upload"

/* =========================================================
   AUDIO / SERVER METADATA
   ========================================================= */
#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1