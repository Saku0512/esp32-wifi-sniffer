#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame_parser.h"

static void test_beacon(void)
{
    uint8_t frame[60] = {0};
    frame[0] = 0x80;
    memset(frame + 16, 0x33, 6);
    // Put HT Operation before DS Parameters to verify DS has precedence.
    const uint8_t ies[] = {
        61, 1, 7,
        0, 3, 'L', 'a', 'b',
        3, 1, 6,
    };
    memcpy(frame + 36, ies, sizeof(ies));

    frame_info_t info;
    assert(parse_80211_frame(frame, 36 + sizeof(ies), &info));
    assert(info.type == 0 && info.subtype == 8 && info.is_beacon);
    assert(info.has_bssid && info.bssid[0] == 0x33);
    assert(info.has_ssid && info.ssid_length == 3);
    assert(memcmp(info.ssid, "Lab", 3) == 0);
    assert(info.ap_channel == 6);

    const uint8_t hidden[] = {0, 0};
    memcpy(frame + 36, hidden, sizeof(hidden));
    assert(parse_80211_frame(frame, 38, &info));
    assert(info.has_ssid && info.ssid_length == 0);
    assert(info.ap_channel == 0);

    const uint8_t malformed[] = {0, 10, 'x'};
    memcpy(frame + 36, malformed, sizeof(malformed));
    assert(!parse_80211_frame(frame, 39, &info));
    assert(!parse_80211_frame(frame, 35, &info));
}

static void test_data_addresses(void)
{
    uint8_t frame[30] = {0};
    frame[0] = 0x08;
    memset(frame + 4, 0x11, 6);  // Address 1
    memset(frame + 10, 0x22, 6); // Address 2
    memset(frame + 16, 0x33, 6); // Address 3
    memset(frame + 24, 0x44, 6); // Address 4
    frame_info_t info;

    frame[1] = 0;
    assert(parse_80211_frame(frame, 24, &info));
    assert(info.destination[0] == 0x11 && info.source[0] == 0x22);
    assert(info.has_bssid && info.bssid[0] == 0x33);

    frame[1] = 1; // To DS
    assert(parse_80211_frame(frame, 24, &info));
    assert(info.destination[0] == 0x33 && info.source[0] == 0x22);
    assert(info.has_bssid && info.bssid[0] == 0x11);

    frame[1] = 2; // From DS
    assert(parse_80211_frame(frame, 24, &info));
    assert(info.destination[0] == 0x11 && info.source[0] == 0x33);
    assert(info.has_bssid && info.bssid[0] == 0x22);

    frame[1] = 0x43; // Four addresses and Protected bit.
    assert(!parse_80211_frame(frame, 24, &info));
    assert(parse_80211_frame(frame, 30, &info));
    assert(info.destination[0] == 0x33 && info.source[0] == 0x44);
    assert(!info.has_bssid && info.protected_frame);
}

static void test_control(void)
{
    const uint8_t ack[] = {0xd4, 0x00};
    frame_info_t info;
    assert(parse_80211_frame(ack, sizeof(ack), &info));
    assert(info.type == 1 && info.subtype == 13);
    assert(!info.has_source && !info.has_destination);
}

int main(void)
{
    test_beacon();
    test_data_addresses();
    test_control();
    puts("frame_parser_test: OK");
    return 0;
}
