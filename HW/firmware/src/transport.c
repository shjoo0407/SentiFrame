/* SPDX-License-Identifier: MIT */
#include "sf_platform.h"
#include "sf_rx.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
LOG_MODULE_REGISTER(sf_transport,LOG_LEVEL_INF);
#define UUID(n) BT_UUID_128_ENCODE(0x0f54f000u+(n),0xe1b4,0x47d4,0xbded,0x55c810ec9020)
static struct bt_uuid_128 service_uuid=BT_UUID_INIT_128(UUID(1));
static struct bt_uuid_128 tx_uuid=BT_UUID_INIT_128(UUID(2));
static struct bt_uuid_128 rx_uuid=BT_UUID_INIT_128(UUID(3));
static struct bt_conn *peer;
K_MUTEX_DEFINE(peer_lock);
static atomic_t subscribed, encrypted, tx_dropped;
static struct sf_rx_buffer rxq;
static struct k_spinlock rx_lock;
struct tx_item { enum sf_transport transport; uint16_t length; uint8_t bytes[SF_FRAME_MAX]; };
K_MSGQ_DEFINE(txq,sizeof(struct tx_item),16,4);
void sf_rx_enqueue(enum sf_transport src, const uint8_t *bytes, size_t len) {
    k_spinlock_key_t key=k_spin_lock(&rx_lock);
    (void)sf_rx_buffer_enqueue(&rxq,src,bytes,len);
    k_spin_unlock(&rx_lock,key);
    k_sem_give(&sf_wake);
}
static void rx_gap(enum sf_transport src, size_t lost) {
    k_spinlock_key_t key=k_spin_lock(&rx_lock);
    sf_rx_buffer_gap(&rxq,src,lost);
    k_spin_unlock(&rx_lock,key);
    k_sem_give(&sf_wake);
}
void sf_rx_reset(enum sf_transport src) { rx_gap(src,0); }
bool sf_rx_pop(struct sf_rx_chunk *c) {
    k_spinlock_key_t key=k_spin_lock(&rx_lock);
    bool found=sf_rx_buffer_pop(&rxq,c);
    k_spin_unlock(&rx_lock,key); return found;
}
uint32_t sf_rx_drops(void) {
    k_spinlock_key_t key=k_spin_lock(&rx_lock);
    uint32_t lost=rxq.dropped_bytes;
    k_spin_unlock(&rx_lock,key); return lost;
}
uint32_t sf_tx_drops(void) { return (uint32_t)atomic_get(&tx_dropped); }
static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr); atomic_set(&subscribed,value==BT_GATT_CCC_NOTIFY); k_sem_give(&sf_wake);
}
static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *data, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(attr);
    if (offset||flags&BT_GATT_WRITE_FLAG_PREPARE) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (len>SF_FRAME_MAX) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (bt_conn_get_security(conn)<BT_SECURITY_L2) return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
    sf_rx_enqueue(SF_BLE,data,len); return len;
}
BT_GATT_SERVICE_DEFINE(sf_service,
    BT_GATT_PRIMARY_SERVICE(&service_uuid),
    BT_GATT_CHARACTERISTIC(&tx_uuid.uuid,BT_GATT_CHRC_NOTIFY,0,NULL,NULL,NULL),
    BT_GATT_CCC(ccc_changed,BT_GATT_PERM_READ_ENCRYPT|BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&rx_uuid.uuid,BT_GATT_CHRC_WRITE|BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT,NULL,rx_write,NULL));
static const struct bt_data advertising[]={
    BT_DATA_BYTES(BT_DATA_FLAGS,BT_LE_AD_GENERAL|BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL,UUID(1))};
static const struct bt_data scan_response[]={
    BT_DATA(BT_DATA_NAME_COMPLETE,CONFIG_BT_DEVICE_NAME,sizeof(CONFIG_BT_DEVICE_NAME)-1)};
