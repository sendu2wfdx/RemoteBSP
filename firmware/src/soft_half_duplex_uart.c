#include "remotebsp_embedded/soft_half_duplex_uart.h"

#if defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)

#include <stddef.h>

#if CONFIG_SOFT_HALF_DUPLEX_UART_BAUD != 40000
#error "TMC2209 单线 UART 后端固定使用 40000 波特率"
#endif

enum {
    RBSP_SOFT_UART_MAX_WRITE_BYTES = 32U,
    RBSP_SOFT_UART_FIRST_RESPONSE_TIMEOUT_US = 3000U,
    RBSP_SOFT_UART_NEXT_RESPONSE_TIMEOUT_US = 250U,
    RBSP_SOFT_UART_START_POLL_HZ = 2000000U,
};

typedef struct {
    uint32_t whole_ticks;
    uint32_t remainder;
    uint32_t divisor;
    uint32_t accumulator;
} rbsp_soft_uart_timing_t;

static bool port_valid(const rbsp_soft_half_duplex_uart_t* uart,
                       uint8_t port) {
    return uart != NULL && port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT;
}

static bool timing_init(const rbsp_soft_half_duplex_uart_t* uart,
                        rbsp_soft_uart_timing_t* timing) {
    const uint32_t divisor = CONFIG_SOFT_HALF_DUPLEX_UART_BAUD * 2U;
    if (divisor == 0U || uart->hal.timing_hz < divisor) {
        return false;
    }
    timing->whole_ticks = uart->hal.timing_hz / divisor;
    timing->remainder = uart->hal.timing_hz % divisor;
    timing->divisor = divisor;
    timing->accumulator = 0U;
    return timing->whole_ticks != 0U;
}

static void delay_half_bit(rbsp_soft_half_duplex_uart_t* uart,
                           rbsp_soft_uart_timing_t* timing) {
    uint32_t ticks = timing->whole_ticks;
    timing->accumulator += timing->remainder;
    if (timing->accumulator >= timing->divisor) {
        ++ticks;
        timing->accumulator -= timing->divisor;
    }
    uart->hal.delay_ticks(uart->hal.context, ticks);
}

static void delay_bit(rbsp_soft_half_duplex_uart_t* uart,
                      rbsp_soft_uart_timing_t* timing) {
    delay_half_bit(uart, timing);
    delay_half_bit(uart, timing);
}

static void transmit_byte(rbsp_soft_half_duplex_uart_t* uart,
                          uint8_t port, uint8_t value,
                          rbsp_soft_uart_timing_t* timing) {
    (void)uart->hal.set_output(uart->hal.context, port, false);
    delay_bit(uart, timing);
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        const bool high = (value & (uint8_t)(1U << bit)) != 0U;
        (void)uart->hal.set_output(uart->hal.context, port, high);
        delay_bit(uart, timing);
    }
    (void)uart->hal.set_output(uart->hal.context, port, true);
    delay_bit(uart, timing);
}

static bool wait_for_start(rbsp_soft_half_duplex_uart_t* uart,
                           uint8_t port, uint16_t timeout_us) {
    const uint32_t poll_ticks =
        (uart->hal.timing_hz + RBSP_SOFT_UART_START_POLL_HZ - 1U) /
        RBSP_SOFT_UART_START_POLL_HZ;
    const uint32_t timeout_ticks = (uint32_t)(
        ((uint64_t)uart->hal.timing_hz * timeout_us) / 1000000ULL);
    uint32_t elapsed = 0U;
    while (elapsed < timeout_ticks) {
        bool high = true;
        if (!uart->hal.read_input(uart->hal.context, port, &high)) {
            return false;
        }
        if (!high) {
            return true;
        }
        uart->hal.delay_ticks(uart->hal.context, poll_ticks);
        elapsed += poll_ticks;
    }
    return false;
}

static bool receive_byte(rbsp_soft_half_duplex_uart_t* uart,
                         uint8_t port, uint8_t* value,
                         rbsp_soft_uart_timing_t* timing) {
    uint8_t received = 0U;
    delay_half_bit(uart, timing);
    delay_half_bit(uart, timing);
    delay_half_bit(uart, timing);
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        bool high = false;
        if (!uart->hal.read_input(uart->hal.context, port, &high)) {
            return false;
        }
        if (high) {
            received |= (uint8_t)(1U << bit);
        }
        delay_bit(uart, timing);
    }
    bool stop_high = false;
    if (!uart->hal.read_input(uart->hal.context, port, &stop_high) ||
        !stop_high) {
        return false;
    }
    *value = received;
    return true;
}

