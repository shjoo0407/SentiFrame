/* SPDX-License-Identifier: MIT */
#include "sf_platform.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/irq.h>
#include <errno.h>

#define CTRL DT_NODELABEL(sf_control)
#define PIN(name, prop) static const struct gpio_dt_spec name=GPIO_DT_SPEC_GET(CTRL,prop)
PIN(charge_arm,charge_arm_gpios); PIN(ppg_en,ppg_enable_gpios);
PIN(led,led_gpios); PIN(motor,motor_gpios);
PIN(bat_en,battery_enable_gpios); PIN(uart_en,uart_enable_gpios);
PIN(usb_pg,usb_pg_gpios); PIN(suspend,suspend_gpios);
PIN(imu_int,imu_int_gpios); PIN(ppg_int,ppg_int_gpios);
PIN(button,button_gpios); PIN(bc_open,bc_open_gpios);
PIN(bc_allow,bc_allow_gpios); PIN(bc_high,bc_high_gpios);
PIN(chg_int,chg_int_gpios);
static const struct device *const i2c=DEVICE_DT_GET(DT_NODELABEL(i2c21));
static const struct device *const uart=DEVICE_DT_GET(DT_NODELABEL(uart20));
static const struct device *const wdt=DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct adc_dt_spec battery=ADC_DT_SPEC_GET(CTRL);
static struct gpio_callback callbacks[6];
static atomic_t power_changed;
static atomic_t uart_on;
static int watchdog_channel=-1;
K_SEM_DEFINE(sf_wake,0,1);
K_MUTEX_DEFINE(uart_lock);

