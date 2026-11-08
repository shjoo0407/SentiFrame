/* SPDX-License-Identifier: MIT */
#ifndef SF_TEST_ZEPHYR_KERNEL_H
#define SF_TEST_ZEPHYR_KERNEL_H
#include <stddef.h>
#include <stdint.h>
#define IS_ENABLED(config) ((config) == 1)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARG_UNUSED(value) ((void)(value))
#define K_MSEC(value) (value)
struct k_sem { int unused; };
uint32_t k_uptime_get_32(void);
void k_msleep(int32_t ms);
int k_sem_take(struct k_sem *sem, int32_t timeout);
#endif
