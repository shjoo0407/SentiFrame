/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include <string.h>

void sf_put16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
void sf_put32(uint8_t *p, uint32_t x) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i));
}
uint16_t sf_get16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
uint32_t sf_get32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
uint16_t sf_crc16(const uint8_t *p, size_t n) {
    uint16_t c=0xffff;
    for (size_t i=0;i<n;i++) {
        c^=(uint16_t)p[i]<<8;
        for (unsigned j=0;j<8;j++) c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));
    }
    return c;
}
size_t sf_encode(const struct sf_packet *p, uint8_t *out, size_t cap) {
    if (!p||!out||p->length>SF_PAYLOAD_MAX) return 0;
    size_t n=SF_HEADER_SIZE+p->length+2;
    if (cap<n) return 0;
    out[0]=0xa5; out[1]=0x5a; out[2]=1; out[3]=p->type;
    sf_put16(out+4,p->length); sf_put32(out+6,p->seq);
    sf_put32(out+10,p->time_ms); sf_put32(out+14,p->boot);
    memcpy(out+SF_HEADER_SIZE,p->payload,p->length);
    sf_put16(out+n-2,sf_crc16(out,n-2));
    return n;
}
static void discard(struct sf_parser *p, size_t n) {
    memmove(p->bytes,p->bytes+n,p->used-n); p->used-=n;
}
bool sf_parser_push(struct sf_parser *p, uint8_t b, struct sf_packet *out) {
    if (p->used==sizeof(p->bytes)) { discard(p,1); p->rejected++; }
    p->bytes[p->used++]=b;
    while (p->used>=2) {
        if (p->bytes[0]!=0xa5||p->bytes[1]!=0x5a) { discard(p,1); continue; }
        if (p->used<6) return false;
        size_t len=sf_get16(p->bytes+4);
        if (p->bytes[2]!=1||len>SF_PAYLOAD_MAX) { discard(p,1); p->rejected++; continue; }
        size_t total=SF_HEADER_SIZE+len+2;
        if (p->used<total) return false;
        if (sf_crc16(p->bytes,total-2)!=sf_get16(p->bytes+total-2)) {
            discard(p,1); p->rejected++; continue;
        }
        out->type=p->bytes[3]; out->length=(uint16_t)len;
        out->seq=sf_get32(p->bytes+6); out->time_ms=sf_get32(p->bytes+10);
        out->boot=sf_get32(p->bytes+14);
        memcpy(out->payload,p->bytes+SF_HEADER_SIZE,len);
        discard(p,total);
        return true;
    }
    return false;
}
bool sf_link_hello(struct sf_link *l, const struct sf_packet *p, bool ready) {
    if (!ready||p->type!=SF_HELLO||p->length||(p->boot&&p->boot!=l->boot)) return false;
    l->usb_seen=false; l->usb_seq=0; return true;
}
bool sf_link_heartbeat(struct sf_link *l, const struct sf_packet *p,
                      uint32_t now, bool ready) {
    if (!ready||p->type!=SF_HEARTBEAT||p->length||p->boot!=l->boot) return false;
    if (l->usb_seen&&(int32_t)(p->seq-l->usb_seq)<=0) return false;
    l->usb_seq=p->seq; l->usb_time=now; l->usb_seen=true; return true;
}
enum sf_transport sf_link_select(const struct sf_link *l, uint32_t now,
                                bool ready, bool ble) {
    if (ready&&l->usb_seen&&(uint32_t)(now-l->usb_time)<SF_USB_LEASE_MS) return SF_USB;
    return ble?SF_BLE:SF_NO_LINK;
}
bool sf_reliable_add(struct sf_reliable *q, const struct sf_packet *p) {
    if (p->length>SF_PAYLOAD_MAX||q->count==SF_PENDING_MAX) { q->overflow++; return false; }
    q->items[q->count++]=(struct sf_pending){.packet=*p}; return true;
}
const struct sf_packet *sf_reliable_due(struct sf_reliable *q, uint32_t now) {
    for (size_t i=0;i<q->count;i++) {
        struct sf_pending *p=&q->items[i];
        if (!p->sent||(uint32_t)(now-p->last_try)>=SF_RETRY_MS) {
            p->sent=true; p->last_try=now; return &p->packet;
        }
    }
    return NULL;
}
bool sf_reliable_ack(struct sf_reliable *q, uint32_t boot, uint32_t seq) {
    for (size_t i=0;i<q->count;i++) if (q->items[i].packet.boot==boot&&q->items[i].packet.seq==seq) {
        memmove(q->items+i,q->items+i+1,(q->count-i-1)*sizeof(q->items[0]));
        q->count--; return true;
    }
    return false;
}
