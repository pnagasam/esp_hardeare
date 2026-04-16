#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "rx";

#define AP_SSID     "priyanka wifi"
#define AP_CHAN     1
#define BEACON_PORT 5000

static uint32_t csi_count    = 0;
static uint32_t beacon_count = 0;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "STA connected: " MACSTR, MAC2STR(evt->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "STA disconnected: " MACSTR, MAC2STR(evt->mac));
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
    printf("CSI,%lld,%d,%d,%d,%d,%d,%d,%u,"
           "%02X:%02X:%02X:%02X:%02X:%02X,",
           ts, info->rx_ctrl.rssi, info->rx_ctrl.rate,
           info->rx_ctrl.sig_mode, info->rx_ctrl.cwb,
           info->rx_ctrl.mcs, info->rx_ctrl.channel, len,
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
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = '\0';
            printf("Got UDP Beacon: %s\n", buf);
            beacon_count++;
        }
    }
}

static void wifi_init_ap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

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

    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_set_channel(AP_CHAN, WIFI_SECOND_CHAN_ABOVE));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

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

    ESP_LOGI(TAG, "softAP '%s' up on ch %d (HT40+)", AP_SSID, AP_CHAN);
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
    wifi_init_ap();
    xTaskCreate(udp_recv_task,  "udp_rx", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "hb",     2048, NULL, 3, NULL);
}
