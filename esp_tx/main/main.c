#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "nvs_flash.h"

static const char *TAG = "tx";

#define AP_SSID        "esp-beacon"
#define AP_CHAN        6
#define BEACON_RATE_MS 200

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t BEACON_PAYLOAD[] = "BEACON";

static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHAN,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP '%s' up on channel %d", AP_SSID, AP_CHAN);
}

static void espnow_init(void) {
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {
        .channel = AP_CHAN,
        .ifidx   = WIFI_IF_AP,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "ESP-NOW initialized, broadcast peer added");
}

static bool any_sta_connected(void) {
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) return false;
    return sta_list.num > 0;
}

static void beacon_task(void *arg) {
    while (1) {
        TickType_t tick_start = xTaskGetTickCount();

        if (!any_sta_connected()) {
            ESP_LOGI(TAG, "no STA connected yet, waiting...");
            vTaskDelayUntil(&tick_start, pdMS_TO_TICKS(3000));
            continue;
        }

        esp_err_t err = esp_now_send(BROADCAST_MAC, BEACON_PAYLOAD,
                                     sizeof(BEACON_PAYLOAD) - 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send failed: %d", err);
        } else {
            ESP_LOGI(TAG, "beacon sent via ESP-NOW broadcast");
        }

        vTaskDelayUntil(&tick_start, pdMS_TO_TICKS(BEACON_RATE_MS));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_softap();
    espnow_init();
    xTaskCreate(beacon_task, "beacon", 4096, NULL, 5, NULL);
}