static void input_irq(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    ARG_UNUSED(cb);
    if ((port==usb_pg.port&&(pins&BIT(usb_pg.pin)))||
        (port==suspend.port&&(pins&BIT(suspend.pin)))) {
        /* Revoke charge immediately; the main thread then writes CHG_DIS.
         * No I2C, BLE, ADC or parsing is performed in an ISR. */
        (void)gpio_pin_set_dt(&charge_arm,0);
        atomic_set(&power_changed,1);
    }
    k_sem_give(&sf_wake);
}
static int bus_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
    ARG_UNUSED(ctx); return i2c_burst_read(i2c,addr,reg,buf,n);
}
static int bus_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *buf, size_t n) {
    ARG_UNUSED(ctx); return i2c_burst_write(i2c,addr,reg,buf,n);
}
static void bus_delay(void *ctx, uint32_t ms) { ARG_UNUSED(ctx); k_msleep((int32_t)ms); }
int sf_board_init(struct sf_bus *bus) {
    const struct gpio_dt_spec *outputs[]={&charge_arm,&ppg_en,&motor,&led,&bat_en,&uart_en};
    const struct gpio_dt_spec *inputs[]={&usb_pg,&suspend,&imu_int,&ppg_int,&button,
                                        &bc_open,&bc_allow,&bc_high,&chg_int};
    for (size_t i=0;i<ARRAY_SIZE(outputs);i++) {
        if (!gpio_is_ready_dt(outputs[i])) return -ENODEV;
        int err=gpio_pin_configure_dt(outputs[i],GPIO_OUTPUT_INACTIVE);
        if (err) return err;
    }
    for (size_t i=0;i<ARRAY_SIZE(inputs);i++) {
        if (!gpio_is_ready_dt(inputs[i])) return -ENODEV;
        int err=gpio_pin_configure_dt(inputs[i],GPIO_INPUT);
        if (err) return err;
    }
    const struct gpio_dt_spec *irq[]={&usb_pg,&suspend,&imu_int,&ppg_int,&button,&chg_int};
    for (size_t i=0;i<ARRAY_SIZE(irq);i++) {
        /* Leave disabled sensor pins as inputs without wake interrupts. */
        if ((i==2&&!IS_ENABLED(CONFIG_SF_IMU))||(i==3&&!IS_ENABLED(CONFIG_SF_PPG))) {
            int err=gpio_pin_interrupt_configure_dt(irq[i],GPIO_INT_DISABLE);
            if (err) return err;
            continue;
        }
        gpio_init_callback(callbacks+i,input_irq,BIT(irq[i]->pin));
        int err=gpio_add_callback(irq[i]->port,callbacks+i);
        if (!err) err=gpio_pin_interrupt_configure_dt(irq[i],
            (i==0||i==1||i==4)?GPIO_INT_EDGE_BOTH:GPIO_INT_EDGE_TO_ACTIVE);
        if (err) return err;
    }
    if (!device_is_ready(i2c)||!adc_is_ready_dt(&battery)||!device_is_ready(uart)) return -ENODEV;
    int err=adc_channel_setup_dt(&battery);
    if (err) return err;
    uart_irq_rx_disable(uart);
    uart_irq_err_disable(uart);
    err=pm_device_action_run(uart,PM_DEVICE_ACTION_SUSPEND);
    if (err&&err!=-EALREADY) return err;
    *bus=(struct sf_bus){.read=bus_read,.write=bus_write,.delay_ms=bus_delay};
    return 0;
}
void sf_charge_gate(void *ctx, bool enabled) {
    ARG_UNUSED(ctx);
    enabled=enabled&&IS_ENABLED(CONFIG_SF_CHARGING)&&IS_ENABLED(CONFIG_SF_PACK_APPROVED);
    /* A USB/suspend interrupt may have revoked permission during I2C readback.
     * Serialize the pending check and GPIO write against that local ISR. */
    unsigned int key=irq_lock();
    if (atomic_get(&power_changed)) enabled=false;
    (void)gpio_pin_set_dt(&charge_arm,enabled);
    irq_unlock(key);
}
int sf_ppg_power(bool enabled) {
    return gpio_pin_set_dt(&ppg_en,enabled&&IS_ENABLED(CONFIG_SF_PPG));
}
void sf_led(bool enabled) { (void)gpio_pin_set_dt(&led,enabled); }
int sf_button_pressed(void) { return gpio_pin_get_dt(&button); }
bool sf_take_power_change(void) { return atomic_set(&power_changed,0)!=0; }
int sf_battery_mv(void) {
    int err=gpio_pin_set_dt(&bat_en,1);
    int32_t mv=-1;
    int16_t raw=0;
    static bool calibrated;
    if (!err) {
        k_msleep(5);
        struct adc_sequence seq={.buffer=&raw,.buffer_size=sizeof(raw),.calibrate=!calibrated};
        err=adc_sequence_init_dt(&battery,&seq);
        if (!err) err=adc_read(battery.dev,&seq);
        if (!err) {
            calibrated=true; mv=raw;
            err=adc_raw_to_millivolts_dt(&battery,&mv);
            if (!err&&(raw<0||mv<0)) err=-ERANGE;
        }
    }
    int off=gpio_pin_set_dt(&bat_en,0); /* unconditional, including conversion errors */
    if (err||off) return -1;
    return (int)((mv*147+23)/47);
}
int sf_read_inputs(struct sf_power_input *p) {
    int pg=gpio_pin_get_dt(&usb_pg), s=gpio_pin_get_dt(&suspend);
    int a=gpio_pin_get_dt(&bc_allow), h=gpio_pin_get_dt(&bc_high), o=gpio_pin_get_dt(&bc_open);
    if (pg<0||s<0||a<0||h<0||o<0) {
        p->present=false; p->cc_valid=false; sf_charge_gate(NULL,false);
        atomic_set(&power_changed,1); k_sem_give(&sf_wake); return -EIO;
    }
    p->present=pg!=0; p->awake=s==0; p->allow=a!=0; p->high=h!=0; p->switch_open=o!=0;
    return 0;
}
int sf_watchdog_start(void) {
    if (!device_is_ready(wdt)) return -ENODEV;
    struct wdt_timeout_cfg cfg={.window={.min=0,.max=6000},.flags=WDT_FLAG_RESET_SOC};
    watchdog_channel=wdt_install_timeout(wdt,&cfg);
    if (watchdog_channel<0) return watchdog_channel;
    return wdt_setup(wdt,WDT_OPT_PAUSE_HALTED_BY_DBG);
}
int sf_watchdog_feed(void) {
    return watchdog_channel<0?-ENODEV:wdt_feed(wdt,watchdog_channel);
}
const struct device *sf_uart_device(void) { return uart; }
static int uart_enable_locked(bool enabled) {
    if (enabled==(atomic_get(&uart_on)!=0)) return 0;
    if (!enabled) {
        (void)gpio_pin_set_dt(&uart_en,0);
        uart_irq_rx_disable(uart); uart_irq_err_disable(uart); atomic_set(&uart_on,0);
        int err=pm_device_action_run(uart,PM_DEVICE_ACTION_SUSPEND);
        return err==-EALREADY?0:err;
    }
    int err=pm_device_action_run(uart,PM_DEVICE_ACTION_RESUME);
    if (err&&err!=-EALREADY) return err;
    err=gpio_pin_set_dt(&uart_en,1);
    if (err) return err;
    atomic_set(&uart_on,1); uart_irq_err_enable(uart); uart_irq_rx_enable(uart); return 0;
}
int sf_uart_enable(bool enabled) {
    k_mutex_lock(&uart_lock,K_FOREVER);
    int err=uart_enable_locked(enabled);
    k_mutex_unlock(&uart_lock); return err;
}
int sf_uart_write(const uint8_t *bytes, size_t length) {
    k_mutex_lock(&uart_lock,K_FOREVER);
    int err=atomic_get(&uart_on)?0:-ENOTCONN;
    if (!err) for (size_t i=0;i<length;i++) uart_poll_out(uart,bytes[i]);
    k_mutex_unlock(&uart_lock); return err;
}
