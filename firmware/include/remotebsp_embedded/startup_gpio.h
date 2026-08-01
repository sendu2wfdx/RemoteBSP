#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RBSP_STARTUP_GPIO_OUTPUT_LOW = 0,
    RBSP_STARTUP_GPIO_OUTPUT_HIGH = 1,
    RBSP_STARTUP_GPIO_INPUT_FLOATING = 2,
    RBSP_STARTUP_GPIO_INPUT_PULLUP = 3,
    RBSP_STARTUP_GPIO_INPUT_PULLDOWN = 4,
} rbsp_startup_gpio_mode_t;

typedef bool (*rbsp_startup_gpio_apply_fn)(
    uint16_t encoded_pin, rbsp_startup_gpio_mode_t mode);

/*
 * 解析menuconfig中的PA0,PB2形式，并保证五个列表之间不存在重复引脚。
 * 空列表合法；任何格式错误、重复或板级回调拒绝都会返回false。
 */
bool rbsp_startup_gpio_apply(
    const char* output_low, const char* output_high,
    const char* input_floating, const char* input_pullup,
    const char* input_pulldown, rbsp_startup_gpio_apply_fn apply);
