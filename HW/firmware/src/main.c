/* SPDX-License-Identifier: MIT */
#include "sf_platform.h"
#include "sf_imu.h"
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <math.h>
#include <string.h>
LOG_MODULE_REGISTER(sentiframe,LOG_LEVEL_INF);

static struct sf_bus bus;
static struct sf_charger charger;
static struct sf_imu imu;
static struct sf_fall fall;
static struct sf_link link;
static struct sf_reliable reliable;
static struct sf_parser usb_parser, ble_parser;
static struct sf_power_input power;
static uint32_t sequence, sensor_errors, context_overflows;
static bool imu_ok, ppg_ok, physical_usb_ready, imu_shutdown_failed;
static bool ppg_failed, ppg_needs_off;
static uint32_t led_until, usb_since;
static struct sf_sample snapshot[SF_HISTORY_SIZE];
static size_t snapshot_count, snapshot_offset;
static uint32_t snapshot_event, snapshot_started, history_try;
static struct sf_packet history_packet;
static bool history_waiting, history_sent;

static struct sf_packet packet(uint8_t type, uint32_t now) {
    return (struct sf_packet){.type=type,.seq=++sequence,.time_ms=now,.boot=link.boot};
}
static enum sf_transport route(uint32_t now) {
    return sf_link_select(&link,now,physical_usb_ready,sf_ble_ready());
}
static void hello(enum sf_transport dst, uint32_t now) {
    struct sf_packet p=packet(SF_HELLO,now);
    p.length=6; p.payload[0]=1; p.payload[1]=6; /* protocol 1, hardware Rev F */
    p.payload[2]=0; p.payload[3]=2; p.payload[4]=1; /* firmware 0.2.1 */
    p.payload[5]=(IS_ENABLED(CONFIG_SF_GYRO)?1:0)|(IS_ENABLED(CONFIG_SF_PPG)?2:0)|
                 (IS_ENABLED(CONFIG_SF_PACK_APPROVED)?4:0)|
                 (IS_ENABLED(CONFIG_SF_IMU)?8:0)|(IS_ENABLED(CONFIG_SF_CHARGING)?16:0);
    (void)sf_transport_send(dst,&p);
}
static void queue_event(enum sf_event kind, uint8_t detail, uint32_t now) {
    bool capture=kind==SF_IMPACT||kind==SF_SOS;
    bool busy=capture&&snapshot_count;
    struct sf_packet p=packet(SF_EVENT,now);
    p.length=12; p.payload[0]=(uint8_t)kind; p.payload[1]=detail;
    sf_put16(p.payload+2,busy?SF_DATA_GAP:0);
    sf_put16(p.payload+4,power.battery_mv<0?UINT16_MAX:(uint16_t)power.battery_mv);
    sf_put16(p.payload+6,(uint16_t)fminf(fall.peak_g*1000.0f,65535.0f));
    int16_t z=(int16_t)(fminf(fmaxf(fall.last_z,-327.68f),327.67f)*100.0f);
    sf_put16(p.payload+8,(uint16_t)z);
    sf_put16(p.payload+10,capture&&!busy?(uint16_t)fall.count:0);
    if (!sf_reliable_add(&reliable,&p)) { LOG_ERR("Event queue full"); return; }
    if (busy) context_overflows++;
    if (capture&&!busy) {
        snapshot_count=sf_fall_history(&fall,snapshot,ARRAY_SIZE(snapshot));
        snapshot_offset=0; snapshot_event=p.seq; snapshot_started=now;
        history_waiting=false; history_sent=false;
    }
    if (kind!=SF_SENSOR_FAULT) led_until=now+500;
}
static void ppg_fault(uint32_t now) {
    if (ppg_failed) return;
    ppg_failed=true; sensor_errors++;
    queue_event(SF_SENSOR_FAULT,3,now);
}
static void ppg_stop(uint32_t now) {
    int err=sf_ppg_power(false);
    ppg_ok=false;
    /* If GPIO failed, keep trying to revoke the rail, and report uncertainty. */
    ppg_needs_off=err!=0;
    if (err) ppg_fault(now);
}
static void history_send(uint32_t now) {
    if (!snapshot_count) return;
    if ((uint32_t)(now-snapshot_started)>60000) {
        snapshot_count=0; history_waiting=false; context_overflows++; return;
    }
    enum sf_transport dst=route(now);
    if (dst==SF_NO_LINK) return;
    if (!history_waiting) {
        history_packet=packet(SF_HISTORY,now);
        size_t n=snapshot_count-snapshot_offset; if (n>4) n=4;
        sf_put32(history_packet.payload,snapshot_event);
        sf_put16(history_packet.payload+4,(uint16_t)snapshot_count);
        sf_put16(history_packet.payload+6,(uint16_t)snapshot_offset);
        history_packet.payload[8]=(uint8_t)n;
        for (size_t i=0;i<n;i++) {
            const struct sf_sample *s=snapshot+snapshot_offset+i;
            uint8_t *b=history_packet.payload+9+i*18;
            sf_put32(b,s->time_ms);
            for (unsigned j=0;j<3;j++) { sf_put16(b+4+2*j,(uint16_t)s->a[j]); sf_put16(b+10+2*j,(uint16_t)s->g[j]); }
            sf_put16(b+16,s->flags);
        }
        history_packet.length=(uint16_t)(9+n*18); history_waiting=true; history_sent=false;
    }
    if (!history_sent||(uint32_t)(now-history_try)>=SF_RETRY_MS) {
        if (!sf_transport_send(dst,&history_packet)) { history_sent=true; history_try=now; }
    }
}
static void consume(const struct sf_packet *p, enum sf_transport src, uint32_t now) {
    if (src==SF_USB&&!physical_usb_ready) return;
    if (src==SF_BLE&&!sf_ble_ready()) return;
    if (p->type==SF_HELLO&&p->length==0&&(p->boot==0||p->boot==link.boot)) {
        if (src==SF_USB&&!sf_link_hello(&link,p,physical_usb_ready)) return;
        hello(src,now); return;
    }
    if (p->boot!=link.boot) return;
    if (p->type==SF_HEARTBEAT&&p->length==0) {
        if (src==SF_USB) (void)sf_link_heartbeat(&link,p,now,physical_usb_ready);
        struct sf_packet response=packet(SF_HEARTBEAT,now);
        (void)sf_transport_send(src,&response);
    } else if (p->type==SF_ACK&&p->length==4) {
        uint32_t seq=sf_get32(p->payload);
        (void)sf_reliable_ack(&reliable,p->boot,seq);
        if (history_waiting&&seq==history_packet.seq) {
            snapshot_offset+=history_packet.payload[8]; history_waiting=false;
            if (snapshot_offset==snapshot_count) snapshot_count=0;
        }
    }
    /* Received packets cannot change charge limits, disable TS, drive the
     * motor, or mark an unknown battery as approved. */
}
static void receive(uint32_t now) {
    struct sf_rx_chunk c;
    for (unsigned n=0;n<24&&sf_rx_pop(&c);n++) {
        struct sf_parser *parser=c.transport==SF_USB?&usb_parser:&ble_parser;
        if (c.gap) memset(parser,0,sizeof(*parser));
        if (c.transport==SF_USB&&!physical_usb_ready) { memset(parser,0,sizeof(*parser)); continue; }
        for (size_t i=0;i<c.length;i++) {
            struct sf_packet p;
            if (sf_parser_push(parser,c.bytes[i],&p)) consume(&p,c.transport,now);
        }
    }
}
static void raw_imu(const struct sf_sample *s) {
    struct sf_packet p=packet(SF_IMU,s->time_ms); p.length=14;
    for (unsigned i=0;i<3;i++) { sf_put16(p.payload+2*i,(uint16_t)s->a[i]); sf_put16(p.payload+6+2*i,(uint16_t)s->g[i]); }
    sf_put16(p.payload+12,s->flags);
    enum sf_transport dst=route(k_uptime_get_32());
    if (dst!=SF_NO_LINK) (void)sf_transport_send(dst,&p);
}
static void status(uint32_t now) {
    struct sf_packet p=packet(SF_STATUS,now); p.length=30;
    sf_put16(p.payload,power.battery_mv<0?UINT16_MAX:(uint16_t)power.battery_mv);
    p.payload[2]=(uint8_t)sf_classify_source(&power);
    p.payload[3]=(charger.enabled?1:0)|(charger.fault?2:0)|(imu_ok?4:0)|(ppg_ok?8:0)|
        (IS_ENABLED(CONFIG_SF_GYRO)?16:0)|(power.pack_approved?32:0)|
        (physical_usb_ready?64:0)|(power.usb_alive?128:0);
    p.payload[4]=charger.stat0; p.payload[5]=charger.stat1;
    p.payload[6]=(uint8_t)route(now); p.payload[7]=(uint8_t)reliable.count;
    sf_put32(p.payload+8,sensor_errors); sf_put32(p.payload+12,sf_tx_drops());
    sf_put32(p.payload+16,sf_rx_drops()); sf_put32(p.payload+20,reliable.overflow);
    sf_put32(p.payload+24,context_overflows); sf_put16(p.payload+28,CONFIG_SF_STREAM_DIVISOR);
    enum sf_transport dst=route(now);
    if (dst!=SF_NO_LINK) (void)sf_transport_send(dst,&p);
}
int main(void) {
    int err=sf_board_init(&bus);
    if (err) { LOG_ERR("GPIO/bus init failed: %d",err); return err; }
    err=sf_watchdog_start();
    if (err) { sf_charge_gate(NULL,false); LOG_ERR("Watchdog unavailable: %d",err); return err; }
    link.boot=sys_rand32_get(); if (!link.boot) link.boot=1;
    power.pack_approved=IS_ENABLED(CONFIG_SF_PACK_APPROVED);
    power.charging_requested=IS_ENABLED(CONFIG_SF_CHARGING); power.battery_mv=-1;
    err=sf_charger_init(&charger,&bus,sf_charge_gate,NULL);
    if (err) { sensor_errors++; LOG_ERR("Charge locked off: %d",err); }
    err=sf_transport_init();
    if (err) { sf_charge_gate(NULL,false); LOG_ERR("Transport init failed: %d",err); return err; }
    if (IS_ENABLED(CONFIG_SF_IMU)) {
        imu_ok=sf_imu_init(&imu,&bus,IS_ENABLED(CONFIG_SF_GYRO))==0;
    } else {
        /* MCU reset does not reset the separately powered BMI270. */
        imu_shutdown_failed=sf_imu_shutdown(&bus)!=0;
    }
    if ((IS_ENABLED(CONFIG_SF_IMU)&&!imu_ok)||imu_shutdown_failed) {
        sensor_errors++; queue_event(SF_SENSOR_FAULT,2,k_uptime_get_32());
    }
    uint32_t last_power=k_uptime_get_32()-250, last_battery=k_uptime_get_32()-10000;
    uint32_t last_imu=0,last_ppg=0,last_status=0,last_retry=k_uptime_get_32();
    usb_since=k_uptime_get_32();
    bool button_raw=false,button_stable=false,charger_reported=false;
    uint32_t button_changed=0,telemetry_count=0;
    while (true) {
        uint32_t now=k_uptime_get_32();
        if (sf_take_power_change()) {
            struct sf_power_input revoked=power; revoked.present=false;
            (void)sf_charger_step(&charger,&revoked,now);
            link.usb_seen=false; physical_usb_ready=false; usb_since=now;
            sf_rx_reset(SF_USB);
            memset(&usb_parser,0,sizeof(usb_parser)); last_power=now-250;
        }
        if ((uint32_t)(now-last_battery)>=10000) { power.battery_mv=sf_battery_mv(); last_battery=now; }
        if ((uint32_t)(now-last_power)>=250) {
            bool was_present=power.present;
            int pin_err=sf_read_inputs(&power);
            if (!was_present&&power.present) usb_since=now;
            if (pin_err) sensor_errors++;
            power.cc_valid=false;
            if (power.present&&!pin_err&&sf_cc_read(&bus,&power)) sensor_errors++;
            physical_usb_ready=power.present&&power.awake&&!power.switch_open&&power.allow&&
                               (uint32_t)(now-usb_since)>=1000;
            if (sf_uart_enable(physical_usb_ready)) {
                physical_usb_ready=false; sf_charge_gate(NULL,false);
                charger.fault=true; sensor_errors++;
            }
            power.usb_alive=sf_link_select(&link,now,physical_usb_ready,false)==SF_USB;
            err=sf_charger_step(&charger,&power,now);
            if (err&&!charger_reported) { charger_reported=true; queue_event(SF_SENSOR_FAULT,7,now); }
            last_power=now;
        }
        receive(now);
        int pressed=sf_button_pressed();
        if (pressed>=0&&(pressed!=0)!=button_raw) { button_raw=pressed!=0; button_changed=now; }
        if (button_raw!=button_stable&&(uint32_t)(now-button_changed)>=30) {
            button_stable=button_raw; if (button_stable) queue_event(SF_SOS,0,now);
        }
        if (IS_ENABLED(CONFIG_SF_IMU)&&imu_ok&&(uint32_t)(now-last_imu)>=20) {
            struct sf_sample samples[64]; size_t n=0;
            err=sf_imu_read(&imu,samples,ARRAY_SIZE(samples),&n,now);
            if (err) {
                sensor_errors++; sf_fall_reset(&fall); queue_event(SF_SENSOR_FAULT,2,now);
                if (err!=-EOVERFLOW) imu_ok=false;
            } else for (size_t i=0;i<n;i++) {
                enum sf_event e=sf_fall_push(&fall,samples+i);
                if (e) queue_event(e,0,samples[i].time_ms);
                if ((telemetry_count++%CONFIG_SF_STREAM_DIVISOR)==0) raw_imu(samples+i);
            }
            last_imu=now;
        }
        if (IS_ENABLED(CONFIG_SF_IMU)&&!imu_ok&&(uint32_t)(now-last_retry)>=5000) {
            /* Retrying a full config image can take ~1s. Keep charging disabled
             * during a recovery and latch the charger fault until reset. */
            sf_charge_gate(NULL,false); charger.fault=true;
            imu_ok=sf_imu_init(&imu,&bus,IS_ENABLED(CONFIG_SF_GYRO))==0;
            sf_fall_reset(&fall); last_retry=k_uptime_get_32();
        }
        if (!IS_ENABLED(CONFIG_SF_IMU)&&imu_shutdown_failed&&
            (uint32_t)(now-last_retry)>=5000) {
            /* A transient bus fault must not leave an old measurement running.
             * This retries only shutdown, never the configuration upload. */
            imu_shutdown_failed=sf_imu_shutdown(&bus)!=0;
            last_retry=k_uptime_get_32();
        }
        bool want_ppg=IS_ENABLED(CONFIG_SF_PPG)&&(power.present||power.battery_mv>=3300);
        if ((!want_ppg&&ppg_ok)||ppg_needs_off) ppg_stop(now);
        if (want_ppg&&!ppg_ok&&!ppg_failed&&!ppg_needs_off) {
            err=sf_ppg_power(true);
            if (!err) { k_msleep(20); err=sf_ppg_init(&bus); }
            if (err) {
                ppg_fault(now); ppg_stop(now);
            } else ppg_ok=true;
        }
        if (IS_ENABLED(CONFIG_SF_PPG)&&ppg_ok&&(uint32_t)(now-last_ppg)>=100) {
            struct sf_ppg_word words[32]; size_t n=0; bool gap=false;
            err=sf_ppg_read(&bus,words,ARRAY_SIZE(words),&n,&gap);
            if (err) {
                ppg_fault(now); ppg_stop(now);
            } else for (size_t i=0;i<n;i++) {
                struct sf_packet p=packet(SF_PPG,now-(uint32_t)(n-1-i)*20u);
                p.length=6; sf_put32(p.payload,words[i].value); p.payload[4]=words[i].tag;
                p.payload[5]=SF_TIME_ESTIMATED|(words[i].saturated?SF_SATURATED:0)|(gap?SF_DATA_GAP:0);
                enum sf_transport dst=route(now);
                if (dst!=SF_NO_LINK) (void)sf_transport_send(dst,&p);
            }
            if (gap) sensor_errors++;
            last_ppg=now;
        }
        enum sf_transport dst=route(now);
        if (dst!=SF_NO_LINK) {
            const struct sf_packet *p=sf_reliable_due(&reliable,now);
            if (p) (void)sf_transport_send(dst,p);
        }
        history_send(now);
        if ((uint32_t)(now-last_status)>=2000) { status(now); last_status=now; }
        bool alarm=(int32_t)(led_until-now)>0;
        bool fault=charger.fault||(IS_ENABLED(CONFIG_SF_IMU)&&!imu_ok)||
            imu_shutdown_failed||ppg_failed||reliable.overflow||context_overflows;
        sf_led(alarm||(fault&&now%2000<80));
        if (sf_watchdog_feed()) { sf_charge_gate(NULL,false); return -EIO; }
        /* System ON idle preserves the FIFO and BLE link; no busy-poll loop. */
        (void)k_sem_take(&sf_wake,K_MSEC(20));
    }
}
