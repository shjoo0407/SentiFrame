/* SPDX-License-Identifier: MIT */
/* Host substitutes for SDK services only. Production transport.c/rx.c and
 * protocol.c are compiled unchanged; this is not a radio or UART HW model. */
#ifndef SF_TRANSPORT_STUBS_H
#define SF_TRANSPORT_STUBS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>

#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CONFIG_BT_DEVICE_NAME "SentiFrameFixture"
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(x) (x)
#define K_SECONDS(x) ((x)*1000)

struct device { unsigned id; };
struct k_spinlock { unsigned dummy; };
typedef unsigned k_spinlock_key_t;
extern pthread_mutex_t sf_stub_spin_mutex;
extern _Thread_local unsigned sf_stub_spin_depth;
extern atomic_uint sf_stub_spin_calls;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock) {
    ARG_UNUSED(lock); assert(!sf_stub_spin_depth);
    assert(!pthread_mutex_lock(&sf_stub_spin_mutex)); sf_stub_spin_depth=1;
    atomic_fetch_add(&sf_stub_spin_calls,1); return 0x5258;
}
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key) {
    ARG_UNUSED(lock); assert(sf_stub_spin_depth==1&&key==0x5258);
    sf_stub_spin_depth=0; assert(!pthread_mutex_unlock(&sf_stub_spin_mutex));
}
struct k_mutex { pthread_mutex_t mutex; };
#define K_MUTEX_DEFINE(name) struct k_mutex name={PTHREAD_MUTEX_INITIALIZER}
static inline void k_mutex_lock(struct k_mutex *lock, int timeout) {
    ARG_UNUSED(timeout); assert(!pthread_mutex_lock(&lock->mutex));
}
static inline void k_mutex_unlock(struct k_mutex *lock) { assert(!pthread_mutex_unlock(&lock->mutex)); }
struct k_sem { atomic_uint count; };
#define K_SEM_DEFINE(name, initial, limit) struct k_sem name={ATOMIC_VAR_INIT(initial)}
static inline void k_sem_give(struct k_sem *sem) { atomic_fetch_add(&sem->count,1); }
struct k_msgq { size_t item_size, count, capacity; uint8_t data[4096]; };
#define K_MSGQ_DEFINE(name, size, count, align) struct k_msgq name={.item_size=(size),.capacity=(count)}
static inline int k_msgq_put(struct k_msgq *q, const void *data, int timeout) {
    ARG_UNUSED(timeout);
    if (q->count==q->capacity) return -ENOSPC;
    assert(q->item_size*(q->count+1)<=sizeof(q->data));
    memcpy(q->data+q->item_size*q->count++,data,q->item_size); return 0;
}
static inline int k_msgq_get(struct k_msgq *q, void *data, int timeout) {
    ARG_UNUSED(timeout); if (!q->count) return -ENOMSG;
    memcpy(data,q->data,q->item_size); q->count--;
    memmove(q->data,q->data+q->item_size,q->item_size*q->count); return 0;
}
struct k_work { unsigned dummy; };
struct k_work_delayable { struct k_work work; void (*handler)(struct k_work *); };
#define K_WORK_DELAYABLE_DEFINE(name, callback) struct k_work_delayable name={.handler=(callback)}
static inline int k_work_reschedule(struct k_work_delayable *work, int delay) {
    ARG_UNUSED(work); ARG_UNUSED(delay); return 0;
}
#define K_THREAD_DEFINE(name, stack, fn, a, b, c, prio, options, delay) \
    static void (*const name)(void *,void *,void *) __attribute__((unused))=(fn)

typedef atomic_int atomic_t;
static inline int atomic_get(const atomic_t *a) { return atomic_load(a); }
static inline void atomic_set(atomic_t *a, int v) { atomic_store(a,v); }
static inline void atomic_inc(atomic_t *a) { atomic_fetch_add(a,1); }

