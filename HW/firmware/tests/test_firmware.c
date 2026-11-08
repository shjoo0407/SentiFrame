/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include "sf_imu.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned assertions, scenarios;
#define CHECK(x) do { assertions++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
struct mock {
    uint8_t reg[128][256], fifo[1024];
    size_t fifo_len, fifo_at, config_bytes;
    unsigned calls, fail_at, gate_high, writes;
    uint32_t delay_ms;
    bool gate, corrupt, require_gate_off;
    uint8_t corrupt_addr, corrupt_reg, corrupt_mask;
    uint32_t ppg_words[128];
    size_t ppg_len, ppg_at;
};
static void reset(struct mock *m) {
    memset(m,0,sizeof(*m));
    m->reg[0x6a][0x03]=0x46; m->reg[0x6a][0x04]=0x05;
    m->reg[0x6a][0x05]=0x20;
    m->reg[0x6a][0x06]=0x56; /* Discharge threshold 3.0V; must remain unchanged. */
    m->reg[0x6a][0x07]=0x84; m->reg[0x6a][0x08]=0x4d;
    m->reg[0x6a][0x0b]=0x0b; m->reg[0x6a][0x0c]=0x40;
    m->reg[0x6a][0x00]=1; m->reg[0x62][0xff]=0x36;
    m->reg[0x68][0x00]=0x24; m->reg[0x68][0x21]=1;
    m->reg[0x68][0x7c]=1;
    m->reg[0x47][0x08]=0x30; m->reg[0x47][0x09]=0x80;
    memcpy(m->reg[0x47],"023BSUT",8);
}
static int mock_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    struct mock *m=ctx; m->calls++;
    if (m->require_gate_off) CHECK(!m->gate);
    if (m->calls==m->fail_at) return -EIO;
    CHECK(addr<128);
    if (addr==0x68&&reg==0x24&&len==2) { sf_put16(buf,(uint16_t)(m->fifo_len-m->fifo_at)); return 0; }
    if (addr==0x68&&reg==0x26) {
        CHECK(len<=m->fifo_len-m->fifo_at); memcpy(buf,m->fifo+m->fifo_at,len); m->fifo_at+=len; return 0;
    }
    if (addr==0x62&&reg==0x06&&len==2) {
        buf[0]=m->reg[addr][reg]; buf[1]=(uint8_t)(m->ppg_len-m->ppg_at); return 0;
    }
    if (addr==0x62&&reg==0x08) {
        CHECK(len==3);
        uint32_t word=m->ppg_at<m->ppg_len?m->ppg_words[m->ppg_at++]:0xf00000;
        buf[0]=(uint8_t)(word>>16); buf[1]=(uint8_t)(word>>8); buf[2]=(uint8_t)word; return 0;
    }
    CHECK((size_t)reg+len<=256); memcpy(buf,m->reg[addr]+reg,len);
    if (m->corrupt&&addr==m->corrupt_addr&&reg==m->corrupt_reg)
        buf[0]^=m->corrupt_mask?m->corrupt_mask:1;
    if (addr==0x6a&&reg==0x02) m->reg[addr][reg]=0; /* RC fault flags */
    return 0;
}
static int mock_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len) {
    struct mock *m=ctx; m->calls++;
    if (m->require_gate_off) CHECK(!m->gate);
    if (m->calls==m->fail_at) return -EIO;
    m->writes++; CHECK(addr<128);
    if (addr==0x68&&reg==0x5e) { m->config_bytes+=len; return 0; }
    if (addr==0x68&&reg==0x7e&&buf[0]==0xb0) { m->fifo_len=0; m->fifo_at=0; }
    CHECK((size_t)reg+len<=256);
    uint8_t previous=m->reg[addr][reg];
    memcpy(m->reg[addr]+reg,buf,len);
    if (addr==0x62&&reg==0x0d&&(buf[0]&1)) m->reg[addr][reg]=0;
    if (addr==0x62&&reg==0x0a&&(buf[0]&0x10)) {
        m->reg[addr][reg]&=(uint8_t)~0x10; m->ppg_len=0; m->ppg_at=0;
    }
    if (addr==0x47&&reg==0x09&&len==1) {
        /* Status and cable direction are RO; INT_STATUS is W1C. */
        m->reg[addr][reg]=(uint8_t)((previous&0xe8)|
            ((previous&0x10)&(uint8_t)~buf[0])|(buf[0]&7));
    }
    return 0;
}
static void mock_delay(void *ctx, uint32_t ms) { ((struct mock *)ctx)->delay_ms+=ms; }
static struct sf_bus bus(struct mock *m) {
    return (struct sf_bus){.ctx=m,.read=mock_read,.write=mock_write,.delay_ms=mock_delay};
}
static void gate(void *ctx, bool on) {
    struct mock *m=ctx;
    if (on) {
        CHECK(m->reg[0x6a][0x03]==0x46);
        CHECK(m->reg[0x6a][0x04]==0x19);
        CHECK(m->reg[0x6a][0x05]==0x24); /* Flash OFF, 6mA precharge / 3mA termination. */
        CHECK((m->reg[0x6a][0x0b]&0xf0)==0xe0);
        CHECK((m->reg[0x6a][0x07]&0xcf)==0x84); /* TS, nominal 3V transition, safety/WDT. */
        CHECK((m->reg[0x6a][0x08]&7)==0);
        m->gate_high++;
    }
    m->gate=on;
}
static struct sf_power_input dcp(void) {
    return (struct sf_power_input){.present=true,.allow=true,.high=true,.switch_open=true,
        .awake=false,.cc_valid=true,.cc_attached=2,.charging_requested=true,
        .pack_approved=true,.battery_mv=3700};
}
static void settle(struct sf_charger *c, struct sf_power_input *p) {
    for (uint32_t t=0;t<=1250;t+=250) CHECK(sf_charger_step(c,p,t)==0);
}
static void protocol(void) {
    scenarios++;
    CHECK(sf_crc16((const uint8_t *)"123456789",9)==0x29b1);
    for (unsigned len=0;len<=SF_PAYLOAD_MAX;len++) {
        scenarios++;
        struct sf_packet p={.type=SF_IMU,.length=(uint16_t)len,.seq=0xfedcba98,
                            .time_ms=0x12345678,.boot=0x88776655};
        for (unsigned i=0;i<len;i++) p.payload[i]=(uint8_t)(i*37);
        uint8_t raw[SF_FRAME_MAX]; size_t n=sf_encode(&p,raw,sizeof(raw));
        CHECK(n==SF_HEADER_SIZE+len+2); CHECK(raw[6]==0x98&&raw[9]==0xfe);
        struct sf_parser parser={0}; struct sf_packet got;
        for (size_t i=0;i<n;i++) CHECK(sf_parser_push(&parser,raw[i],&got)==(i==n-1));
        CHECK(got.seq==p.seq&&got.boot==p.boot&&got.time_ms==p.time_ms&&got.length==len);
        CHECK(!memcmp(got.payload,p.payload,len)); CHECK(parser.used==0);
        CHECK(sf_encode(&p,raw,n-1)==0);
        raw[n-1]^=0x80;
        for (size_t i=0;i<n;i++) CHECK(!sf_parser_push(&parser,raw[i],&got));
        n=sf_encode(&p,raw,sizeof(raw));
        bool found=false;
        for (size_t i=0;i<n;i++) if (sf_parser_push(&parser,raw[i],&got)) found=true;
        CHECK(found&&got.seq==p.seq);
    }
    struct sf_packet p={.type=SF_STATUS,.length=SF_PAYLOAD_MAX+1};
    uint8_t raw[SF_FRAME_MAX]; CHECK(sf_encode(&p,raw,sizeof(raw))==0);
    p.length=0; p.seq=1234;
    size_t n=sf_encode(&p,raw,sizeof(raw));
    struct sf_parser parser={0}; struct sf_packet got;
    uint32_t rng=0xdeadbeef;
    for (unsigned i=0;i<100000;i++) {
        rng=rng*1664525u+1013904223u;
        (void)sf_parser_push(&parser,(uint8_t)(rng>>24),&got); CHECK(parser.used<=SF_FRAME_MAX);
    }
    bool found=false;
    for (unsigned k=0;k<8;k++) for (size_t i=0;i<n;i++) if (sf_parser_push(&parser,raw[i],&got)&&got.seq==1234) found=true;
    CHECK(found);
}
static void routing(void) {
    scenarios++;
    struct sf_link l={.boot=100};
    struct sf_packet hb={.type=SF_HEARTBEAT,.boot=100,.seq=1};
    CHECK(sf_link_select(&l,0,true,true)==SF_BLE); /* VBUS alone is insufficient */
    CHECK(!sf_link_heartbeat(&l,&hb,1,false));
    hb.boot=99; CHECK(!sf_link_heartbeat(&l,&hb,1,true)); hb.boot=100;
    hb.type=SF_ACK; CHECK(!sf_link_heartbeat(&l,&hb,1,true)); hb.type=SF_HEARTBEAT;
    CHECK(sf_link_heartbeat(&l,&hb,10,true));
    CHECK(sf_link_select(&l,100,true,true)==SF_USB);
    CHECK(!sf_link_heartbeat(&l,&hb,1000,true)); /* replay cannot renew the lease */
    CHECK(sf_link_select(&l,1510,true,true)==SF_BLE);
    CHECK(sf_link_select(&l,100,false,true)==SF_BLE);
    CHECK(sf_link_select(&l,1510,true,false)==SF_NO_LINK);
    l.usb_seq=UINT32_MAX; hb.seq=0;
    CHECK(sf_link_heartbeat(&l,&hb,UINT32_MAX-20,true));
    CHECK(sf_link_select(&l,50,true,true)==SF_USB); /* uptime and seq rollover */
    CHECK(sf_link_select(&l,2000,true,true)==SF_BLE);
    struct sf_packet restart={.type=SF_HELLO};
    CHECK(sf_link_hello(&l,&restart,true));
    hb.seq=1;CHECK(sf_link_heartbeat(&l,&hb,2001,true)); /* restarted Pi app */
    restart.boot=99;CHECK(!sf_link_hello(&l,&restart,true));
    struct sf_reliable q={0};
    for (unsigned i=0;i<SF_PENDING_MAX;i++) {
        struct sf_packet e={.type=SF_EVENT,.boot=100,.seq=i}; CHECK(sf_reliable_add(&q,&e));
    }
    CHECK(!sf_reliable_add(&q,&hb)&&q.overflow==1);
    for (unsigned i=0;i<SF_PENDING_MAX;i++) CHECK(sf_reliable_due(&q,10)->seq==i);
    CHECK(sf_reliable_due(&q,11)==NULL);
    CHECK(sf_reliable_due(&q,1010)->seq==0);
    CHECK(!sf_reliable_ack(&q,99,0)); CHECK(sf_reliable_ack(&q,100,0));
    CHECK(!sf_reliable_ack(&q,100,0)); CHECK(q.count==7);
}
static void charging(void) {
    struct mock m; struct sf_charger c; reset(&m); struct sf_bus b=bus(&m);
    scenarios++; CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    unsigned calls=m.calls;
    CHECK(!m.gate&&m.gate_high==0); CHECK(m.reg[0x6a][0x0b]==0xeb);
    CHECK(m.reg[0x6a][0x08]==0x48); /* preserved pushbutton/timer bits */
    for (unsigned at=1;at<=calls;at++) {
        scenarios++; reset(&m); m.fail_at=at; b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)<0); CHECK(!m.gate&&c.fault&&m.gate_high==0);
    }
    struct sf_power_input p=dcp();
    reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    settle(&c,&p); CHECK(m.gate&&c.enabled); CHECK(m.gate_high==1); /* ignores CP suspend on DCP */
    for (unsigned mask=0;mask<12;mask++) {
        scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp();
        switch (mask) {
        case 0:p.pack_approved=false;break;case 1:p.present=false;break;
        case 2:p.allow=false;break;case 3:p.cc_valid=false;break;
        case 4:p.cc_current=2;break;case 5:p.cc_attached=1;break;
        case 6:p.cc_attached=3;break;case 7:p.cc_accessory=4;break;
        case 8:p.battery_mv=-1;break;case 9:p.battery_mv=5000;break;
        case 10:p.battery_mv=2000;break;case 11:p.high=false;break;
        }
        settle(&c,&p); CHECK(!m.gate&&m.gate_high==0);
    }
    for (unsigned reg=0;reg<6;reg++) {
        scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); settle(&c,&p);
        const uint8_t regs[]={3,4,5,7,8,11};
        m.reg[0x6a][regs[reg]]=regs[reg]==8?5:0;
        CHECK(sf_charger_step(&c,&p,1500)<0); CHECK(!m.gate&&c.fault);
    }
    for (unsigned at=1;at<=9;at++) {
        scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); settle(&c,&p); m.fail_at=m.calls+at;
        CHECK(sf_charger_step(&c,&p,1500)<0); CHECK(!m.gate&&c.fault);
    }
    for (unsigned ts=1;ts<=3;ts++) {
        scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); settle(&c,&p); m.reg[0x6a][1]=(uint8_t)(ts<<3);
        CHECK(sf_charger_step(&c,&p,1500)==0); CHECK(!m.gate);
    }
    const uint8_t faults[]={1,4,0x80}; /* BUVLO is discharge-only, tested separately. */
    for (unsigned f=0;f<sizeof(faults);f++) {
        scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); settle(&c,&p); m.reg[0x6a][2]=faults[f];
        (void)sf_charger_step(&c,&p,1500); CHECK(!m.gate);
    }
    scenarios++;reset(&m);b=bus(&m);CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    p=dcp();settle(&c,&p);m.reg[0x6a][1]=4;
    CHECK(sf_charger_step(&c,&p,1500)<0&&!m.gate&&c.fault);
    scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    p=dcp(); p.high=false; p.switch_open=false; p.awake=true;
    CHECK(sf_classify_source(&p)==SF_SRC_SDP); settle(&c,&p); CHECK(!m.gate);
    p.usb_alive=true;
    for (uint32_t t=1500;t<=2750;t+=250) CHECK(sf_charger_step(&c,&p,t)==0);
    CHECK(m.gate);
    p.awake=false; CHECK(sf_charger_step(&c,&p,3000)==0&&!m.gate);
    scenarios++; reset(&m); b=bus(&m); CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    p=dcp();p.switch_open=false;p.awake=true;CHECK(sf_classify_source(&p)==SF_SRC_CDP);
    settle(&c,&p);CHECK(m.gate);p.awake=false;CHECK(sf_charger_step(&c,&p,2000)==0&&!m.gate);
    p=dcp();p.high=false;p.cc_current=1;CHECK(sf_classify_source(&p)==SF_SRC_TYPEC_1A5);
    p.cc_current=3;CHECK(sf_classify_source(&p)==SF_SRC_TYPEC_3A);
    p.cc_current=2;CHECK(sf_classify_source(&p)==SF_SRC_UNKNOWN);
    reset(&m); b=bus(&m); p=(struct sf_power_input){0};
    CHECK(sf_cc_read(&b,&p)==0&&p.cc_valid&&p.cc_current==3&&p.cc_attached==2);
    m.reg[0x47][8]=0x20;CHECK(sf_cc_read(&b,&p)==0&&p.cc_current==2);
    m.reg[0x47][9]=0x95;CHECK(sf_cc_read(&b,&p)==0&&m.reg[0x47][9]==0x85);
    m.fail_at=m.calls+1;CHECK(sf_cc_read(&b,&p)<0&&!p.cc_valid);
    m.fail_at=0;m.reg[0x47][0]=0;CHECK(sf_cc_read(&b,&p)==-ENODEV&&!p.cc_valid);
}
static void charging_switch(void) {
    struct mock m; struct sf_charger c; struct sf_bus b;
    for (unsigned requested=0;requested<2;requested++) for (unsigned approved=0;approved<2;approved++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        struct sf_power_input p=dcp();
        p.charging_requested=requested!=0; p.pack_approved=approved!=0;
        settle(&c,&p);
        CHECK(c.enabled==(requested&&approved)); CHECK(m.gate==c.enabled);
        CHECK(!c.fault);
        CHECK(m.reg[0x6a][4]==(c.enabled?0x19:0x99));
    }
    scenarios++; reset(&m); b=bus(&m);
    CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    struct sf_power_input p=dcp(); settle(&c,&p); CHECK(m.gate);
    unsigned before=m.calls;
    p.charging_requested=false;
    CHECK(sf_charger_step(&c,&p,1500)==0);
    unsigned revoke_calls=m.calls-before;
    CHECK(!c.enabled&&!m.gate&&!c.fault&&m.reg[0x6a][4]==0x99);
    CHECK(c.stable_count==0);
    for (uint32_t t=1750;t<10000;t+=250) CHECK(sf_charger_step(&c,&p,t)==0&&!m.gate);
    CHECK(m.gate_high==1); /* A disabled feature never re-arms on a stable source. */
    p.charging_requested=true;
    for (uint32_t t=10000;t<=11250;t+=250) CHECK(sf_charger_step(&c,&p,t)==0);
    CHECK(m.gate&&m.gate_high==2); /* Enabling requires the full admission delay. */
    for (unsigned at=1;at<=revoke_calls;at++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); settle(&c,&p); CHECK(m.gate);
        m.fail_at=m.calls+at; p.charging_requested=false;
        CHECK(sf_charger_step(&c,&p,1500)<0);
        CHECK(!c.enabled&&!m.gate&&c.fault);
        CHECK(m.gate_high==1); /* Hardware permission is revoked before a failing bus call. */
    }
}
static void charging_buvlo_recovery(void) {
    struct mock m; struct sf_charger c; struct sf_bus b;
    struct sf_power_input p;
    scenarios++; reset(&m); b=bus(&m);
    m.reg[0x6a][5]=0xff; m.reg[0x6a][7]=0xf4; /* Retained hostile/previous settings. */
    CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    CHECK(m.reg[0x6a][5]==0x24&&m.reg[0x6a][7]==0xb4);
    CHECK((m.reg[0x6a][7]&0x40)==0&&(m.reg[0x6a][7]&0x30)==0x30);
    CHECK(m.reg[0x6a][6]==0x56);
    scenarios++; reset(&m); b=bus(&m);
    CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    CHECK(m.reg[0x6a][5]==0x24); /* Verified 2x10% of 30mA = 6mA hardware precharge. */
    p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
    for (uint32_t t=0;t<=750;t+=250) {
        m.reg[0x6a][2]=2; /* Repeated BUVLO indication must not reset admission. */
        CHECK(sf_charger_step(&c,&p,t)==0&&!c.fault&&!m.gate);
    }
    unsigned before=m.calls;
    m.reg[0x6a][2]=2;
    CHECK(sf_charger_step(&c,&p,1000)==0&&c.enabled&&m.gate&&!c.fault);
    unsigned admission_calls=m.calls-before;
    CHECK(c.stat1==0x40&&c.flags==2&&m.reg[0x6a][2]==0);
    CHECK(m.gate_high==1&&m.reg[0x6a][4]==0x19);
    p.battery_mv=3700; m.reg[0x6a][1]=0;
    CHECK(sf_charger_step(&c,&p,1250)==0&&m.gate&&!c.fault&&m.gate_high==1);
    CHECK(m.reg[0x6a][6]==0x56); /* Recovery never weakens BAT->SYS discharge UVLO. */
    /* A later discharge-path indication on a valid source is not a charge fault. */
    p.battery_mv=2900; m.reg[0x6a][1]=0x40; m.reg[0x6a][2]=2;
    CHECK(sf_charger_step(&c,&p,1500)==0&&m.gate&&!c.fault&&m.gate_high==1);

    for (unsigned requested=0;requested<2;requested++) for (unsigned approved=0;approved<2;approved++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900;
        p.charging_requested=requested!=0; p.pack_approved=approved!=0;
        m.reg[0x6a][1]=0x40; m.reg[0x6a][2]=2;
        settle(&c,&p);
        CHECK(m.gate==(requested&&approved)&&c.enabled==m.gate&&!c.fault);
    }
    const int voltages[]={-1,0,2299,2300,4250,4251};
    for (size_t j=0;j<sizeof(voltages)/sizeof(voltages[0]);j++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=voltages[j];
        m.reg[0x6a][1]=0x40; m.reg[0x6a][2]=2;
        settle(&c,&p);
        bool allowed=voltages[j]>=2300&&voltages[j]<=4250;
        CHECK(m.gate==allowed&&c.enabled==allowed&&!c.fault);
    }
    /* BUVLO must not mask temperature, power-good, OVP, OCP or safety faults. */
    const struct { uint8_t stat0,stat1,flags; bool fatal; } faults[]={
        {0x81,0x40,0x02,false}, {0x03,0x40,0x02,false},
        {0x01,0x48,0x82,false}, {0x01,0x50,0x02,false}, {0x01,0x58,0x02,false},
        {0x01,0xc0,0x06,true}, {0x01,0x40,0x03,true}, {0x01,0x44,0x02,true},
        {0x00,0x40,0x02,false}
    };
    for (size_t j=0;j<sizeof(faults)/sizeof(faults[0]);j++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
        settle(&c,&p); CHECK(m.gate);
        m.reg[0x6a][0]=faults[j].stat0; m.reg[0x6a][1]=faults[j].stat1;
        m.reg[0x6a][2]=faults[j].flags;
        int err=sf_charger_step(&c,&p,1500);
        CHECK(!m.gate&&!c.enabled&&c.fault==faults[j].fatal);
        CHECK((err<0)==faults[j].fatal);
        m.reg[0x6a][0]=1; m.reg[0x6a][1]=0x40; m.reg[0x6a][2]=2;
        for (uint32_t t=1750;t<=3000;t+=250) {
            err=sf_charger_step(&c,&p,t);
            CHECK((err<0)==faults[j].fatal);
        }
        CHECK(m.gate==!faults[j].fatal&&c.fault==faults[j].fatal);
    }
    /* Fail every read/write of the transition that would first enable recovery. */
    for (unsigned at=1;at<=admission_calls;at++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
        for (uint32_t t=0;t<=750;t+=250) CHECK(sf_charger_step(&c,&p,t)==0&&!m.gate);
        m.reg[0x6a][2]=2; m.fail_at=m.calls+at;
        CHECK(sf_charger_step(&c,&p,1000)<0);
        CHECK(!m.gate&&!c.enabled&&c.fault&&m.gate_high==0);
    }
    const uint8_t protections[]={3,4,5,7,8,11};
    for (size_t j=0;j<sizeof(protections);j++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
        settle(&c,&p); CHECK(m.gate);
        /* Include FLASH/precharge/termination drift, not only the fast-charge setting. */
        m.reg[0x6a][protections[j]]^=protections[j]==11?0x10:0x01;
        CHECK(sf_charger_step(&c,&p,1500)<0&&!m.gate&&!c.enabled&&c.fault);
    }
    const struct { uint8_t reg,mask; } mode_drift[]={
        {5,0x80}, {5,0x40}, {5,0x10}, {7,0x40}
    };
    for (size_t j=0;j<sizeof(mode_drift)/sizeof(mode_drift[0]);j++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
        settle(&c,&p); CHECK(m.gate);
        m.reg[0x6a][mode_drift[j].reg]^=mode_drift[j].mask;
        CHECK(sf_charger_step(&c,&p,1500)<0&&!m.gate&&!c.enabled&&c.fault);
    }
    for (unsigned changed=0;changed<4;changed++) {
        scenarios++; reset(&m); b=bus(&m);
        CHECK(sf_charger_init(&c,&b,gate,&m)==0);
        p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
        settle(&c,&p); CHECK(m.gate);
        if (changed==0) p.present=false;
        if (changed==1) p.allow=false;
        if (changed==2) p.cc_valid=false;
        if (changed==3) p.battery_mv=2299;
        m.require_gate_off=true; m.reg[0x6a][2]=2;
        CHECK(sf_charger_step(&c,&p,1500)==0&&!m.gate&&!c.enabled&&!c.fault);
        CHECK(m.reg[0x6a][4]==0x99); /* GPIO permission revoked before any I2C. */
    }
    scenarios++; reset(&m); b=bus(&m);
    CHECK(sf_charger_init(&c,&b,gate,&m)==0);
    p=dcp(); p.battery_mv=2900; m.reg[0x6a][1]=0x40;
    settle(&c,&p); CHECK(m.gate);
    p.present=false; m.require_gate_off=true; m.fail_at=m.calls+1;
    CHECK(sf_charger_step(&c,&p,1500)<0&&!m.gate&&!c.enabled&&c.fault);
}
static void ppg(void) {
    scenarios++; struct mock m;reset(&m);struct sf_bus b=bus(&m);
    CHECK(sf_ppg_init(&b)==0);
    CHECK(m.reg[0x62][0x2a]==0&&m.reg[0x62][0x23]==8&&m.reg[0x62][0x24]==0);
    CHECK(m.reg[0x62][0x0d]==0x0c&&m.reg[0x62][0x12]==8&&m.reg[0x62][0x0a]==8);
    unsigned calls=m.calls;
    for (unsigned at=1;at<=calls;at++) {
        scenarios++; reset(&m); b=bus(&m); m.fail_at=at; CHECK(sf_ppg_init(&b)<0);
    }
    reset(&m);b=bus(&m);m.reg[0x62][0xff]=0;CHECK(sf_ppg_init(&b)==-ENODEV);
    reset(&m);b=bus(&m);m.ppg_len=3;
    m.ppg_words[0]=(1u<<19)|12345; m.ppg_words[1]=(1u<<19)|0x7ffff;
    m.ppg_words[2]=(0x1eu<<19); m.reg[0x62][6]=1;
    struct sf_ppg_word out[32];size_t n;bool gap;
    CHECK(sf_ppg_read(&b,out,32,&n,&gap)==0&&n==2&&gap);
    CHECK(out[0].tag==1&&out[0].value==12345&&!out[0].saturated);
    CHECK(out[1].saturated);CHECK(sf_ppg_read(&b,out,0,&n,&gap)==-EINVAL);
}
static void imu(void) {
    for (unsigned gyro=0;gyro<=1;gyro++) {
        scenarios++; struct mock m;reset(&m);m.reg[0x68][0x6b]=0x30;
        struct sf_bus b=bus(&m);struct sf_imu i;
        int err=sf_imu_init(&i,&b,gyro!=0);
        if (err) fprintf(stderr,"BMI270 init=%d last vendor status=%d calls=%u\n",err,i.dev.intf_rslt,m.calls);
        CHECK(err==0); CHECK(m.config_bytes==8192);
        CHECK((m.reg[0x68][0x41]&3)==3); /* +/-16g */
        CHECK((m.reg[0x68][0x40]&0x0f)==8); /* 100Hz */
        CHECK((m.reg[0x68][0x7d]&7)==(gyro?6:4)); /* AUX disabled */
        CHECK((m.reg[0x68][0x6b]&0x30)==0); /* AUX/OIS disabled */
        CHECK((m.reg[0x68][0x53]&0x0a)==0x0a); /* INT1 active high/output */
        CHECK((m.reg[0x68][0x53]&4)==0); /* push-pull */
        for (unsigned j=0;j<4;j++) {
            m.fifo[m.fifo_len++]=gyro?0x8c:0x84;
            if (gyro) for (unsigned axis=0;axis<3;axis++) { sf_put16(m.fifo+m.fifo_len,(uint16_t)(100+axis));m.fifo_len+=2; }
            for (unsigned axis=0;axis<3;axis++) { sf_put16(m.fifo+m.fifo_len,(uint16_t)(2048+axis));m.fifo_len+=2; }
        }
        struct sf_sample out[64];size_t n;
        CHECK(sf_imu_read(&i,out,64,&n,1000)==0&&n==4);
        CHECK(out[0].a[0]==2048&&out[0].time_ms==970&&out[3].time_ms==1000);
        CHECK((out[0].flags&SF_GYRO_VALID)==(gyro?SF_GYRO_VALID:0));
        if (gyro) CHECK(out[0].g[2]==102);
        CHECK(sf_imu_read(&i,out,64,&n,1020)==0&&n==0);
        m.fifo_at=0;m.fifo_len=600;CHECK(sf_imu_read(&i,out,64,&n,1040)==-EOVERFLOW&&n==0);
        CHECK(m.fifo_len==0);
        reset(&m);b=bus(&m);m.reg[0x68][0]=0;CHECK(sf_imu_init(&i,&b,gyro!=0)<0);
    }
}
static void imu_warm_state(struct mock *m) {
    reset(m);
    m->reg[0x68][0x7d]=0x0f; m->reg[0x68][0x7c]=7;
    m->reg[0x68][0x53]=m->reg[0x68][0x54]=0x1e;
    m->reg[0x68][0x56]=m->reg[0x68][0x57]=0x7f;
    m->reg[0x68][0x58]=0xff;
    m->reg[0x68][0x48]=3; m->reg[0x68][0x49]=0xff;
    m->reg[0x68][0x6b]=0x30;
    m->fifo_len=14;
}
static void imu_suspend(void) {
    scenarios++; struct mock m; imu_warm_state(&m); struct sf_bus b=bus(&m);
    CHECK(sf_imu_shutdown(&b)==0);
    unsigned calls=m.calls;
    CHECK(m.reg[0x68][0x7d]==0&&m.reg[0x68][0x7c]==1);
    const uint8_t cleared[]={0x53,0x54,0x56,0x57,0x58,0x48,0x49,0x6b};
    for (size_t j=0;j<sizeof(cleared);j++) CHECK(m.reg[0x68][cleared[j]]==0);
    CHECK(m.fifo_len==0&&m.config_bytes==0&&m.delay_ms>=22);
    CHECK(sf_imu_shutdown(&b)==0&&m.config_bytes==0); /* Idempotent; no 8KiB upload. */
    for (unsigned at=1;at<=calls;at++) {
        scenarios++; imu_warm_state(&m); b=bus(&m); m.fail_at=at;
        CHECK(sf_imu_shutdown(&b)<0);
        CHECK(m.config_bytes==0);
        if (at>1) CHECK(m.calls==calls); /* Later shutdown steps still run. */
    }
    const uint8_t verified[][2]={
        {0x7d,1},{0x7c,1},{0x53,8},{0x54,8},{0x56,1},{0x57,1},
        {0x58,1},{0x48,1},{0x49,1},{0x6b,0x10}
    };
    for (size_t j=0;j<sizeof(verified)/sizeof(verified[0]);j++) {
        scenarios++; imu_warm_state(&m); b=bus(&m);
        m.corrupt=true; m.corrupt_addr=0x68; m.corrupt_reg=verified[j][0];
        m.corrupt_mask=verified[j][1];
        CHECK(sf_imu_shutdown(&b)==-EIO&&m.config_bytes==0);
    }
    scenarios++; reset(&m); b=bus(&m); m.reg[0x68][0]=0;
    CHECK(sf_imu_shutdown(&b)==-ENODEV&&m.writes==0);
    CHECK(sf_imu_shutdown(NULL)==-EINVAL);
    struct sf_bus invalid=b; invalid.read=NULL; CHECK(sf_imu_shutdown(&invalid)==-EINVAL);
    invalid=b; invalid.write=NULL; CHECK(sf_imu_shutdown(&invalid)==-EINVAL);
    invalid=b; invalid.delay_ms=NULL; CHECK(sf_imu_shutdown(&invalid)==-EINVAL);
    for (unsigned gyro=0;gyro<2;gyro++) {
        scenarios++; reset(&m); b=bus(&m); struct sf_imu i;
        CHECK(sf_imu_init(&i,&b,gyro!=0)==0&&m.config_bytes==8192);
        CHECK(sf_imu_shutdown(&b)==0);
        CHECK(m.reg[0x68][0x7d]==0&&m.reg[0x68][0x7c]==1&&m.config_bytes==8192);
    }
}
static struct sf_sample stationary(uint32_t t) {
    return (struct sf_sample){.a={0,0,2048},.time_ms=t};
}
static void falls(void) {
    scenarios++;struct sf_fall f;sf_fall_reset(&f);
    for (uint32_t t=0;t<10000;t+=10) { struct sf_sample s=stationary(t);CHECK(sf_fall_push(&f,&s)==0); }
    CHECK(f.count==200&&isfinite(f.last_z));
    struct sf_sample s=stationary(10000);s.a[2]=8192;
    CHECK(sf_fall_push(&f,&s)==SF_IMPACT);CHECK(f.last_z>6);
    struct sf_sample history[200];size_t n=sf_fall_history(&f,history,200);
    CHECK(n==200&&history[199].a[2]==8192); /* impact is never outlier-clipped */
    bool suspected=false;
    for (uint32_t t=10010;t<11000;t+=10) {
        s=(struct sf_sample){.a={2048,0,0},.time_ms=t};
        if (sf_fall_push(&f,&s)==SF_FALL_SUSPECTED) suspected=true;
    }
    CHECK(suspected);
    scenarios++;sf_fall_reset(&f);
    for (uint32_t t=0;t<1000;t+=10) { s=stationary(t);CHECK(sf_fall_push(&f,&s)==0); }
    s=stationary(1000);s.a[2]=8192;CHECK(sf_fall_push(&f,&s)==SF_IMPACT);
    s=(struct sf_sample){.a={2048,0,0},.time_ms=2000};
    CHECK(sf_fall_push(&f,&s)==0&&!f.pending&&f.count==1); /* loss is not stillness */
    scenarios++;sf_fall_reset(&f);
    for (uint32_t t=0;t<1000;t+=10) { s=stationary(t);(void)sf_fall_push(&f,&s); }
    s=stationary(1000);s.a[2]=8192;(void)sf_fall_push(&f,&s);
    for (uint32_t t=1010;t<5000;t+=10) {
        s=(struct sf_sample){.a={2048,0,0},.g={1000,0,0},.flags=SF_GYRO_VALID,.time_ms=t};
        CHECK(sf_fall_push(&f,&s)!=SF_FALL_SUSPECTED);
    }
    scenarios++;sf_fall_reset(&f);
    for (unsigned j=0;j<300;j++) {
        s=stationary(UINT32_MAX-1500u+j*10u);CHECK(sf_fall_push(&f,&s)==0);
    }
    CHECK(f.count==200&&isfinite(f.last_z));
}
int main(int argc, char **argv) {
    if (argc==2&&!strcmp(argv[1],"--vectors")) {
        puts("[");
        for (unsigned len=0;len<=SF_PAYLOAD_MAX;len++) {
            struct sf_packet p={.type=SF_IMU,.length=(uint16_t)len,
                .seq=0xfedcba98,.time_ms=0x12345678,.boot=0x88776655};
            for (unsigned j=0;j<len;j++) p.payload[j]=(uint8_t)(j*37);
            uint8_t raw[SF_FRAME_MAX];size_t n=sf_encode(&p,raw,sizeof(raw));
            printf("\"");for (size_t j=0;j<n;j++) printf("%02x",raw[j]);
            printf("\"%s\n",len==SF_PAYLOAD_MAX?"":",");
        }
        puts("]");return 0;
    }
    protocol();routing();charging();charging_switch();charging_buvlo_recovery();ppg();imu();imu_suspend();falls();
    printf("{\"scenarios\":%u,\"assertions\":%u,\"result\":\"PASS\"}\n",scenarios,assertions);
    return 0;
}
