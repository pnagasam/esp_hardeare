#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

#define AP_SSID     "priyanka incorporated"
#define AP_CHAN     11
#define BEACON_PORT 5000
#define CSI_QUEUE_LEN 20
#define CSI_MAX_LEN   384

static uint32_t csi_count    = 0;
static uint32_t beacon_count = 0;

typedef struct {
    int64_t  ts;
    int8_t   rssi;
    uint8_t  rate;
    uint8_t  sig_mode;
    uint8_t  cwb;
    uint8_t  mcs;
    uint8_t  channel;
    uint16_t len;
    uint8_t  mac[6];
    int8_t   buf[CSI_MAX_LEN];
} csi_queued_t;

static QueueHandle_t csi_queue;

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
    csi_queued_t item;
    item.ts       = info->rx_ctrl.timestamp;
    item.rssi     = info->rx_ctrl.rssi;
    item.rate     = info->rx_ctrl.rate;
    item.sig_mode = info->rx_ctrl.sig_mode;
    item.cwb      = info->rx_ctrl.cwb;
    item.mcs      = info->rx_ctrl.mcs;
    item.channel  = info->rx_ctrl.channel;
    item.len      = (info->len > CSI_MAX_LEN) ? CSI_MAX_LEN : info->len;
    memcpy(item.mac, info->mac, 6);
    memcpy(item.buf, info->buf, item.len);
    /* non-blocking: drop if queue is full rather than stalling Wi-Fi task */
    xQueueSend(csi_queue, &item, 0);
    csi_count++;
}

static void csi_print_task(void *arg) {
    /* stack-allocated buffer big enough for header + worst-case CSI values */
    static char line[CSI_MAX_LEN * 5 + 200];
    csi_queued_t item;
    while (1) {
        if (xQueueReceive(csi_queue, &item, portMAX_DELAY) == pdTRUE) {
            int pos = snprintf(line, sizeof(line),
                "CSI,%lld,%d,%d,%d,%d,%d,%d,%u,%u,"
                "%02X:%02X:%02X:%02X:%02X:%02X,",
                item.ts, item.rssi, item.rate,
                item.sig_mode, item.cwb,
                item.mcs, item.channel, item.len, item.len / 2,
                item.mac[0], item.mac[1], item.mac[2],
                item.mac[3], item.mac[4], item.mac[5]);
            for (uint16_t i = 0; i < item.len; i++) {
                pos += snprintf(line + pos, sizeof(line) - pos,
				"%02X%c", (uint8_t)item.buf[i], (i + 1 == item.len) ? '\n' : ' ');
            }
            fputs(line, stdout);
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(AP_CHAN, WIFI_SECOND_CHAN_BELOW));
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
    csi_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_queued_t));
    wifi_init_ap();
    xTaskCreate(csi_print_task, "csi_pr", 8192, NULL, 4, NULL);
    xTaskCreate(heartbeat_task, "hb",     4096, NULL, 3, NULL);
}