extern const uint8_t *sf_stub_uart_bytes;
extern size_t sf_stub_uart_count, sf_stub_uart_at;
extern int sf_stub_uart_error;
static inline int uart_irq_update(const struct device *dev) { ARG_UNUSED(dev); return 1; }
static inline int uart_irq_rx_ready(const struct device *dev) {
    ARG_UNUSED(dev); return sf_stub_uart_at<sf_stub_uart_count;
}
static inline int uart_fifo_read(const struct device *dev, uint8_t *data, int capacity) {
    ARG_UNUSED(dev); assert(capacity>0);
    if (sf_stub_uart_at==sf_stub_uart_count) return 0;
    *data=sf_stub_uart_bytes[sf_stub_uart_at++]; return 1; /* actual selected SDK semantics */
}
static inline int uart_err_check(const struct device *dev) {
    ARG_UNUSED(dev); int errors=sf_stub_uart_error; sf_stub_uart_error=0; return errors;
}
static inline int uart_irq_callback_user_data_set(const struct device *dev,
                                                void (*fn)(const struct device *,void *), void *data) {
    ARG_UNUSED(dev); ARG_UNUSED(fn); ARG_UNUSED(data); return 0;
}
static inline int settings_load(void) { return 0; }

struct bt_uuid { unsigned dummy; };
struct bt_uuid_128 { struct bt_uuid uuid; };
#define BT_UUID_128_ENCODE(...) 0
#define BT_UUID_INIT_128(...) {{0}}
struct bt_conn { unsigned refs; };
typedef unsigned bt_security_t;
enum bt_security_err { BT_SECURITY_ERR_SUCCESS=0 };
#define BT_SECURITY_L2 2
struct bt_conn_cb {
    void (*connected)(struct bt_conn *,uint8_t);
    void (*disconnected)(struct bt_conn *,uint8_t);
    void (*security_changed)(struct bt_conn *,bt_security_t,enum bt_security_err);
};
#define BT_CONN_CB_DEFINE(name) struct bt_conn_cb name
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { c->refs++; return c; }
static inline void bt_conn_unref(struct bt_conn *c) { assert(c->refs); c->refs--; }
static inline int bt_conn_get_security(struct bt_conn *c) { ARG_UNUSED(c); return BT_SECURITY_L2; }
static inline int bt_conn_set_security(struct bt_conn *c, unsigned level) {
    ARG_UNUSED(c); ARG_UNUSED(level); return 0;
}
struct bt_gatt_attr {
    const void *user;
    ssize_t (*write)(struct bt_conn *,const struct bt_gatt_attr *,const void *,uint16_t,uint16_t,uint8_t);
    void (*ccc)(const struct bt_gatt_attr *,uint16_t);
};
struct bt_service_stub { struct bt_gatt_attr attrs[6]; };
#define BT_GATT_PRIMARY_SERVICE(id) {.user=(id)}
#define BT_GATT_CHARACTERISTIC(id,props,perm,rdfn,wrfn,data) {.user=(id),.write=(wrfn)}
#define BT_GATT_CCC(fn,perm) {.ccc=(fn)}
#define BT_GATT_SERVICE_DEFINE(name,...) static struct bt_service_stub name={.attrs={__VA_ARGS__}}
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_WRITE_FLAG_PREPARE 1
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_AUTHENTICATION 5
#define BT_GATT_ERR(code) (-(code))
#define BT_GATT_CHRC_NOTIFY 1
#define BT_GATT_CHRC_WRITE 2
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 4
#define BT_GATT_PERM_READ_ENCRYPT 1
#define BT_GATT_PERM_WRITE_ENCRYPT 2
static inline int bt_gatt_notify(struct bt_conn *c, const struct bt_gatt_attr *attr,
                                 const void *data, uint16_t len) {
    ARG_UNUSED(c); ARG_UNUSED(attr); ARG_UNUSED(data); ARG_UNUSED(len); return 0;
}
struct bt_data { unsigned type; };
#define BT_DATA_BYTES(kind,...) {.type=(kind)}
#define BT_DATA(kind,data,len) {.type=(kind)}
#define BT_DATA_FLAGS 1
#define BT_LE_AD_GENERAL 2
#define BT_LE_AD_NO_BREDR 4
#define BT_DATA_UUID128_ALL 7
#define BT_DATA_NAME_COMPLETE 9
#define BT_LE_ADV_OPT_CONN 1
#define BT_LE_ADV_PARAM(...) NULL
static inline int bt_le_adv_start(const void *params, const struct bt_data *ad,size_t na,
                                  const struct bt_data *scan,size_t ns) {
    ARG_UNUSED(params); ARG_UNUSED(ad); ARG_UNUSED(na); ARG_UNUSED(scan); ARG_UNUSED(ns); return 0;
}
static inline int bt_enable(void *callback) { ARG_UNUSED(callback); return 0; }
#endif
