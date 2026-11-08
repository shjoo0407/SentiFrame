/* SPDX-License-Identifier: MIT */
/* Execute the real main.c with a deterministic HAL, never a copied loop. */
#include "sf_platform.h"
#include "sf_imu.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sf_application_main(void);
static unsigned assertions;
#define CHECK(test) do { assertions++; if (!(test)) { \
    fprintf(stderr,"FAIL app %s:%d: %s (scenario=%s, charging=%d imu=%d ppg=%d pack=%d)\n", \
        __FILE__,__LINE__,#test,scenario,CONFIG_SF_CHARGING,CONFIG_SF_IMU, \
        CONFIG_SF_PPG,CONFIG_SF_PACK_APPROVED); abort(); } } while (0)
static const char *scenario;
static uint32_t clock_ms=100;
static unsigned feed_count, imu_init_count, imu_shutdown_count, imu_read_count;
static unsigned ppg_init_count, ppg_read_count, ppg_on_count, ppg_off_count;
static unsigned charger_init_count, charger_step_count, hello_count, status_count;
static unsigned heartbeat_count, rx_gap_count, rx_byte_chunks, rx_reset_count;
static unsigned sos_count, fault_imu, fault_ppg, fault_charger, late_led, settled_led;
static bool ppg_powered, gate_enabled, hello_received, usb_edge_sent;
static uint8_t final_status;
static struct sf_packet acknowledgements[32];
static enum sf_transport ack_transport[32];
static size_t ack_head, ack_count;
static uint8_t rx_wire[3*SF_FRAME_MAX];
static size_t rx_wire_count, rx_wire_at, rx_gap_at;
static enum sf_transport rx_transport;
static bool rx_gap_pending;
struct k_sem sf_wake;

static bool is(const char *name) { return strcmp(scenario,name)==0; }
static bool usb_case(void) { return is("rx_usb_gap"); }
uint32_t k_uptime_get_32(void) { return clock_ms; }
void k_msleep(int32_t ms) { CHECK(ms>=0); clock_ms+=(uint32_t)ms; }
int k_sem_take(struct k_sem *sem, int32_t timeout) {
    CHECK(sem==&sf_wake&&timeout==20); clock_ms+=(uint32_t)timeout; return -EAGAIN;
}
uint32_t sys_rand32_get(void) { return 0xfeed1234u; }

int sf_board_init(struct sf_bus *bus) { memset(bus,0,sizeof(*bus)); return is("board_failure")?-EIO:0; }
int sf_watchdog_start(void) { return is("watchdog_failure")?-EIO:0; }
int sf_watchdog_feed(void) { return ++feed_count==700?-EIO:0; }
void sf_charge_gate(void *ctx, bool enabled) {
    (void)ctx;
    if (enabled) CHECK(CONFIG_SF_CHARGING&&CONFIG_SF_PACK_APPROVED);
    gate_enabled=enabled;
}
int sf_ppg_power(bool enabled) {
    if (enabled) {
        CHECK(CONFIG_SF_PPG); ppg_on_count++; ppg_powered=true;
    } else {
        ppg_off_count++;
        if (is("ppg_off_stuck")||(is("ppg_off_failure")&&ppg_off_count==1)) return -EIO;
        ppg_powered=false;
    }
    return 0;
}
void sf_led(bool enabled) {
    if (clock_ms>2000&&enabled) late_led++;
    if (clock_ms>7000&&enabled) settled_led++;
}
int sf_battery_mv(void) {
    if ((is("battery_cutoff")||is("ppg_off_failure")||is("ppg_off_stuck"))&&clock_ms>5000)
        return 3200;
    return 3700;
}
int sf_read_inputs(struct sf_power_input *input) {
    input->present=usb_case(); input->awake=true; input->allow=true;
    input->high=false; input->switch_open=false; return 0;
}
bool sf_take_power_change(void) {
    if (usb_case()&&!usb_edge_sent&&clock_ms>=1700) {
        usb_edge_sent=true; return true;
    }
    return false;
}
int sf_button_pressed(void) { return clock_ms>=500&&clock_ms<650; }
int sf_uart_enable(bool enabled) { if (!usb_case()) CHECK(!enabled); return 0; }
int sf_transport_init(void) { return is("transport_failure")?-EIO:0; }
bool sf_ble_ready(void) { return true; }
uint32_t sf_rx_drops(void) { return 0; }
uint32_t sf_tx_drops(void) { return 0; }
void sf_rx_reset(enum sf_transport transport) {
    CHECK(transport==SF_USB); rx_reset_count++;
    if (rx_transport==transport) { rx_wire_at=rx_wire_count; rx_gap_pending=false; }
    /* Retain the other transport's pending acknowledgements. */
    size_t kept=0, original=ack_count;
    for (size_t n=0;n<original;n++) {
        size_t index=(ack_head+n)%ARRAY_SIZE(acknowledgements);
        if (ack_transport[index]==transport) continue;
        size_t destination=(ack_head+kept)%ARRAY_SIZE(acknowledgements);
        acknowledgements[destination]=acknowledgements[index];
        ack_transport[destination]=ack_transport[index]; kept++;
    }
    ack_count=kept;
}

