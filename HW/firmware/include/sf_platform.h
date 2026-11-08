/* SPDX-License-Identifier: MIT */
#ifndef SF_PLATFORM_H
#define SF_PLATFORM_H
#include "sf_core.h"
#include "sf_rx.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
extern struct k_sem sf_wake;
int sf_board_init(struct sf_bus *bus);
void sf_charge_gate(void *ctx, bool enabled);
int sf_ppg_power(bool enabled);
void sf_led(bool enabled);
int sf_battery_mv(void);
int sf_read_inputs(struct sf_power_input *input);
bool sf_take_power_change(void);
int sf_button_pressed(void);
int sf_watchdog_start(void);
int sf_watchdog_feed(void);
const struct device *sf_uart_device(void);
int sf_uart_enable(bool enabled);
int sf_uart_write(const uint8_t *bytes, size_t length);

void sf_rx_enqueue(enum sf_transport transport, const uint8_t *bytes, size_t len);
bool sf_rx_pop(struct sf_rx_chunk *chunk);
int sf_transport_init(void);
int sf_transport_send(enum sf_transport transport, const struct sf_packet *packet);
bool sf_ble_ready(void);
uint32_t sf_rx_drops(void);
uint32_t sf_tx_drops(void);
#endif
