#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "tx";

#define AP_SSID        "priyanka wifi"
#define BEACON_RATE_MS 50
#define BEACON_PORT    5000
#define BEACON_PAYLOAD "beacon test"

static EventGroupHandle_t wifi_evt;
#define CONNECTED_BIT BIT0

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_evt, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(wifi_evt, CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t sta = {
        .sta = {
            .ssid               = AP_SSID,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .scan_method        = WIFI_ALL_CHANNEL_SCAN,
            .sort_method        = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "STA connecting to '%s'", AP_SSID);
}

static void beacon_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int bcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast)) < 0) {
        ESP_LOGE(TAG, "set SO_BROADCAST failed: %d", errno);
    }

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    TickType_t tick_start = xTaskGetTickCount();
    while (1) {
        if (xEventGroupGetBits(wifi_evt) & CONNECTED_BIT) {
            int ret = sendto(sock, BEACON_PAYLOAD, strlen(BEACON_PAYLOAD), 0,
                             (struct sockaddr *)&dest, sizeof(dest));
            if (ret < 0) {
                ESP_LOGE(TAG, "sendto failed: %d", errno);
            } else {
              ESP_LOGI(TAG, "I sent a packet...");
            }
        }
        vTaskDelayUntil(&tick_start, pdMS_TO_TICKS(BEACON_RATE_MS));
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    xTaskCreate(beacon_task, "beacon", 4096, NULL, 5, NULL);
}
