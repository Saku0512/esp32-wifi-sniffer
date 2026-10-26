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
#include "frame_parser.h"

#define FIRST_CHANNEL 1
#define LAST_CHANNEL 11
#define CHANNEL_DWELL_MS 700
#define REPORT_INTERVAL_MS 60000
#define FRAME_QUEUE_LENGTH 128
#define MAX_OBSERVED_BSSIDS 256
#define MAX_BEACON_SAMPLES 48
#define FCS_LENGTH 4

typedef struct {
    frame_info_t frame;
    uint8_t rx_channel;
    int8_t rssi;
} packet_event_t;

typedef struct {
    uint32_t total;
    uint32_t by_type[3];
    uint32_t beacons;
    uint32_t unique_bssids;
    uint32_t dwell_ms;
} channel_stats_t;

typedef struct {
    uint8_t bssid[6];
    uint16_t seen_rx_channels;
} observed_bssid_t;

static QueueHandle_t frame_queue;
static uint32_t queue_drops;
static channel_stats_t channels[LAST_CHANNEL + 1];
static observed_bssid_t observed_bssids[MAX_OBSERVED_BSSIDS];
static size_t observed_bssid_count;
static bool bssid_table_full;
static uint32_t subtype_counts[3][16];
static uint32_t type_counts[3];
static uint32_t rssi_bins[8];
static uint32_t processed_frames;
static uint32_t sampled_beacons;
static bool sampled_data_direction[4];

static void on_packet(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_CTRL && type != WIFI_PKT_DATA) {
        return;
    }

    const wifi_promiscuous_pkt_t *packet = buffer;
    const size_t frame_length = packet->rx_ctrl.sig_len;
    if (frame_length < FCS_LENGTH + 2) {
        return;
    }

    packet_event_t event = {
        .rx_channel = packet->rx_ctrl.channel,
        .rssi = packet->rx_ctrl.rssi,
    };
    if (!parse_80211_frame(packet->payload, frame_length - FCS_LENGTH, &event.frame) ||
        event.frame.type != (uint8_t)type) {
        return;
    }

    // Only bounded metadata is queued; data payload is never copied.
    if (xQueueSend(frame_queue, &event, 0) != pdTRUE) {
        __atomic_fetch_add(&queue_drops, 1, __ATOMIC_RELAXED);
    }
}

