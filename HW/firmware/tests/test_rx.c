/* SPDX-License-Identifier: MIT */
#include "sf_core.h"
#include "sf_rx.h"
#include "sf_transport_stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>

pthread_mutex_t sf_stub_spin_mutex=PTHREAD_MUTEX_INITIALIZER;
_Thread_local unsigned sf_stub_spin_depth;
atomic_uint sf_stub_spin_calls;
const uint8_t *sf_stub_uart_bytes;
size_t sf_stub_uart_count,sf_stub_uart_at;
int sf_stub_uart_error;
static const struct device uart_device={1};
K_SEM_DEFINE(sf_wake,0,1);
const struct device *sf_uart_device(void) { return &uart_device; }
int sf_read_inputs(struct sf_power_input *p) { (void)p; return -ENOTCONN; }
int sf_uart_write(const uint8_t *p,size_t n) { (void)p; (void)n; return -ENOTCONN; }

/* Compile the actual production ISR, spinlock wrappers and BLE callbacks.
 * Only external SDK services are substituted above/in rx_stubs/. */
#include "../src/transport.c"

static atomic_ullong assertions;
static unsigned scenarios;
#define CHECK(x) do { atomic_fetch_add(&assertions,1); if (!(x)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

static void reset_wrapper(void) {
    CHECK(!sf_stub_spin_depth); CHECK(peer==NULL);
    memset(&rxq,0,sizeof(rxq));
    sf_stub_uart_error=0; sf_stub_uart_at=sf_stub_uart_count=0;
    atomic_store(&sf_stub_spin_calls,0);
}
static size_t encode(uint8_t kind,uint32_t sequence,uint8_t *out,size_t payload) {
    struct sf_packet packet={.type=kind,.seq=sequence,.boot=0x13579bdf,.time_ms=1234,
                             .length=(uint16_t)payload};
    for (size_t i=0;i<payload;i++) packet.payload[i]=(uint8_t)(i*17+3);
    return sf_encode(&packet,out,SF_FRAME_MAX);
}
static unsigned parse(struct sf_parser *p,const struct sf_rx_chunk *chunk,
                       uint32_t *last_sequence) {
    if (chunk->gap) memset(p,0,sizeof(*p));
    unsigned count=0;
    for (unsigned i=0;i<chunk->length;i++) {
        struct sf_packet packet;
        if (sf_parser_push(p,chunk->bytes[i],&packet)) {
            count++; *last_sequence=packet.seq;
        }
    }
    return count;
}
static void uart_byte(uint8_t byte) {
    sf_stub_uart_bytes=&byte; sf_stub_uart_at=0; sf_stub_uart_count=1;
    uart_irq(&uart_device,NULL);
    CHECK(sf_stub_uart_at==1); /* one real-SDK-style byte per IRQ */
}
static void two_acks_during_main_pause(void) {
    scenarios++; reset_wrapper();
    uint8_t first[SF_FRAME_MAX],second[SF_FRAME_MAX];
    size_t n=encode(SF_ACK,41,first,4),m=encode(SF_ACK,42,second,4);
    CHECK(n==24&&m==24);
    /* 48 bytes at 115200 8N1 take 4.17ms. Do not drain until both have
     * arrived, as during the real ADC's 5ms wait or an I2C transaction. */
    for (size_t i=0;i<n;i++) uart_byte(first[i]);
    for (size_t i=0;i<m;i++) uart_byte(second[i]);
    CHECK(sf_rx_drops()==0);
    struct sf_rx_chunk chunk; struct sf_parser parser={0}; uint32_t last=0;
    CHECK(sf_rx_pop(&chunk)); CHECK(chunk.transport==SF_USB&&!chunk.gap&&chunk.length==48);
    CHECK(parse(&parser,&chunk,&last)==2&&last==42); CHECK(!sf_rx_pop(&chunk));
    CHECK(atomic_load(&sf_stub_spin_calls)>=51);
}
static void maximum_frame_and_owned_copy(void) {
    scenarios++; reset_wrapper();
    uint8_t raw[SF_FRAME_MAX]; size_t n=encode(SF_IMU,75,raw,SF_PAYLOAD_MAX);
    CHECK(n==SF_FRAME_MAX);
    for (size_t i=0;i<n;i++) uart_byte(raw[i]);
    struct sf_rx_chunk chunk; CHECK(sf_rx_pop(&chunk));
    CHECK(chunk.length==SF_FRAME_MAX&&!memcmp(chunk.bytes,raw,n));
    uint8_t preserved[SF_FRAME_MAX]; memcpy(preserved,chunk.bytes,n);
    memset(raw,0x55,sizeof(raw));
    for (unsigned i=0;i<200;i++) sf_rx_enqueue(SF_USB,raw,sizeof(raw));
    CHECK(!memcmp(chunk.bytes,preserved,n)); /* no pointers into writable ring */
    struct sf_parser parser={0}; uint32_t last=0;
    CHECK(parse(&parser,&chunk,&last)==1&&last==75);
}
static uint8_t pattern(size_t i) { return (uint8_t)(i*13+7); }
static void ring_wrap_and_exact_full(void) {
    scenarios++;
    struct sf_rx_buffer q={0}; uint8_t bytes[SF_RX_USB_CAPACITY];
    for (size_t i=0;i<sizeof(bytes);i++) bytes[i]=pattern(i);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,sizeof(bytes)));
    CHECK(q.usb.size==SF_RX_USB_CAPACITY&&q.dropped_bytes==0);
    size_t at=0; struct sf_rx_chunk chunk;
    while (sf_rx_buffer_pop(&q,&chunk)) {
        CHECK(!chunk.gap&&chunk.transport==SF_USB&&chunk.length<=SF_FRAME_MAX);
        for (unsigned i=0;i<chunk.length;i++) CHECK(chunk.bytes[i]==pattern(at++));
    }
    CHECK(at==sizeof(bytes)&&q.usb.size==0);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,4000)); at=0;
    for (unsigned n=0;n<20;n++) {
        CHECK(sf_rx_buffer_pop(&q,&chunk));
        for (unsigned i=0;i<chunk.length;i++) CHECK(chunk.bytes[i]==pattern(at++));
    }
    for (unsigned i=0;i<2000;i++) bytes[i]=pattern(4000+i);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,2000));
    while (sf_rx_buffer_pop(&q,&chunk)) {
        CHECK(!chunk.gap);
        for (unsigned i=0;i<chunk.length;i++) CHECK(chunk.bytes[i]==pattern(at++));
    }
    CHECK(at==6000&&q.dropped_bytes==0);
}
static void zero_length_gap_and_first_bytes(void) {
    scenarios++;
    struct sf_rx_buffer q={0}; uint8_t bytes[SF_RX_USB_CAPACITY]={0};
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,sizeof(bytes)));
    CHECK(!sf_rx_buffer_enqueue(&q,SF_USB,bytes,1));
    CHECK(q.dropped_bytes==SF_RX_USB_CAPACITY+1&&q.gaps==1);
    struct sf_rx_chunk chunk; CHECK(sf_rx_buffer_pop(&q,&chunk));
    CHECK(chunk.gap&&!chunk.length&&chunk.transport==SF_USB);
    CHECK(!sf_rx_buffer_pop(&q,&chunk));
    uint8_t one=0xa5; CHECK(sf_rx_buffer_enqueue(&q,SF_USB,&one,1));
    CHECK(sf_rx_buffer_pop(&q,&chunk)); CHECK(!chunk.gap&&chunk.length==1&&chunk.bytes[0]==one);
    sf_rx_buffer_gap(&q,SF_USB,0);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,&one,1));
    CHECK(sf_rx_buffer_pop(&q,&chunk)); CHECK(chunk.gap&&chunk.length==1&&chunk.bytes[0]==one);
}
static void cannot_join_across_overrun(void) {
    scenarios++; reset_wrapper();
    uint8_t frame[SF_FRAME_MAX],padding[SF_RX_USB_CAPACITY]={0};
    size_t n=encode(SF_HELLO,99,frame,0); CHECK(n==20);
    sf_rx_enqueue(SF_USB,frame,10);
    struct sf_parser parser={0}; struct sf_rx_chunk chunk; uint32_t last=0;
    CHECK(sf_rx_pop(&chunk)); CHECK(parse(&parser,&chunk,&last)==0&&parser.used==10);
    sf_rx_enqueue(SF_USB,padding,sizeof(padding));
    sf_rx_enqueue(SF_USB,padding,1); /* explicit overrun; queued prefix is discarded */
    sf_rx_enqueue(SF_USB,frame+10,n-10);
    CHECK(sf_rx_pop(&chunk)); CHECK(chunk.gap&&chunk.length==n-10);
    CHECK(parse(&parser,&chunk,&last)==0); /* old valid CRC suffix cannot complete old header */
    sf_rx_enqueue(SF_USB,frame,n);
    CHECK(sf_rx_pop(&chunk)); CHECK(parse(&parser,&chunk,&last)==1&&last==99);
    CHECK(sf_rx_drops()==SF_RX_USB_CAPACITY+1);
}
static void isolation_and_fairness(void) {
    scenarios++;
    struct sf_rx_buffer q={0}; uint8_t bytes[SF_RX_USB_CAPACITY]; memset(bytes,0x3c,sizeof(bytes));
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,348));
    CHECK(sf_rx_buffer_enqueue(&q,SF_BLE,bytes,348));
    struct sf_rx_chunk chunk;
    for (unsigned i=0;i<6;i++) {
        CHECK(sf_rx_buffer_pop(&q,&chunk));
        CHECK(chunk.transport==(i%2?SF_BLE:SF_USB)&&chunk.length==SF_FRAME_MAX&&!chunk.gap);
    }
    CHECK(!sf_rx_buffer_pop(&q,&chunk));
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,sizeof(bytes)));
    CHECK(sf_rx_buffer_enqueue(&q,SF_BLE,bytes,17));
    CHECK(!sf_rx_buffer_enqueue(&q,SF_USB,bytes,1));
    CHECK(q.ble.size==17&&!q.ble.gap);
    unsigned ble=0,usb=0;
    while (sf_rx_buffer_pop(&q,&chunk)) {
        if (chunk.transport==SF_BLE) { CHECK(chunk.length==17&&!chunk.gap); ble++; }
        else { CHECK(!chunk.length&&chunk.gap); usb++; }
    }
    CHECK(ble==1&&usb==1);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,bytes,7));
    CHECK(sf_rx_buffer_enqueue(&q,SF_BLE,bytes,SF_RX_BLE_CAPACITY));
    CHECK(!sf_rx_buffer_enqueue(&q,SF_BLE,bytes,1));
    CHECK(q.usb.size==7&&!q.usb.gap);
}
static void invalid_input_and_saturated_counters(void) {
    scenarios++;
    struct sf_rx_buffer q={0}; uint8_t byte=1; struct sf_rx_chunk chunk;
    CHECK(!sf_rx_buffer_enqueue(NULL,SF_USB,&byte,1));
    CHECK(!sf_rx_buffer_pop(NULL,&chunk)); CHECK(!sf_rx_buffer_pop(&q,NULL));
    sf_rx_buffer_gap(NULL,SF_USB,1);
    CHECK(sf_rx_buffer_enqueue(&q,SF_USB,&byte,1));
    CHECK(!sf_rx_buffer_pop(&q,NULL)&&q.usb.size==1);
    CHECK(sf_rx_buffer_pop(&q,&chunk)&&chunk.length==1&&chunk.bytes[0]==byte);
    CHECK(!sf_rx_buffer_enqueue(&q,SF_USB,NULL,0)); CHECK(q.dropped_bytes==0&&!q.usb.gap);
    CHECK(!sf_rx_buffer_enqueue(&q,SF_NO_LINK,&byte,8)); CHECK(q.dropped_bytes==8);
    CHECK(!sf_rx_buffer_enqueue(&q,SF_USB,NULL,3));
    CHECK(sf_rx_buffer_pop(&q,&chunk)&&chunk.gap&&!chunk.length);
    CHECK(q.dropped_bytes==11);
    q.dropped_bytes=UINT32_MAX-3; q.gaps=UINT32_MAX-1;
    sf_rx_buffer_gap(&q,SF_BLE,9);
    CHECK(q.dropped_bytes==UINT32_MAX&&q.gaps==UINT32_MAX);
    sf_rx_buffer_gap(&q,SF_USB,SIZE_MAX);
    CHECK(q.dropped_bytes==UINT32_MAX&&q.gaps==UINT32_MAX);
    CHECK(!sf_rx_buffer_enqueue(&q,SF_USB,&byte,SIZE_MAX));
    CHECK(q.dropped_bytes==UINT32_MAX&&q.usb.size==0);
}
static void hardware_error_and_ble_lifecycle(void) {
    scenarios++; reset_wrapper();
    uint8_t frame[SF_FRAME_MAX]; size_t n=encode(SF_HELLO,101,frame,0);
    struct sf_parser parser={0}; struct sf_rx_chunk chunk; uint32_t last=0;
    sf_rx_enqueue(SF_USB,frame,10); CHECK(sf_rx_pop(&chunk));
    CHECK(!parse(&parser,&chunk,&last));
    sf_rx_enqueue(SF_USB,frame,5); sf_stub_uart_error=3;
    uart_byte(frame[10]);
    for (size_t i=11;i<n;i++) uart_byte(frame[i]);
    CHECK(sf_rx_pop(&chunk)&&chunk.gap); CHECK(!parse(&parser,&chunk,&last));
    CHECK(sf_rx_drops()==6&&sf_stub_uart_error==0);
    sf_rx_enqueue(SF_USB,frame,n); CHECK(sf_rx_pop(&chunk));
    CHECK(parse(&parser,&chunk,&last)==1&&last==101);
    sf_rx_enqueue(SF_USB,frame,7); sf_rx_enqueue(SF_BLE,frame,10);
    struct bt_conn connection={.refs=1}; connected(&connection,0);
    CHECK(connection.refs==2&&rxq.usb.size==7&&rxq.ble.size==0&&rxq.ble.gap);
    sf_rx_enqueue(SF_BLE,frame+10,n-10); disconnected(&connection,0);
    CHECK(connection.refs==1&&peer==NULL&&rxq.ble.size==0&&rxq.ble.gap);
    CHECK(rxq.usb.size==7);
    sf_rx_reset(SF_USB); CHECK(rxq.usb.size==0&&rxq.usb.gap);
}
static void long_burst_and_recovery(void) {
    scenarios++; reset_wrapper();
    for (unsigned i=0;i<100000;i++) uart_byte((uint8_t)i);
    CHECK(rxq.usb.size<=SF_RX_USB_CAPACITY&&sf_rx_drops()>0);
    struct sf_rx_chunk chunk; bool gap=false;
    while (sf_rx_pop(&chunk)) gap|=chunk.gap;
    CHECK(gap);
    uint8_t raw[SF_FRAME_MAX]; size_t n=encode(SF_HEARTBEAT,12345,raw,0);
    for (size_t i=0;i<n;i++) uart_byte(raw[i]);
    struct sf_parser parser={0}; uint32_t last=0; CHECK(sf_rx_pop(&chunk));
    CHECK(parse(&parser,&chunk,&last)==1&&last==12345);
}
struct producer_args { enum sf_transport transport; atomic_bool done; };
#define CONCURRENT_BYTES 50000u
static void *producer(void *arg) {
    struct producer_args *a=arg;
    for (size_t i=0;i<CONCURRENT_BYTES;i++) {
        for (;;) {
            k_spinlock_key_t key=k_spin_lock(&rx_lock);
            const struct sf_rx_lane *l=a->transport==SF_USB?&rxq.usb:&rxq.ble;
            size_t cap=a->transport==SF_USB?SF_RX_USB_CAPACITY:SF_RX_BLE_CAPACITY;
            bool free=l->size<cap; k_spin_unlock(&rx_lock,key);
            if (free) break;
            sched_yield();
        }
        uint8_t byte=pattern(i); sf_rx_enqueue(a->transport,&byte,1);
    }
    CHECK(!sf_stub_spin_depth); atomic_store(&a->done,true); return NULL;
}
static void concurrent_production_wrapper(void) {
    scenarios++; reset_wrapper();
    struct producer_args usb={.transport=SF_USB},ble={.transport=SF_BLE};
    pthread_t threads[2]; CHECK(!pthread_create(threads,NULL,producer,&usb));
    CHECK(!pthread_create(threads+1,NULL,producer,&ble));
    size_t seen_usb=0,seen_ble=0; struct sf_rx_chunk chunk;
    for (;;) {
        if (!sf_rx_pop(&chunk)) {
            /* A producer can enqueue its final byte after the first empty
             * observation but before publishing done. Recheck after done. */
            if (!atomic_load(&usb.done)||!atomic_load(&ble.done)) {
                sched_yield(); continue;
            }
            if (!sf_rx_pop(&chunk)) break;
        }
        CHECK(!chunk.gap&&chunk.length>0&&chunk.length<=SF_FRAME_MAX);
        size_t *seen=chunk.transport==SF_USB?&seen_usb:&seen_ble;
        for (unsigned i=0;i<chunk.length;i++) CHECK(chunk.bytes[i]==pattern((*seen)++));
    }
    CHECK(!pthread_join(threads[0],NULL)); CHECK(!pthread_join(threads[1],NULL));
    CHECK(seen_usb==CONCURRENT_BYTES&&seen_ble==CONCURRENT_BYTES&&sf_rx_drops()==0);
    CHECK(!sf_stub_spin_depth);
}
int main(void) {
    two_acks_during_main_pause(); maximum_frame_and_owned_copy(); ring_wrap_and_exact_full();
    zero_length_gap_and_first_bytes(); cannot_join_across_overrun(); isolation_and_fairness();
    invalid_input_and_saturated_counters(); hardware_error_and_ble_lifecycle();
    long_burst_and_recovery(); concurrent_production_wrapper();
    printf("{\"result\":\"PASS\",\"scenarios\":%u,\"assertions\":%llu,"
           "\"concurrent_bytes_per_transport\":%u}\n",scenarios,
           (unsigned long long)atomic_load(&assertions),CONCURRENT_BYTES);
    return 0;
}
