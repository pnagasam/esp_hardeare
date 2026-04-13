#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "rx";

#define AP_SSID     "esp-beacon"
#define BEACON_PORT 5000

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

static void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    (void)buf;
    (void)type;
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

static void udp_recv_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    char buf[64];
    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len > 0) {
          printf("Got UDP Beacon\n");
            beacon_count++;
        }
    }
}

static void wifi_init_sta(void) {
    wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

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
    wifi_config_t ap = {
        .ap = {
            .ssid           = "esp-rx-dummy",
            .ssid_len       = strlen("esp-rx-dummy"),
            .channel        = 1,
            .max_connection = 1,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
                     | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_csi_config_t csi_cfg = {0};
    csi_cfg.lltf_en           = true;
    csi_cfg.htltf_en          = true;
    csi_cfg.stbc_htltf2_en    = true;
    csi_cfg.ltf_merge_en      = false;
    csi_cfg.channel_filter_en = true;
    csi_cfg.manu_scale        = false;
    csi_cfg.shift             = 0;
    csi_cfg.dump_ack_en       = false;

    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI armed; connecting to %s...", AP_SSID);

    xEventGroupWaitBits(wifi_evt, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "connected");
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
    xTaskCreate(udp_recv_task,  "udp_rx",    4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "hb",        2048, NULL, 3, NULL);
}
