#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_SSID_MAX_LENGTH 32

typedef struct {
    uint8_t type;
    uint8_t subtype;
    bool to_ds;
    bool from_ds;
    bool protected_frame;
    bool is_beacon;
    bool has_ssid;
    bool has_bssid;
    bool has_source;
    bool has_destination;
    uint8_t ssid_length;
    uint8_t ap_channel; // DS Parameters, or HT Operation if DS is absent.
    uint8_t ssid[FRAME_SSID_MAX_LENGTH];
    uint8_t bssid[6];
    uint8_t source[6];
    uint8_t destination[6];
} frame_info_t;

// length excludes the four-byte FCS. No payload bytes are retained.
bool parse_80211_frame(const uint8_t *frame, size_t length, frame_info_t *info);