static void advertise(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(adv_work,advertise);
static void advertise(struct k_work *work) {
    ARG_UNUSED(work);
    int err=bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,800,1600,NULL),
         advertising,ARRAY_SIZE(advertising),scan_response,ARRAY_SIZE(scan_response));
    if (err&&err!=-EALREADY) {
        LOG_WRN("Advertising retry: %d",err); k_work_reschedule(&adv_work,K_SECONDS(1));
    }
}
static void connected(struct bt_conn *c, uint8_t err) {
    if (err) { k_work_reschedule(&adv_work,K_MSEC(500)); return; }
    k_mutex_lock(&peer_lock,K_FOREVER); peer=bt_conn_ref(c); k_mutex_unlock(&peer_lock);
    atomic_set(&encrypted,0); atomic_set(&subscribed,0); rx_gap(SF_BLE,0);
    int rc=bt_conn_set_security(c,BT_SECURITY_L2);
    if (rc) LOG_WRN("Pairing/security request: %d",rc);
    k_sem_give(&sf_wake);
}
static void disconnected(struct bt_conn *c, uint8_t reason) {
    ARG_UNUSED(reason);
    atomic_set(&encrypted,0); atomic_set(&subscribed,0); rx_gap(SF_BLE,0);
    k_mutex_lock(&peer_lock,K_FOREVER);
    if (peer==c) { bt_conn_unref(peer); peer=NULL; }
    k_mutex_unlock(&peer_lock);
    k_work_reschedule(&adv_work,K_MSEC(500)); k_sem_give(&sf_wake);
}
static void security_changed(struct bt_conn *c, bt_security_t level, enum bt_security_err err) {
    ARG_UNUSED(c); atomic_set(&encrypted,!err&&level>=BT_SECURITY_L2); k_sem_give(&sf_wake);
}
BT_CONN_CB_DEFINE(connection_callbacks)={.connected=connected,.disconnected=disconnected,
                                        .security_changed=security_changed};
bool sf_ble_ready(void) { return atomic_get(&subscribed)&&atomic_get(&encrypted); }
static void uart_irq(const struct device *dev, void *data) {
    ARG_UNUSED(data);
    if (!uart_irq_update(dev)) return;
    /* ERRORSRC survives the Nordic ISR's EVENT_ERROR clear. The exact number
     * of bytes lost to a hardware overrun is unknown: count at least one and
     * force parser resynchronization instead of joining across that gap. */
    if (uart_err_check(dev)) rx_gap(SF_USB,1);
    while (uart_irq_rx_ready(dev)) {
        uint8_t bytes[32]; int n=uart_fifo_read(dev,bytes,sizeof(bytes));
        if (n<=0) break;
        sf_rx_enqueue(SF_USB,bytes,(size_t)n);
    }
}
int sf_transport_send(enum sf_transport transport, const struct sf_packet *packet) {
    if (transport!=SF_USB&&transport!=SF_BLE) return -ENOTCONN;
    struct tx_item tx={.transport=transport};
    tx.length=(uint16_t)sf_encode(packet,tx.bytes,sizeof(tx.bytes));
    if (!tx.length) return -EINVAL;
    int err=k_msgq_put(&txq,&tx,K_NO_WAIT);
    if (err) atomic_inc(&tx_dropped);
    return err;
}
static int send_ble(const struct tx_item *tx) {
    if (!sf_ble_ready()) return -ENOTCONN;
    k_mutex_lock(&peer_lock,K_FOREVER);
    struct bt_conn *c=peer?bt_conn_ref(peer):NULL;
    k_mutex_unlock(&peer_lock);
    if (!c) return -ENOTCONN;
    int err=0;
    /* Always use <=20-byte fragments: even the default ATT MTU=23 works.
     * The host reconstructs the exact same CRC-protected byte stream as UART. */
    for (size_t at=0;at<tx->length&&!err;at+=20) {
        size_t n=tx->length-at; if (n>20) n=20;
        err=bt_gatt_notify(c,&sf_service.attrs[2],tx->bytes+at,(uint16_t)n);
    }
    bt_conn_unref(c); return err;
}
static void tx_worker(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    struct tx_item tx;
    while (true) {
        k_msgq_get(&txq,&tx,K_FOREVER);
        int err=0;
        if (tx.transport==SF_BLE) err=send_ble(&tx);
        else {
            struct sf_power_input power={0};
            err=sf_read_inputs(&power);
            if (!err&&(!power.present||!power.awake||power.switch_open)) err=-ENOTCONN;
            if (!err) err=sf_uart_write(tx.bytes,tx.length);
        }
        if (err) atomic_inc(&tx_dropped);
    }
}
K_THREAD_DEFINE(tx_thread,3072,tx_worker,NULL,NULL,NULL,7,0,0);
int sf_transport_init(void) {
    int err=uart_irq_callback_user_data_set(sf_uart_device(),uart_irq,NULL);
    if (err) return err;
    err=bt_enable(NULL);
    if (!err) err=settings_load();
    if (!err) k_work_reschedule(&adv_work,K_NO_WAIT);
    return err;
}
