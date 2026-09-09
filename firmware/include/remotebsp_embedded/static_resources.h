#pragma once

#include "remotebsp_embedded/startup_gpio.h"

#include <stdbool.h>
#include <stdint.h>

#define RBSP_STATIC_RESOURCE_TABLE_SCHEMA_VERSION 2U

typedef struct {
    uint8_t port;
    uint16_t rx_pin;
    uint16_t tx_pin;
    uint32_t minimum_baud_rate;
    uint32_t maximum_baud_rate;
} rbsp_static_uart_entry_t;

typedef struct {
    uint8_t channel;
    uint16_t pin;
    uint32_t maximum_value;
} rbsp_static_waveform_entry_t;

/*
 * 固件启动时完整校验 Studio 表。除各类字段边界外，还拒绝跨 GPIO、UART、
 * PWM 和定时位流的重复引脚，防止损坏或错配的生成表被部分激活。
 */
bool rbsp_static_resources_validate(
    const rbsp_startup_gpio_entry_t* gpio_entries, uint16_t gpio_count,
    const rbsp_static_uart_entry_t* uart_entries, uint8_t uart_count,
    const rbsp_static_waveform_entry_t* pwm_entries, uint8_t pwm_count,
    const rbsp_static_waveform_entry_t* timed_entries, uint8_t timed_count);

/* UART_CREATE 与波形命令的运行时白名单查询。 */
bool rbsp_static_uart_allows(
    const rbsp_static_uart_entry_t* entries, uint8_t count,
    uint8_t port, uint32_t baud_rate);
bool rbsp_static_waveform_allows(
    const rbsp_static_waveform_entry_t* entries, uint8_t count,
    uint8_t channel, uint32_t value);
