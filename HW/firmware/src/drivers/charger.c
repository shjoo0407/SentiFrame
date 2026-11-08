/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include <errno.h>
#include <string.h>
#define BQ 0x6a
static int read1(struct sf_charger *c, uint8_t r, uint8_t *v) {
    return c->bus.read(c->bus.ctx,BQ,r,v,1);
}
static int write1(struct sf_charger *c, uint8_t r, uint8_t v) {
    return c->bus.write(c->bus.ctx,BQ,r,&v,1);
}
static int verified(struct sf_charger *c, uint8_t r, uint8_t v, uint8_t mask) {
    uint8_t got;
    int err=write1(c,r,v);
    if (!err) err=read1(c,r,&got);
    if (!err&&(got&mask)!=(v&mask)) err=-EIO;
    return err;
}
void sf_charger_stop(struct sf_charger *c) {
    c->gate(c->gate_ctx,false); c->enabled=false;
}
static int fail(struct sf_charger *c, int err) {
    sf_charger_stop(c); c->fault=true; return err?err:-EIO;
}
int sf_charger_init(struct sf_charger *c, const struct sf_bus *b,
                    void (*gate)(void *, bool), void *ctx) {
    memset(c,0,sizeof(*c)); c->bus=*b; c->gate=gate; c->gate_ctx=ctx;
    sf_charger_stop(c);
    uint8_t id, ts, timer, ic;
    if (read1(c,0x0c,&id)|| (id&0x0f)!=0 ||
        read1(c,0x0b,&ts)||read1(c,0x08,&timer)||read1(c,0x07,&ic)) return fail(c,-EIO);
    /* Gate is OFF throughout: 4.20V, charge-disable+30mA, 10% termination,
     * 2x termination precharge (6mA), flash charge off, VINDPM 4.5V,
     * thermal regulation retained at 100C, nominal 3.0V precharge transition,
     * NTC retained, 6h safety timer,
     * 160s watchdog/register reset, input limit 50mA. */
    if (verified(c,0x04,0x99,0xff)||verified(c,0x03,0x46,0xff)||
        verified(c,0x05,0x24,0xff)||
        verified(c,0x0b,(uint8_t)((ts&0x0f)|0xe0),0xff)||
        verified(c,0x07,(uint8_t)((ic&0x30)|0x84),0xff)||
        verified(c,0x08,(uint8_t)(timer&0xf8),0xff)) return fail(c,-EIO);
    c->initialized=true; return 0;
}
enum sf_source sf_classify_source(const struct sf_power_input *p) {
    if (!p->present) return SF_SRC_NONE;
    if (!p->allow||!p->cc_valid||p->cc_attached!=2||p->cc_accessory||p->cc_current==2)
        return SF_SRC_UNKNOWN;
    if (p->high&&p->switch_open) return SF_SRC_DCP;
    if (p->switch_open) {
        if (p->cc_current==1) return SF_SRC_TYPEC_1A5;
        if (p->cc_current==3) return SF_SRC_TYPEC_3A;
        return SF_SRC_UNKNOWN;
    }
    return p->high?SF_SRC_CDP:SF_SRC_SDP;
}
static int check_config(struct sf_charger *c) {
    const uint8_t regs[]={0x03,0x04,0x05,0x07,0x08,0x0b};
    const uint8_t masks[]={0xff,0xff,0xff,0xcf,0x07,0xf0};
    const uint8_t want[]={0x46,c->enabled?0x19:0x99,0x24,0x84,0x00,0xe0};
    for (size_t i=0;i<sizeof(regs);i++) {
        uint8_t x;
        if (read1(c,regs[i],&x)||(x&masks[i])!=want[i]) return -EIO;
    }
    return 0;
}
int sf_charger_step(struct sf_charger *c, const struct sf_power_input *p, uint32_t now) {
    if (!c->initialized||c->fault) return fail(c,-EIO);
    enum sf_source source=sf_classify_source(p);
    bool admitted=source!=SF_SRC_NONE&&source!=SF_SRC_UNKNOWN&&
                  p->charging_requested&&p->pack_approved&&
                  p->battery_mv>=2300&&p->battery_mv<=4250;
    if ((source==SF_SRC_SDP||source==SF_SRC_CDP)&&!p->awake) admitted=false;
    if (source==SF_SRC_SDP&&!p->usb_alive) admitted=false;
    if (source!=c->stable_source) {
        admitted=false; c->stable_source=source; c->stable_since=now; c->stable_count=0;
    }
    if (!admitted) {
        bool was_enabled=c->enabled;
        sf_charger_stop(c); /* revoke hardware permission before ANY I2C operation */
        if (was_enabled&&verified(c,0x04,0x99,0xff)) return fail(c,-EIO);
        c->stable_count=0; c->stable_since=now;
    } else if (c->stable_count<4) c->stable_count++;
    if (check_config(c)||read1(c,0x00,&c->stat0)||read1(c,0x01,&c->stat1)||
        read1(c,0x02,&c->flags)) return fail(c,-EIO);
    /* TS_OPEN, thermal regulation, TS hot/cold/cool/warm, OVP, safety timer
     * and OCP still inhibit admission. BUVLO (STAT1 bit6 / FLAG0 bit1) protects
     * BAT->SYS discharge, not charging: an approved cell within the admission
     * range must be able to recover through the IC's hardware precharge path.
     * See BQ25186 sections 6.1.1.2 and 6.3.8.2. Never lower its BUVLO threshold
     * or bypass the input, pack, TS, current-limit or readback checks. */
    bool good=(c->stat0&0x83)==0x01&&(c->stat1&0x9c)==0&&(c->flags&0x85)==0;
    if (c->stat1&0x04 || c->flags&0x05) return fail(c,-EIO);
    if (!good||!admitted||c->stable_count<4||(uint32_t)(now-c->stable_since)<1000) {
        bool was_enabled=c->enabled;
        sf_charger_stop(c);
        if (was_enabled&&verified(c,0x04,0x99,0xff)) return fail(c,-EIO);
        if (!good) { c->stable_count=0; c->stable_since=now; }
        return 0;
    }
    if (!c->enabled) {
        if (verified(c,0x04,0x19,0xff)) return fail(c,-EIO);
        c->enabled=true; c->gate(c->gate_ctx,true); /* always LAST */
    }
    return 0;
}
int sf_cc_read(const struct sf_bus *b, struct sf_power_input *p) {
    p->cc_valid=false;
    uint8_t x[2];
    const uint8_t signature[8]={'0','2','3','B','S','U','T',0};
    for (uint8_t r=0;r<8;r++) {
        uint8_t value;
        int rc=b->read(b->ctx,0x47,r,&value,1);
        if (rc) return rc;
        if (value!=signature[r]) return -ENODEV;
    }
    /* Do not assume auto-increment on this controller. */
    int err=b->read(b->ctx,0x47,0x08,x,1);
    if (!err) err=b->read(b->ctx,0x47,0x09,x+1,1);
    if (err) return err;
    p->cc_current=(x[0]>>4)&3; p->cc_accessory=(x[0]>>1)&7;
    p->cc_attached=(x[1]>>6)&3; p->cc_valid=true;
    /* INT is NC: poll. Clear the W1C change flag, preserving other fields. */
    if (x[1]&0x10) {
        uint8_t value=(uint8_t)((x[1]&0x07)|0x10);
        err=b->write(b->ctx,0x47,0x09,&value,1);
        if (err) p->cc_valid=false;
    }
    return err;
}
