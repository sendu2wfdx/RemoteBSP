#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remotebsp_config.h"

#if defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)

#include "remotebsp_embedded/byte_ring.h"

/*
 * TMC2209 专用的内部单线 UART 后端：只负责字节级时序、收发缓存和板级
 * GPIO/定时器回调。TMC 寄存器语义、CRC 和重试策略仍由 Linux 端负责。
 */
typedef struct {
    void* context;
    bool (*port_prepare)(void* context, uint8_t port);
    bool (*set_output)(void* context, uint8_t port, bool high);
    bool (*set_input)(void* context, uint8_t port);
    bool (*read_input)(void* context, uint8_t port, bool* high);
    uint32_t timing_hz;
    void (*delay_ticks)(void* context, uint32_t ticks);
    uint32_t (*irq_save_disable)(void* context);
    void (*irq_restore)(void* context, uint32_t state);
} rbsp_soft_half_duplex_uart_hal_t;

typedef struct {
    rbsp_byte_ring_t receive_ring;
    uint8_t receive_storage[CONFIG_SOFT_HALF_DUPLEX_UART_RX_BUFFER_SIZE];
    bool configured;
} rbsp_soft_half_duplex_uart_port_t;

typedef struct {
    rbsp_soft_half_duplex_uart_hal_t hal;
    rbsp_soft_half_duplex_uart_port_t
        ports[CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT];
} rbsp_soft_half_duplex_uart_t;

bool rbsp_soft_half_duplex_uart_init(
    rbsp_soft_half_duplex_uart_t* uart,
    const rbsp_soft_half_duplex_uart_hal_t* hal);
bool rbsp_soft_half_duplex_uart_configure(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    uint32_t baud_rate, uint8_t data_bits, uint8_t stop_bits,
    uint8_t parity);
size_t rbsp_soft_half_duplex_uart_read(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    uint8_t* data, size_t capacity);
bool rbsp_soft_half_duplex_uart_write(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    const uint8_t* data, size_t length);

#endif
