#pragma once

/* =========================================================
   APP MODE
   =========================================================
   0 = Mode A -> normal inference + display
   1 = Mode B -> debug audio upload
   ========================================================= */
#define APP_MODE_DEBUG_UPLOAD 0

/* =========================================================
   WIFI CONFIG (used only in Mode B)
   ========================================================= */
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