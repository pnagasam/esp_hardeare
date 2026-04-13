#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "tx";

#define AP_SSID        "esp-beacon"
#define AP_CHAN        1
#define BEACON_RATE_MS 20
#define BEACON_PORT    5000
#define BEACON_PAYLOAD "BEACON"

static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

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
    wifi_config_t sta = { 0 };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "softAP '%s' up on channel %d (HT40)", AP_SSID, AP_CHAN);
}

static void mac_print_task(void *arg) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    while (1) {
        ESP_LOGI(TAG, "AP MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void beacon_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(BEACON_PORT),
        .sin_addr.s_addr = inet_addr("255.255.255.255"),
    };

    while (1) {
        TickType_t tick_start = xTaskGetTickCount();

        int ret = sendto(sock, BEACON_PAYLOAD, sizeof(BEACON_PAYLOAD) - 1, 0,
                         (struct sockaddr *)&dest, sizeof(dest));
        if (ret < 0) {
            ESP_LOGE(TAG, "sendto failed: %d", errno);
        }

        vTaskDelayUntil(&tick_start, pdMS_TO_TICKS(BEACON_RATE_MS));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_softap();
    xTaskCreate(beacon_task,    "beacon",    4096, NULL, 5, NULL);
    xTaskCreate(mac_print_task, "mac_print", 2048, NULL, 3, NULL);
}
