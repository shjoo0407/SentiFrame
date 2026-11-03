/* SPDX-License-Identifier: MIT */
#ifndef SF_CORE_H
#define SF_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_PAYLOAD_MAX 96u
#define SF_HEADER_SIZE 18u
#define SF_FRAME_MAX (SF_HEADER_SIZE + SF_PAYLOAD_MAX + 2u)
#define SF_HISTORY_SIZE 200u
#define SF_PENDING_MAX 8u
#define SF_USB_LEASE_MS 1500u
#define SF_RETRY_MS 1000u

enum sf_type { SF_HELLO=1, SF_HEARTBEAT=2, SF_ACK=3,
               SF_IMU=16, SF_PPG=17, SF_STATUS=18, SF_EVENT=19,
               SF_HISTORY=20 };
enum sf_event { SF_IMPACT=1, SF_FALL_SUSPECTED=2, SF_SOS=3, SF_SENSOR_FAULT=4 };
enum sf_transport { SF_NO_LINK, SF_USB, SF_BLE };
enum sf_flags { SF_GYRO_VALID=1, SF_SATURATED=2, SF_TIME_ESTIMATED=4,
                SF_DATA_GAP=8 };
struct sf_packet {
    uint8_t type;
    uint16_t length;
    uint32_t seq, time_ms, boot;
    uint8_t payload[SF_PAYLOAD_MAX];
};
struct sf_parser { uint8_t bytes[SF_FRAME_MAX]; size_t used; uint32_t rejected; };
uint16_t sf_crc16(const uint8_t *data, size_t len);
size_t sf_encode(const struct sf_packet *p, uint8_t *out, size_t cap);
bool sf_parser_push(struct sf_parser *p, uint8_t byte, struct sf_packet *out);
void sf_put16(uint8_t *out, uint16_t x);
void sf_put32(uint8_t *out, uint32_t x);
uint16_t sf_get16(const uint8_t *in);
uint32_t sf_get32(const uint8_t *in);

struct sf_link {
    uint32_t boot, usb_time, usb_seq;
    bool usb_seen;
};
bool sf_link_heartbeat(struct sf_link *l, const struct sf_packet *p,
                       uint32_t now, bool physical_usb_ready);
bool sf_link_hello(struct sf_link *l, const struct sf_packet *p, bool physical_usb_ready);
enum sf_transport sf_link_select(const struct sf_link *l, uint32_t now,
                                 bool physical_usb_ready, bool ble_ready);
struct sf_pending { struct sf_packet packet; uint32_t last_try; bool sent; };
struct sf_reliable {
    struct sf_pending items[SF_PENDING_MAX];
    size_t count;
    uint32_t overflow;
};
bool sf_reliable_add(struct sf_reliable *q, const struct sf_packet *p);
const struct sf_packet *sf_reliable_due(struct sf_reliable *q, uint32_t now);
bool sf_reliable_ack(struct sf_reliable *q, uint32_t boot, uint32_t seq);

struct sf_bus {
    void *ctx;
    int (*read)(void *, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
    int (*write)(void *, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);
    void (*delay_ms)(void *, uint32_t ms);
};
enum sf_source { SF_SRC_NONE, SF_SRC_UNKNOWN, SF_SRC_SDP, SF_SRC_CDP,
                 SF_SRC_DCP, SF_SRC_TYPEC_1A5, SF_SRC_TYPEC_3A };
struct sf_power_input {
    bool present, awake, allow, high, switch_open;
    bool cc_valid, pack_approved, usb_alive;
    bool charging_requested; /* Independent feature switch; zero is fail-closed. */
    uint8_t cc_attached, cc_current, cc_accessory;
    int32_t battery_mv; /* -1 means ADC failure, never a fake zero-percent */
};
struct sf_charger {
    struct sf_bus bus;
    void (*gate)(void *, bool);
    void *gate_ctx;
    bool initialized, enabled, fault;
    enum sf_source stable_source;
    uint32_t stable_since;
    unsigned stable_count;
    uint8_t stat0, stat1, flags;
};
enum sf_source sf_classify_source(const struct sf_power_input *in);
int sf_charger_init(struct sf_charger *c, const struct sf_bus *bus,
                    void (*gate)(void *, bool), void *ctx);
int sf_charger_step(struct sf_charger *c, const struct sf_power_input *in,
                    uint32_t now);
void sf_charger_stop(struct sf_charger *c);
int sf_cc_read(const struct sf_bus *bus, struct sf_power_input *in);

struct sf_ppg_word { uint32_t value; uint8_t tag; bool saturated; };
int sf_ppg_init(const struct sf_bus *bus);
int sf_ppg_read(const struct sf_bus *bus, struct sf_ppg_word *out, size_t cap,
                size_t *count, bool *gap);

struct sf_sample { int16_t a[3], g[3]; uint32_t time_ms; uint16_t flags; };
struct sf_fall {
    struct sf_sample history[SF_HISTORY_SIZE];
    float magnitude[SF_HISTORY_SIZE];
    size_t pos, count;
    double sum, sum2;
    float baseline[3], gravity[3], peak_g, last_z;
    uint32_t last_time, impact_time, still_time;
    bool pending, still;
};
void sf_fall_reset(struct sf_fall *f);
enum sf_event sf_fall_push(struct sf_fall *f, const struct sf_sample *sample);
size_t sf_fall_history(const struct sf_fall *f, struct sf_sample *out, size_t cap);
#endif
