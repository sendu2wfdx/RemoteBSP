#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RBSP_STARTUP_GPIO_TABLE_SCHEMA_VERSION 1U

typedef enum {
    RBSP_STARTUP_GPIO_OUTPUT_LOW = 0,
    RBSP_STARTUP_GPIO_OUTPUT_HIGH = 1,
    RBSP_STARTUP_GPIO_INPUT_FLOATING = 2,
    RBSP_STARTUP_GPIO_INPUT_PULLUP = 3,
    RBSP_STARTUP_GPIO_INPUT_PULLDOWN = 4,
} rbsp_startup_gpio_mode_t;

typedef bool (*rbsp_startup_gpio_apply_fn)(
    uint16_t encoded_pin, rbsp_startup_gpio_mode_t mode);

typedef struct {
    uint16_t encoded_pin;
    rbsp_startup_gpio_mode_t mode;
} rbsp_startup_gpio_entry_t;

/*
 * 解析menuconfig中的PA0,PB2形式，并保证五个列表之间不存在重复引脚。
 * 空列表合法；任何格式错误、重复或板级回调拒绝都会返回false。
 */
bool rbsp_startup_gpio_apply(
    const char* output_low, const char* output_high,
    const char* input_floating, const char* input_pullup,
    const char* input_pulldown, rbsp_startup_gpio_apply_fn apply);

/* 查询一个引脚是否存在于五个静态列表中，并返回其声明模式。 */
bool rbsp_startup_gpio_find(
    const char* output_low, const char* output_high,
    const char* input_floating, const char* input_pullup,
    const char* input_pulldown, uint16_t encoded_pin,
    rbsp_startup_gpio_mode_t* mode);

/* 消费 Studio 生成的只读静态表；重复引脚、越界模式和空指针均拒绝。 */
bool rbsp_startup_gpio_apply_table(
    const rbsp_startup_gpio_entry_t* entries, uint16_t count,
    rbsp_startup_gpio_apply_fn apply);

bool rbsp_startup_gpio_find_table(
    const rbsp_startup_gpio_entry_t* entries, uint16_t count,
    uint16_t encoded_pin, rbsp_startup_gpio_mode_t* mode);