static void format_ssid(const frame_info_t *frame, char *output, size_t capacity)
{
    if (!frame->has_ssid) {
        snprintf(output, capacity, "<missing>");
        return;
    }

    bool hidden = frame->ssid_length == 0;
    if (!hidden) {
        hidden = true;
        for (size_t i = 0; i < frame->ssid_length; ++i) {
            if (frame->ssid[i] != 0) {
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
    for (size_t i = 0; i < frame->ssid_length; ++i) {
        const uint8_t byte = frame->ssid[i];
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
            output[used++] = (char)byte;
        } else {
            used += (size_t)snprintf(output + used, capacity - used, "\\x%02X", byte);
        }
    }
    output[used] = '\0';
}

static const char *type_name(uint8_t type)
{
    static const char *const names[] = {"Management", "Control", "Data"};
    return names[type];
}

static const char *subtype_name(uint8_t type, uint8_t subtype)
{
    if (type == 0) {
        switch (subtype) {
        case 4: return "Probe Request";
        case 5: return "Probe Response";
        case 8: return "Beacon";
        default: return "Other";
        }
    }
    if (type == 1) {
        switch (subtype) {
        case 8: return "Block Ack Request";
        case 9: return "Block Ack";
        case 11: return "RTS";
        case 12: return "CTS";
        case 13: return "ACK";
        default: return "Other";
        }
    }
    switch (subtype) {
    case 0: return "Data";
    case 4: return "Null Data";
    case 8: return "QoS Data";
    case 12: return "QoS Null";
    default: return "Other";
    }
}

static unsigned rssi_bucket(int8_t rssi)
{
    if (rssi < -90) {
        return 0;
    }
    if (rssi >= -30) {
        return 7;
    }
    return 1 + (unsigned)(rssi + 90) / 10;
}

static void print_beacon(const packet_event_t *event)
{
    char ssid[FRAME_SSID_MAX_LENGTH * 4 + 1];
    char ap_channel[4];
    format_ssid(&event->frame, ssid, sizeof(ssid));
    if (event->frame.ap_channel == 0) {
        snprintf(ap_channel, sizeof(ap_channel), "-");
    } else {
        snprintf(ap_channel, sizeof(ap_channel), "%u",
                 (unsigned)event->frame.ap_channel);
    }
    const uint8_t *bssid = event->frame.bssid;
    printf("[RX CH %2u] [AP CH %2s] BEACON RSSI:%d BSSID:%02X:%02X:%02X:%02X:%02X:%02X SSID:%s\n",
           (unsigned)event->rx_channel, ap_channel, (int)event->rssi,
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ssid);
}

static void record_beacon(const packet_event_t *event)
{
    if (event->rx_channel < FIRST_CHANNEL || event->rx_channel > LAST_CHANNEL) {
        return;
    }

    const uint8_t *bssid = event->frame.bssid;
    size_t index = 0;
    while (index < observed_bssid_count &&
           memcmp(observed_bssids[index].bssid, bssid, 6) != 0) {
        ++index;
    }
    if (index == observed_bssid_count) {
        if (observed_bssid_count == MAX_OBSERVED_BSSIDS) {
            bssid_table_full = true;
            return;
        }
        memcpy(observed_bssids[index].bssid, bssid, 6);
        observed_bssids[index].seen_rx_channels = 0;
        ++observed_bssid_count;
    }

    const uint16_t mask = (uint16_t)(1u << event->rx_channel);
    if ((observed_bssids[index].seen_rx_channels & mask) == 0) {
        observed_bssids[index].seen_rx_channels |= mask;
        ++channels[event->rx_channel].unique_bssids;
        if (sampled_beacons < MAX_BEACON_SAMPLES) {
            print_beacon(event);
            ++sampled_beacons;
        }
    }
}

static void print_data_address_example(const frame_info_t *frame)
{
    const uint8_t *source = frame->source;
    const uint8_t *destination = frame->destination;
    printf("DATA MAP ToDS:%u FromDS:%u Protected:%u SRC:%02X:%02X:%02X:%02X:%02X:%02X DST:%02X:%02X:%02X:%02X:%02X:%02X\n",
           (unsigned)frame->to_ds, (unsigned)frame->from_ds,
           (unsigned)frame->protected_frame,
           source[0], source[1], source[2], source[3], source[4], source[5],
           destination[0], destination[1], destination[2],
           destination[3], destination[4], destination[5]);
}

static void record_packet(const packet_event_t *event)
{
    const frame_info_t *frame = &event->frame;
    ++processed_frames;
    ++type_counts[frame->type];
    ++rssi_bins[rssi_bucket(event->rssi)];
    if (++subtype_counts[frame->type][frame->subtype] == 1) {
        printf("FIRST %s subtype:%u (%s)\n", type_name(frame->type),
               (unsigned)frame->subtype,
               subtype_name(frame->type, frame->subtype));
    }

    if (event->rx_channel >= FIRST_CHANNEL && event->rx_channel <= LAST_CHANNEL) {
        channel_stats_t *stats = &channels[event->rx_channel];
        ++stats->total;
        ++stats->by_type[frame->type];
        if (frame->is_beacon) {
            ++stats->beacons;
        }
    }

    if (frame->is_beacon && frame->has_bssid) {
        record_beacon(event);
    }
    if (frame->type == 2 && frame->has_source && frame->has_destination) {
        const unsigned direction = (unsigned)frame->to_ds +
                                   2u * (unsigned)frame->from_ds;
        if (!sampled_data_direction[direction]) {
            sampled_data_direction[direction] = true;
            print_data_address_example(frame);
        }
    }
}

static void print_summary(uint32_t elapsed_ms, uint8_t current_channel,
                          uint32_t current_channel_ms)
{
    const uint32_t dropped = __atomic_load_n(&queue_drops, __ATOMIC_RELAXED);
    printf("\n=== SUMMARY elapsed_s=%lu processed=%lu queue_dropped=%lu unique_bssids=%u bssid_table_full=%u ===\n",
           (unsigned long)(elapsed_ms / 1000), (unsigned long)processed_frames,
           (unsigned long)dropped, (unsigned)observed_bssid_count,
           (unsigned)bssid_table_full);
    printf("TYPE Management=%lu Control=%lu Data=%lu\n",
           (unsigned long)type_counts[0], (unsigned long)type_counts[1],
           (unsigned long)type_counts[2]);
    printf("SUBTYPE Beacon=%lu ProbeReq=%lu ProbeResp=%lu ACK=%lu RTS=%lu CTS=%lu Data=%lu QoSData=%lu\n",
           (unsigned long)subtype_counts[0][8],
           (unsigned long)subtype_counts[0][4],
           (unsigned long)subtype_counts[0][5],
           (unsigned long)subtype_counts[1][13],
           (unsigned long)subtype_counts[1][11],
           (unsigned long)subtype_counts[1][12],
           (unsigned long)subtype_counts[2][0],
           (unsigned long)subtype_counts[2][8]);

    for (uint8_t channel = FIRST_CHANNEL; channel <= LAST_CHANNEL; ++channel) {
        const channel_stats_t *stats = &channels[channel];
        const uint32_t dwell_ms = stats->dwell_ms +
                                  (channel == current_channel ? current_channel_ms : 0);
        printf("CH %2u dwell_ms=%lu total=%lu mgmt=%lu ctrl=%lu data=%lu beacon=%lu bssids=%lu\n",
               (unsigned)channel, (unsigned long)dwell_ms,
               (unsigned long)stats->total,
               (unsigned long)stats->by_type[0],
               (unsigned long)stats->by_type[1],
               (unsigned long)stats->by_type[2],
               (unsigned long)stats->beacons,
               (unsigned long)stats->unique_bssids);
    }
    static const char *const labels[] = {
        "<-90", "-90..-81", "-80..-71", "-70..-61",
        "-60..-51", "-50..-41", "-40..-31", ">=-30",
    };
    for (unsigned i = 0; i < 8; ++i) {
        printf("RSSI %s dBm=%lu\n", labels[i], (unsigned long)rssi_bins[i]);
    }
    printf("=== END SUMMARY ===\n\n");
}

void app_main(void)
{
    frame_queue = xQueueCreate(FRAME_QUEUE_LENGTH, sizeof(packet_event_t));
    if (frame_queue == NULL) {
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
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_CTRL |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    wifi_promiscuous_filter_t control_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_ctrl_filter(&control_filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(on_packet));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    printf("Listening for Management/Control/Data on channels %d-%d; summary every %d s (no AP connection)\n",
           FIRST_CHANNEL, LAST_CHANNEL, REPORT_INTERVAL_MS / 1000);

    uint8_t channel = FIRST_CHANNEL;
    const TickType_t started = xTaskGetTickCount();
    TickType_t last_switch = started;
    TickType_t last_report = started;
    for (;;) {
        packet_event_t event;
        if (xQueueReceive(frame_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            record_packet(&event);
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_switch >= pdMS_TO_TICKS(CHANNEL_DWELL_MS)) {
            channels[channel].dwell_ms +=
                (uint32_t)(now - last_switch) * portTICK_PERIOD_MS;
            channel = channel == LAST_CHANNEL ? FIRST_CHANNEL : channel + 1;
            ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
            last_switch = now;
        }
        if (now - last_report >= pdMS_TO_TICKS(REPORT_INTERVAL_MS)) {
            print_summary((uint32_t)(now - started) * portTICK_PERIOD_MS,
                          channel,
                          (uint32_t)(now - last_switch) * portTICK_PERIOD_MS);
            last_report = now;
        }
    }
}
