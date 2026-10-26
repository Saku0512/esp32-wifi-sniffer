#include "frame_parser.h"

#include <string.h>

#define MAC_HEADER_LENGTH 24
#define FOUR_ADDRESS_HEADER_LENGTH 30
#define BEACON_IES_OFFSET 36
#define SSID_ELEMENT_ID 0
#define DS_PARAMS_ELEMENT_ID 3
#define HT_OPERATION_ELEMENT_ID 61

static void copy_mac(uint8_t target[6], const uint8_t *source)
{
    memcpy(target, source, 6);
}

bool parse_80211_frame(const uint8_t *frame, size_t length, frame_info_t *info)
{
    if (frame == NULL || info == NULL || length < 2 || (frame[0] & 0x03) != 0) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->type = (frame[0] >> 2) & 0x03;
    info->subtype = frame[0] >> 4;
    info->to_ds = (frame[1] & 0x01) != 0;
    info->from_ds = (frame[1] & 0x02) != 0;
    info->protected_frame = (frame[1] & 0x40) != 0;

    if (info->type > 2) {
        return false;
    }
    if (info->type == 1) {
        // Control subtypes have different header layouts. Only classify them.
        return true;
    }
    if (length < MAC_HEADER_LENGTH) {
        return false;
    }

    const uint8_t *address1 = frame + 4;
    const uint8_t *address2 = frame + 10;
    const uint8_t *address3 = frame + 16;

    if (info->type == 0) {
        copy_mac(info->destination, address1);
        copy_mac(info->source, address2);
        copy_mac(info->bssid, address3);
        info->has_destination = true;
        info->has_source = true;
        info->has_bssid = true;

        info->is_beacon = info->subtype == 8;
        if (!info->is_beacon) {
            return true;
        }
        if (length < BEACON_IES_OFFSET) {
            return false;
        }

        bool has_ds_channel = false;
        for (size_t offset = BEACON_IES_OFFSET; offset + 2 <= length;) {
            const uint8_t element_id = frame[offset];
            const uint8_t element_length = frame[offset + 1];
            offset += 2;
            if (element_length > length - offset) {
                return false;
            }
            if (element_id == SSID_ELEMENT_ID) {
                if (element_length > FRAME_SSID_MAX_LENGTH) {
                    return false;
                }
                info->has_ssid = true;
                info->ssid_length = element_length;
                memcpy(info->ssid, frame + offset, element_length);
            } else if (element_id == DS_PARAMS_ELEMENT_ID && element_length == 1) {
                const uint8_t channel = frame[offset];
                if (channel >= 1 && channel <= 14) {
                    info->ap_channel = channel;
                    has_ds_channel = true;
                }
            } else if (element_id == HT_OPERATION_ELEMENT_ID && element_length >= 1 &&
                       !has_ds_channel) {
                const uint8_t channel = frame[offset];
                if (channel >= 1 && channel <= 14) {
                    info->ap_channel = channel;
                }
            }
            offset += element_length;
        }
        return true;
    }

    // Data address roles depend on the two Distribution System direction bits.
    if (info->to_ds && info->from_ds) {
        if (length < FOUR_ADDRESS_HEADER_LENGTH) {
            return false;
        }
        copy_mac(info->destination, address3);
        copy_mac(info->source, frame + 24);
    } else if (info->to_ds) {
        copy_mac(info->bssid, address1);
        copy_mac(info->source, address2);
        copy_mac(info->destination, address3);
        info->has_bssid = true;
    } else if (info->from_ds) {
        copy_mac(info->destination, address1);
        copy_mac(info->bssid, address2);
        copy_mac(info->source, address3);
        info->has_bssid = true;
    } else {
        copy_mac(info->destination, address1);
        copy_mac(info->source, address2);
        copy_mac(info->bssid, address3);
        info->has_bssid = true;
    }
    info->has_destination = true;
    info->has_source = true;
    return true;
}