static void ack(const struct sf_packet *packet, enum sf_transport transport) {
    CHECK(ack_count<ARRAY_SIZE(acknowledgements));
    size_t index=(ack_head+ack_count)%ARRAY_SIZE(acknowledgements);
    acknowledgements[index]=(struct sf_packet){.type=SF_ACK,.length=4,.boot=packet->boot};
    ack_transport[index]=transport;
    sf_put32(acknowledgements[index].payload,packet->seq); ack_count++;
}
int sf_transport_send(enum sf_transport transport, const struct sf_packet *packet) {
    CHECK(transport==SF_BLE||(usb_case()&&transport==SF_USB));
    if (packet->type==SF_HELLO) {
        hello_count++; CHECK(packet->length==6);
        CHECK(packet->payload[2]==0&&packet->payload[3]==2&&packet->payload[4]==1);
        CHECK(packet->payload[5]==((CONFIG_SF_GYRO?1:0)|(CONFIG_SF_PPG?2:0)|
            (CONFIG_SF_PACK_APPROVED?4:0)|(CONFIG_SF_IMU?8:0)|(CONFIG_SF_CHARGING?16:0)));
        if (usb_case()) CHECK(transport==SF_USB);
    } else if (packet->type==SF_HEARTBEAT) {
        CHECK(packet->length==0); heartbeat_count++;
        if (usb_case()) CHECK(transport==SF_USB);
    } else if (packet->type==SF_IMU) {
        CHECK(CONFIG_SF_IMU); CHECK(packet->length==14);
    } else if (packet->type==SF_PPG) {
        CHECK(CONFIG_SF_PPG&&ppg_powered); CHECK(packet->length==6);
    } else if (packet->type==SF_STATUS) {
        status_count++; CHECK(packet->length==30); final_status=packet->payload[3];
        CHECK(((final_status&32)!=0)==(CONFIG_SF_PACK_APPROVED!=0));
        if (!CONFIG_SF_IMU) CHECK(!(final_status&4));
        if (!CONFIG_SF_PPG) CHECK(!(final_status&8));
        if (!CONFIG_SF_CHARGING||!CONFIG_SF_PACK_APPROVED) CHECK(!(final_status&1));
    } else if (packet->type==SF_EVENT) {
        if (packet->payload[0]==SF_SOS) sos_count++;
        if (packet->payload[0]==SF_SENSOR_FAULT) {
            if (packet->payload[1]==2) fault_imu++;
            else if (packet->payload[1]==3) fault_ppg++;
            else if (packet->payload[1]==7) fault_charger++;
            else CHECK(false);
        }
        ack(packet,transport);
    } else if (packet->type==SF_HISTORY) ack(packet,transport);
    return 0;
}
bool sf_rx_pop(struct sf_rx_chunk *chunk) {
    if (rx_wire_at==rx_wire_count) {
        struct sf_packet packet;
        rx_wire_at=0; rx_wire_count=0;
        if (ack_count) {
            packet=acknowledgements[ack_head]; rx_transport=ack_transport[ack_head];
            ack_head=(ack_head+1)%ARRAY_SIZE(acknowledgements); ack_count--;
        } else if (!hello_received&&clock_ms>=(usb_case()?1300u:200u)) {
            packet=(struct sf_packet){.type=SF_HELLO}; hello_received=true;
            rx_transport=usb_case()?SF_USB:SF_BLE;
            if (is("rx_ble_gap")||usb_case()) {
                /* The parser has already consumed a prefix when transport loss
                 * invalidates it. A zero-byte gap must reset before the old
                 * suffix, even if prefix+suffix happen to have a valid CRC. */
                packet.seq=0x11223344;
                rx_wire_count=sf_encode(&packet,rx_wire,sizeof(rx_wire));
                CHECK(rx_wire_count==20); rx_gap_at=rx_wire_count-2; rx_gap_pending=true;
                packet.seq=0x55667788;
                rx_wire_count+=sf_encode(&packet,rx_wire+rx_wire_count,sizeof(rx_wire)-rx_wire_count);
                packet=(struct sf_packet){.type=SF_HEARTBEAT,.seq=1,.boot=0xfeed1234};
                rx_wire_count+=sf_encode(&packet,rx_wire+rx_wire_count,sizeof(rx_wire)-rx_wire_count);
            }
        } else return false;
        if (!rx_wire_count) rx_wire_count=sf_encode(&packet,rx_wire,sizeof(rx_wire));
        CHECK(rx_wire_count>0&&rx_wire_count<=sizeof(rx_wire));
    }
    *chunk=(struct sf_rx_chunk){.transport=rx_transport};
    if (rx_gap_pending&&rx_wire_at==rx_gap_at) {
        chunk->gap=true; rx_gap_pending=false; rx_gap_count++; return true;
    }
    /* Arbitrary receive boundaries must never be interpreted as frame
     * boundaries; a 24-byte ACK crosses main's 24-chunk receive budget. */
    chunk->length=1; chunk->bytes[0]=rx_wire[rx_wire_at++]; rx_byte_chunks++;
    return true;
}

