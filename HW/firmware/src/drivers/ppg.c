/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include <errno.h>
#define PPG 0x62
static int wr(const struct sf_bus *b, uint8_t r, uint8_t v) {
    return b->write(b->ctx,PPG,r,&v,1);
}
int sf_ppg_init(const struct sf_bus *b) {
    uint8_t x;
    if (b->read(b->ctx,PPG,0xff,&x,1)||x!=0x36) return -ENODEV;
    if (wr(b,0x0d,0x01)) return -EIO;
    b->delay_ms(b->ctx,2);
    if (b->read(b->ctx,PPG,0x0d,&x,1)||(x&1)) return -ETIMEDOUT;
    /* SINGLE_PPG=1, LP_MODE=1, shutdown while configuring. */
    if (wr(b,0x0d,0x0e)) return -EIO;
    const uint8_t settings[][2]={
        {0x02,0x80},{0x03,0x00}, /* FIFO almost-full IRQ only */
        {0x09,112},{0x0a,0x08}, /* 16 words watermark, no rollover, clear on read */
        {0x10,0x00}, /* no external sync or sensor timestamp tags */
        {0x11,0x0a}, /* ambient rejection ON, 16uA ADC, 58.7us integration */
        {0x12,0x08}, /* 50.027 samples/s, average=1 */
        {0x13,0xc0},{0x15,0x01}, /* 12us settling, PD bias */
        {0x20,0x01},{0x21,0x00},{0x22,0x00}, /* green-only exposure */
        {0x2a,0x00}, /* actual LED range register: 31mA full-scale */
        {0x23,0x08},{0x24,0x00},{0x25,0x00} /* green ~0.97mA, red/IR OFF */
    };
    for (size_t i=0;i<sizeof(settings)/sizeof(settings[0]);i++) {
        if (wr(b,settings[i][0],settings[i][1])||
            b->read(b->ctx,PPG,settings[i][0],&x,1)||x!=settings[i][1]) return -EIO;
    }
    uint8_t status[2];
    if (b->read(b->ctx,PPG,0x00,status,2)||wr(b,0x0a,0x18)) return -EIO; /* flush */
    return wr(b,0x0d,0x0c); /* enable single-channel dynamic low-power */
}
int sf_ppg_read(const struct sf_bus *b, struct sf_ppg_word *out, size_t cap,
                size_t *count, bool *gap) {
    *count=0; *gap=false;
    uint8_t counters[2], status[2];
    if (!cap||!out) return -EINVAL;
    if (b->read(b->ctx,PPG,0x06,counters,2)||b->read(b->ctx,PPG,0x00,status,2)) return -EIO;
    *gap=(counters[0]&0x7f)!=0;
    size_t n=counters[1];
    if (n>128) return -EIO;
    if (n>cap) n=cap;
    for (size_t i=0;i<n;i++) {
        uint8_t raw[3];
        /* A complete 3-byte burst is mandatory; FIFO_DATA does not auto-increment. */
        if (b->read(b->ctx,PPG,0x08,raw,3)) return -EIO;
        uint32_t v=((uint32_t)raw[0]<<16)|((uint32_t)raw[1]<<8)|raw[2];
        uint8_t tag=(uint8_t)(v>>19);
        if (tag==0x1e) { *gap=true; continue; } /* empty FIFO marker */
        out[*count]=(struct sf_ppg_word){.value=v&0x7ffff,.tag=tag,
                                      .saturated=(v&0x7ffff)==0x7ffff};
        (*count)++;
    }
    return 0;
}
