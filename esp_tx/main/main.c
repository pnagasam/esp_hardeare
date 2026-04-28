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
#include "led_strip.h"

static const char *TAG = "tx";

#define WS2812_GPIO 48

#define AP_SSID        "priyanka incorporated"
#define BEACON_PORT    5000
#define BEACON_PAYLOAD "beacon test"

static EventGroupHandle_t wifi_evt;
#define CONNECTED_BIT BIT0

#define BEACON_RATE_MS 5   // 500 Hz

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
        /* re-assert HT40 after association so it isn't overridden */
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
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

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BEACON_RATE_MS));

        if (xEventGroupGetBits(wifi_evt) & CONNECTED_BIT) {
            int ret = sendto(sock, BEACON_PAYLOAD, strlen(BEACON_PAYLOAD), 0,
                             (struct sockaddr *)&dest, sizeof(dest));

            if (ret < 0) {
                ESP_LOGE(TAG, "sendto failed: %d", errno);
            } else {
                ESP_LOGI(TAG, "packet sent at %lu ms", xTaskGetTickCount() * portTICK_PERIOD_MS);
            }
        }
    }
}

static void init_led(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
    };
    led_strip_handle_t led;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led));

    // TX = ORANGE
    ESP_ERROR_CHECK(led_strip_set_pixel(led, 0, 255, 80, 0));
    ESP_ERROR_CHECK(led_strip_refresh(led));
    ESP_LOGI(TAG, "LED set to ORANGE (TX)");
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_led();
    wifi_init_sta();
    xTaskCreate(beacon_task, "beacon", 4096, NULL, 5, NULL);
}
