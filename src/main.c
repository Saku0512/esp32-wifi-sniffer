#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define FIRST_CHANNEL 1
#define LAST_CHANNEL 11
#define CHANNEL_DWELL_MS 700
#define BEACON_QUEUE_LENGTH 24

// 802.11 management header (24 bytes), then beacon fixed fields (12 bytes).
#define BEACON_IES_OFFSET 36
#define FCS_LENGTH 4
#define SSID_MAX_LENGTH 32
#define SSID_ELEMENT_ID 0
#define DS_PARAMS_ELEMENT_ID 3
#define HT_OPERATION_ELEMENT_ID 61

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[SSID_MAX_LENGTH];
    uint8_t ssid_length;
    uint8_t rx_channel;
    uint8_t ap_channel; // Zero when the beacon does not advertise a channel.
    int8_t rssi;
    bool has_ssid;
} beacon_event_t;

static QueueHandle_t beacon_queue;

static void on_packet(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    const wifi_promiscuous_pkt_t *packet = buffer;
    const uint8_t *frame = packet->payload;
    const size_t frame_length = packet->rx_ctrl.sig_len;
    if (frame_length < BEACON_IES_OFFSET + FCS_LENGTH || frame[0] != 0x80) {
        return; // Not a complete 802.11 beacon frame.
    }

    beacon_event_t event = {
        .rx_channel = packet->rx_ctrl.channel,
        .rssi = packet->rx_ctrl.rssi,
    };
    memcpy(event.bssid, frame + 16, sizeof(event.bssid)); // Address 3 is the BSSID.

    // Tagged parameters use [element ID, length, value...]. Exclude the FCS.
    const size_t ie_end = frame_length - FCS_LENGTH;
    bool has_ds_channel = false;
    for (size_t offset = BEACON_IES_OFFSET; offset + 2 <= ie_end;) {
        const uint8_t element_id = frame[offset];
        const uint8_t element_length = frame[offset + 1];
        offset += 2;
        if (element_length > ie_end - offset) {
            return; // Truncated or malformed information element.
        }
        if (element_id == SSID_ELEMENT_ID) {
            if (element_length > SSID_MAX_LENGTH) {
                return;
            }
            event.has_ssid = true;
            event.ssid_length = element_length;
            memcpy(event.ssid, frame + offset, element_length);
        } else if (element_id == DS_PARAMS_ELEMENT_ID && element_length == 1) {
            const uint8_t channel = frame[offset];
            if (channel >= 1 && channel <= 14) {
                event.ap_channel = channel;
                has_ds_channel = true;
            }
        } else if (element_id == HT_OPERATION_ELEMENT_ID && element_length >= 1 &&
                   !has_ds_channel) {
            // HT Operation's first byte is the primary channel. DS Parameters wins.
            const uint8_t channel = frame[offset];
            if (channel >= 1 && channel <= 14) {
                event.ap_channel = channel;
            }
        }
        offset += element_length;
    }

    // Never print from the Wi-Fi driver's receive callback.
    (void)xQueueSend(beacon_queue, &event, 0);
}

static void format_ssid(const beacon_event_t *event, char *output, size_t capacity)
{
    if (!event->has_ssid) {
        snprintf(output, capacity, "<missing>");
        return;
    }

    bool hidden = event->ssid_length == 0;
    if (!hidden) {
        hidden = true;
        for (size_t i = 0; i < event->ssid_length; ++i) {
            if (event->ssid[i] != 0) {
                hidden = false;
                break;
            }
        }
    }
    if (hidden) {
        snprintf(output, capacity, "<hidden>");
        return;
    }

    // Escape control and non-ASCII bytes so SSIDs cannot alter terminal output.
    size_t used = 0;
    for (size_t i = 0; i < event->ssid_length; ++i) {
        const uint8_t byte = event->ssid[i];
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
            output[used++] = (char)byte;
        } else {
            used += (size_t)snprintf(output + used, capacity - used, "\\x%02X", byte);
        }
    }
    output[used] = '\0';
}

void app_main(void)
{
    beacon_queue = xQueueCreate(BEACON_QUEUE_LENGTH, sizeof(beacon_event_t));
    if (beacon_queue == NULL) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(FIRST_CHANNEL, WIFI_SECOND_CHAN_NONE));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(on_packet));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    printf("Listening for beacons on channels %d-%d (no AP connection)\n",
           FIRST_CHANNEL, LAST_CHANNEL);

    uint8_t channel = FIRST_CHANNEL;
    TickType_t last_switch = xTaskGetTickCount();
    for (;;) {
        beacon_event_t event;
        if (xQueueReceive(beacon_queue, &event, pdMS_TO_TICKS(100)) == pdTRUE) {
            char ssid[SSID_MAX_LENGTH * 4 + 1];
            char ap_channel[4];
            format_ssid(&event, ssid, sizeof(ssid));
            if (event.ap_channel == 0) {
                snprintf(ap_channel, sizeof(ap_channel), "-");
            } else {
                snprintf(ap_channel, sizeof(ap_channel), "%u", (unsigned)event.ap_channel);
            }
            printf("[RX CH %2u] [AP CH %2s] BEACON RSSI:%d BSSID:%02X:%02X:%02X:%02X:%02X:%02X SSID:%s\n",
                   (unsigned)event.rx_channel, ap_channel, (int)event.rssi,
                   event.bssid[0], event.bssid[1], event.bssid[2],
                   event.bssid[3], event.bssid[4], event.bssid[5], ssid);
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_switch >= pdMS_TO_TICKS(CHANNEL_DWELL_MS)) {
            channel = channel == LAST_CHANNEL ? FIRST_CHANNEL : channel + 1;
            ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
            last_switch = now;
        }
    }
}