int sf_charger_init(struct sf_charger *charger, const struct sf_bus *bus,
                    void (*gate)(void *,bool), void *ctx) {
    charger_init_count++; memset(charger,0,sizeof(*charger));
    charger->initialized=true; charger->bus=*bus; charger->gate=gate; charger->gate_ctx=ctx;
    gate(ctx,false); return 0;
}
int sf_charger_step(struct sf_charger *charger, const struct sf_power_input *input, uint32_t now) {
    (void)now; charger_step_count++;
    CHECK(input->charging_requested==(CONFIG_SF_CHARGING!=0));
    CHECK(input->pack_approved==(CONFIG_SF_PACK_APPROVED!=0));
    charger->enabled=false; /* no external source in this fixture */
    return charger->fault?-EIO:0;
}
enum sf_source sf_classify_source(const struct sf_power_input *input) {
    return input->present?SF_SRC_UNKNOWN:SF_SRC_NONE;
}
int sf_cc_read(const struct sf_bus *bus, struct sf_power_input *input) {
    (void)bus; CHECK(usb_case()); input->cc_valid=true;
    input->cc_attached=2; input->cc_accessory=0; input->cc_current=0; return 0;
}
int sf_imu_init(struct sf_imu *imu, const struct sf_bus *bus, bool gyro) {
    (void)imu; (void)bus; CHECK(CONFIG_SF_IMU); CHECK(gyro==(CONFIG_SF_GYRO!=0));
    imu_init_count++; return is("imu_init_failure")?-EIO:0;
}
int sf_imu_shutdown(const struct sf_bus *bus) {
    (void)bus; CHECK(!CONFIG_SF_IMU); imu_shutdown_count++;
    return is("imu_shutdown_failure")||
        (is("imu_shutdown_once")&&imu_shutdown_count==1)?-EIO:0;
}
int sf_imu_read(struct sf_imu *imu, struct sf_sample *samples, size_t cap,
                size_t *count, uint32_t now) {
    (void)imu; CHECK(CONFIG_SF_IMU&&cap>=2); imu_read_count++;
    if (is("imu_read_failure")&&imu_read_count>=3) return -EIO;
    samples[0]=(struct sf_sample){.a={0,0,2048},.time_ms=now-10,.flags=0};
    samples[1]=(struct sf_sample){.a={0,0,2048},.time_ms=now,.flags=0};
    *count=2; return 0;
}
int sf_ppg_init(const struct sf_bus *bus) {
    (void)bus; CHECK(CONFIG_SF_PPG&&ppg_powered); ppg_init_count++;
    return is("ppg_init_failure")?-EIO:0;
}
int sf_ppg_read(const struct sf_bus *bus, struct sf_ppg_word *words, size_t cap,
                size_t *count, bool *gap) {
    (void)bus; CHECK(CONFIG_SF_PPG&&ppg_powered&&cap>=1); ppg_read_count++;
    if (is("ppg_read_failure")&&ppg_read_count>=3) return -EIO;
    words[0]=(struct sf_ppg_word){.value=1234,.tag=1}; *count=1; *gap=false; return 0;
}

