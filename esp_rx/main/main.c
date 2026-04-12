#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "rx";

#define AP_SSID "esp-beacon"

static EventGroupHandle_t wifi_evt;
#define GOT_IP_BIT BIT0

static uint32_t csi_count    = 0;
static uint32_t beacon_count = 0;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_evt, GOT_IP_BIT);
    }
}

static void csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) return;
    int64_t ts = esp_timer_get_time();
    const int8_t *csi = info->buf;
    uint16_t len = info->len;

    printf("CSI,%lld,%d,%d,%d,%u,"
           "%02X:%02X:%02X:%02X:%02X:%02X,",
           ts, info->rx_ctrl.rssi, info->rx_ctrl.rate,
           info->rx_ctrl.channel, len,
           info->mac[0], info->mac[1], info->mac[2],
           info->mac[3], info->mac[4], info->mac[5]);
    for (uint16_t i = 0; i < len; i++) {
        printf("%d%c", csi[i], (i + 1 == len) ? '\n' : ' ');
    }
    csi_count++;
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len) {
    beacon_count++;
}

static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_LOGI(TAG, "ESP-NOW initialized");
}

static void wifi_init_sta(void) {
    wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &h2));

    wifi_config_t sta = {
        .sta = {
            .ssid = AP_SSID,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to %s...", AP_SSID);
    xEventGroupWaitBits(wifi_evt, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "connected");

    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);

    wifi_csi_config_t csi_cfg = {0};
    csi_cfg.lltf_en          = true;
    csi_cfg.htltf_en         = true;
    csi_cfg.stbc_htltf2_en   = true;
    csi_cfg.ltf_merge_en     = true;
    csi_cfg.channel_filter_en = true;
    csi_cfg.manu_scale       = false;
    csi_cfg.shift            = 0;

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI enabled");
}

static void heartbeat_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "csi callbacks: %lu  beacons: %lu",
                 (unsigned long)csi_count, (unsigned long)beacon_count);
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    espnow_init();
    xTaskCreate(heartbeat_task, "hb", 2048, NULL, 3, NULL);
}
