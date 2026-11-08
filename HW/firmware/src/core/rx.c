/* SPDX-License-Identifier: MIT */
#include "sf_rx.h"
#include <string.h>

static void add_saturated(uint32_t *counter, size_t amount) {
    if (amount>UINT32_MAX-*counter) *counter=UINT32_MAX;
    else *counter+=(uint32_t)amount;
}
static struct sf_rx_lane *lane(struct sf_rx_buffer *q, enum sf_transport src) {
    return src==SF_USB?&q->usb:(src==SF_BLE?&q->ble:NULL);
}
static uint8_t *storage(struct sf_rx_buffer *q, enum sf_transport src) {
    return src==SF_USB?q->usb_bytes:q->ble_bytes;
}
static size_t capacity(enum sf_transport src) {
    return src==SF_USB?SF_RX_USB_CAPACITY:SF_RX_BLE_CAPACITY;
}
void sf_rx_buffer_gap(struct sf_rx_buffer *q, enum sf_transport src, size_t lost) {
    if (!q) return;
    struct sf_rx_lane *l=lane(q,src);
    add_saturated(&q->dropped_bytes,lost);
    if (!l) return;
    add_saturated(&q->dropped_bytes,l->size);
    add_saturated(&q->gaps,1);
    l->head=0; l->size=0; l->gap=true;
}
bool sf_rx_buffer_enqueue(struct sf_rx_buffer *q, enum sf_transport src,
                          const uint8_t *bytes, size_t length) {
    if (!q) return false;
    struct sf_rx_lane *l=lane(q,src);
    if (!length) return false; /* an empty write creates no missing bytes */
    if (!l||!bytes||length>capacity(src)-l->size) {
        /* Keep no ambiguous prefix across an overrun. Reject the whole write
         * and discard only that transport's queued bytes. Main will see a
         * gap before any new bytes, including if it had popped a prefix. */
        sf_rx_buffer_gap(q,src,length); return false;
    }
    size_t cap=capacity(src), tail=(l->head+l->size)%cap;
    size_t first=cap-tail; if (first>length) first=length;
    memcpy(storage(q,src)+tail,bytes,first);
    memcpy(storage(q,src),bytes+first,length-first);
    l->size+=length;
    return true;
}
static bool ready(const struct sf_rx_lane *l) { return l->size||l->gap; }
bool sf_rx_buffer_pop(struct sf_rx_buffer *q, struct sf_rx_chunk *out) {
    if (!q||!out) return false;
    enum sf_transport first=q->prefer_ble?SF_BLE:SF_USB;
    enum sf_transport second=q->prefer_ble?SF_USB:SF_BLE;
    enum sf_transport src=ready(lane(q,first))?first:second;
    struct sf_rx_lane *l=lane(q,src);
    if (!ready(l)) return false;
    *out=(struct sf_rx_chunk){.transport=src,.gap=l->gap};
    l->gap=false;
    size_t length=l->size; if (length>SF_FRAME_MAX) length=SF_FRAME_MAX;
    size_t cap=capacity(src), first_bytes=cap-l->head;
    if (first_bytes>length) first_bytes=length;
    memcpy(out->bytes,storage(q,src)+l->head,first_bytes);
    memcpy(out->bytes+first_bytes,storage(q,src),length-first_bytes);
    out->length=(uint8_t)length;
    l->head=(l->head+length)%cap; l->size-=length;
    q->prefer_ble=src==SF_USB;
    return true;
}