int main(int argc, char **argv) {
    scenario=argc>1?argv[1]:"normal";
    CHECK(sf_application_main()==-EIO); CHECK(!gate_enabled);
    if (is("board_failure")||is("watchdog_failure")) {
        CHECK(charger_init_count==0&&imu_init_count==0&&ppg_init_count==0);
    } else if (is("transport_failure")) {
        CHECK(charger_init_count==1&&imu_init_count==0&&ppg_init_count==0);
    } else {
        CHECK(feed_count==700&&clock_ms>=14000);
        CHECK(charger_init_count==1&&charger_step_count>40);
        CHECK(hello_count==1&&status_count>=6&&sos_count>=1);
        CHECK(rx_byte_chunks>=44);
        if (is("rx_ble_gap")||usb_case()) CHECK(rx_gap_count==1&&heartbeat_count==1);
        else CHECK(rx_gap_count==0&&heartbeat_count==0);
        CHECK(rx_reset_count==(usb_case()?1u:0u));
        if (CONFIG_SF_IMU) {
            CHECK(imu_shutdown_count==0);
            if (is("imu_init_failure")) CHECK(imu_init_count>=3&&imu_read_count==0&&fault_imu>0);
            else if (is("imu_read_failure")) CHECK(imu_init_count>=3&&imu_read_count>=3&&fault_imu>0);
            else CHECK(imu_init_count==1&&imu_read_count>600&&fault_imu==0);
        } else {
            CHECK(imu_init_count==0&&imu_read_count==0);
            if (is("imu_shutdown_failure")) CHECK(imu_shutdown_count>=3&&settled_led>0);
            else if (is("imu_shutdown_once")) CHECK(imu_shutdown_count==2&&settled_led==0);
            else CHECK(imu_shutdown_count==1);
            CHECK((fault_imu>0)==(is("imu_shutdown_failure")||is("imu_shutdown_once")));
            CHECK(fault_charger==0); /* OFF recovery never starts a full upload or latches charging. */
        }
        if (CONFIG_SF_PPG) {
            CHECK(ppg_init_count==1&&ppg_on_count==1);
            if (is("ppg_init_failure")) CHECK(ppg_read_count==0&&fault_ppg>0&&!ppg_powered);
            else if (is("ppg_read_failure")) CHECK(ppg_read_count==3&&fault_ppg>0&&!ppg_powered);
            else CHECK(ppg_read_count>80);
            if (is("battery_cutoff")) CHECK(ppg_off_count==1&&!ppg_powered&&fault_ppg==0);
            if (is("ppg_off_failure")) CHECK(ppg_off_count>=2&&!ppg_powered&&fault_ppg>0);
            if (is("ppg_off_stuck")) CHECK(ppg_off_count>100&&ppg_powered&&fault_ppg>0);
        } else CHECK(ppg_init_count==0&&ppg_read_count==0&&ppg_on_count==0&&fault_ppg==0);
        if (is("normal")||is("battery_cutoff")||is("rx_ble_gap")||usb_case())
            CHECK(late_led==0&&fault_charger==0);
        if (is("imu_init_failure")||is("imu_read_failure")||is("imu_shutdown_failure")||
            is("imu_shutdown_once")||
            is("ppg_init_failure")||is("ppg_read_failure")||is("ppg_off_failure")||is("ppg_off_stuck"))
            CHECK(late_led>0);
    }
    printf("{\"scenario\":\"%s\",\"assertions\":%u,\"result\":\"PASS\"}\n",scenario,assertions);
    return 0;
}
