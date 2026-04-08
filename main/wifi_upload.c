#include "wifi_upload.h"
#include "esp_task_wdt.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_err.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WIFI_UPLOAD";

static EventGroupHandle_t s_wifi_event_group = NULL;

static bool s_nvs_ready = false;
static bool s_netif_ready = false;
static bool s_event_loop_ready = false;
static bool s_sta_netif_created = false;
static bool s_wifi_driver_ready = false;
static bool s_event_handlers_registered = false;
static bool s_wifi_started = false;
static bool s_wifi_ready = false;

static int s_retry_num = 0;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ready = false;

        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected from AP, reason=%d", disc->reason);
        } else {
            ESP_LOGW(TAG, "Disconnected from AP");
        }

        if (s_retry_num < 10) {
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying WiFi connection... (%d/10)", s_retry_num);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_ready = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_upload_init(void)
{
    esp_err_t err;

    if (s_wifi_ready) {
        return true;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi event group");
            return false;
        }
    }

    if (!s_nvs_ready) {
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);
        s_nvs_ready = true;
    }

    if (!s_netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        s_event_loop_ready = true;
    }

    if (!s_sta_netif_created) {
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        if (sta_netif == NULL) {
            ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
            return false;
        }
        s_sta_netif_created = true;
    }

    if (!s_wifi_driver_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_driver_ready = true;
    }

    if (!s_event_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL,
            NULL
        ));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL,
            NULL
        ));

        s_event_handlers_registered = true;
    }

    wifi_config_t wifi_config = {0};

    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -127;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.failure_retry_cnt = 10;
    wifi_config.sta.listen_interval = 3;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_wifi_ready = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_connect();
    }

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        s_wifi_ready = true;
        return true;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return false;
}

bool upload_pcm_audio(const int16_t *samples, int sample_count)
{
    if (!s_wifi_ready) {
        ESP_LOGE(TAG, "WiFi not ready");
        return false;
    }

    if (!samples || sample_count <= 0) {
        ESP_LOGE(TAG, "Invalid audio buffer");
        return false;
    }

    int payload_bytes = sample_count * (int)sizeof(int16_t);

    char sample_rate_str[16];
    char sample_count_str[16];
    snprintf(sample_rate_str, sizeof(sample_rate_str), "%d", AUDIO_SAMPLE_RATE_HZ);
    snprintf(sample_count_str, sizeof(sample_count_str), "%d", sample_count);

    esp_http_client_config_t config = {
        .url = UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", sample_rate_str);
    esp_http_client_set_header(client, "X-Bits-Per-Sample", "16");
    esp_http_client_set_header(client, "X-Channels", "1");
    esp_http_client_set_header(client, "X-Sample-Count", sample_count_str);

    esp_task_wdt_reset();

    esp_err_t err = esp_http_client_open(client, payload_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_task_wdt_reset();

    int written = esp_http_client_write(client, (const char *)samples, payload_bytes);
    if (written != payload_bytes) {
        ESP_LOGE(TAG, "HTTP write failed. Written=%d Expected=%d", written, payload_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_task_wdt_reset();

    (void)esp_http_client_fetch_headers(client);

    esp_task_wdt_reset();

    int http_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Upload done. HTTP status = %d", http_code);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return (http_code >= 200 && http_code < 300);
}