bool rbsp_soft_half_duplex_uart_init(
    rbsp_soft_half_duplex_uart_t* uart,
    const rbsp_soft_half_duplex_uart_hal_t* hal) {
    if (uart == NULL || hal == NULL || hal->port_prepare == NULL ||
        hal->set_output == NULL || hal->set_input == NULL ||
        hal->read_input == NULL || hal->timing_hz == 0U ||
        hal->delay_ticks == NULL || hal->irq_save_disable == NULL ||
        hal->irq_restore == NULL) {
        return false;
    }
    uart->hal = *hal;
    for (uint8_t port = 0U;
         port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT; ++port) {
        if (!rbsp_byte_ring_init(&uart->ports[port].receive_ring,
                                 uart->ports[port].receive_storage,
                                 sizeof(uart->ports[port].receive_storage))) {
            return false;
        }
        uart->ports[port].configured = false;
    }
    return true;
}

bool rbsp_soft_half_duplex_uart_configure(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    uint32_t baud_rate, uint8_t data_bits, uint8_t stop_bits,
    uint8_t parity) {
    rbsp_soft_uart_timing_t timing;
    if (!port_valid(uart, port) ||
        baud_rate != CONFIG_SOFT_HALF_DUPLEX_UART_BAUD ||
        data_bits != 8U || stop_bits != 1U || parity != 0U ||
        !timing_init(uart, &timing) ||
        !uart->hal.port_prepare(uart->hal.context, port) ||
        !uart->hal.set_input(uart->hal.context, port)) {
        return false;
    }
    rbsp_byte_ring_clear(&uart->ports[port].receive_ring);
    uart->ports[port].configured = true;
    return true;
}

size_t rbsp_soft_half_duplex_uart_read(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    uint8_t* data, size_t capacity) {
    if (!port_valid(uart, port) || data == NULL ||
        !uart->ports[port].configured) {
        return 0U;
    }
    return rbsp_byte_ring_read(&uart->ports[port].receive_ring,
                               data, capacity);
}

bool rbsp_soft_half_duplex_uart_write(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port,
    const uint8_t* data, size_t length) {
    if (!port_valid(uart, port) || data == NULL || length == 0U ||
        length > RBSP_SOFT_UART_MAX_WRITE_BYTES ||
        !uart->ports[port].configured) {
        return false;
    }
    rbsp_soft_uart_timing_t timing;
    if (!timing_init(uart, &timing)) {
        return false;
    }

    const uint32_t interrupt_state =
        uart->hal.irq_save_disable(uart->hal.context);
    bool success = uart->hal.set_output(uart->hal.context, port, true);
    if (success) {
        delay_bit(uart, &timing);
        delay_bit(uart, &timing);
        for (size_t index = 0U; index < length; ++index) {
            transmit_byte(uart, port, data[index], &timing);
        }
        success = uart->hal.set_input(uart->hal.context, port);
    }
    if (success) {
        uint16_t timeout_us = RBSP_SOFT_UART_FIRST_RESPONSE_TIMEOUT_US;
        while (wait_for_start(uart, port, timeout_us)) {
            uint8_t received = 0U;
            if (!timing_init(uart, &timing) ||
                !receive_byte(uart, port, &received, &timing)) {
                break;
            }
            if (!rbsp_byte_ring_push(&uart->ports[port].receive_ring,
                                     received)) {
                success = false;
                break;
            }
            timeout_us = RBSP_SOFT_UART_NEXT_RESPONSE_TIMEOUT_US;
        }
    }
    uart->hal.irq_restore(uart->hal.context, interrupt_state);
    return success;
}

bool rbsp_soft_half_duplex_uart_reset(
    rbsp_soft_half_duplex_uart_t* uart, uint8_t port) {
    if (!port_valid(uart, port)) {
        return false;
    }
    rbsp_byte_ring_clear(&uart->ports[port].receive_ring);
    uart->ports[port].configured = false;
    return uart->hal.set_input(uart->hal.context, port);
}

#endif
