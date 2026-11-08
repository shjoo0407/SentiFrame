/* SPDX-License-Identifier: MIT */
#ifndef SF_RX_H
#define SF_RX_H
#include "sf_core.h"

/* At 115200 8N1 USB holds ~355ms of a continuous byte stream. This is
 * bounded buffering, not a guarantee against an indefinitely stalled main. */
#define SF_RX_USB_CAPACITY 4096u
#define SF_RX_BLE_CAPACITY 1024u

struct sf_rx_chunk {
    enum sf_transport transport;
    uint8_t length;
    bool gap; /* Reset this transport's parser BEFORE bytes, even if length=0. */
    uint8_t bytes[SF_FRAME_MAX];
};
struct sf_rx_lane { size_t head, size; bool gap; };
struct sf_rx_buffer {
    struct sf_rx_lane usb, ble;
    uint8_t usb_bytes[SF_RX_USB_CAPACITY], ble_bytes[SF_RX_BLE_CAPACITY];
    bool prefer_ble;
    uint32_t dropped_bytes, gaps; /* saturate instead of wrapping to zero */
};

/* A zero-initialized buffer is ready to use. The production wrapper MUST
 * serialize every operation (including counters) with its ISR-safe spinlock.
 * Enqueue copies bytes; pop copies owned output. No caller pointer is retained. */
bool sf_rx_buffer_enqueue(struct sf_rx_buffer *q, enum sf_transport src,
                          const uint8_t *bytes, size_t length);
bool sf_rx_buffer_pop(struct sf_rx_buffer *q, struct sf_rx_chunk *out);
/* Drop pending bytes for one transport, count them + known lost bytes, and
 * emit a gap before subsequently retained data. Other transport is untouched.
 * Hardware error losses may be unknown: the wrapper counts at least one. */
void sf_rx_buffer_gap(struct sf_rx_buffer *q, enum sf_transport src, size_t lost);
/* Production ISR-safe wrapper: discard queued bytes and reset the parser at
 * a link lifecycle boundary. The zero-length gap is observable immediately. */
void sf_rx_reset(enum sf_transport src);
#endif
