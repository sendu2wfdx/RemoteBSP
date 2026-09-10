#include "remotebsp_embedded/soft_half_duplex_uart.h"

#include <assert.h>
#include <string.h>

typedef struct {
    bool output_high;
    bool output_mode;
    uint32_t output_changes;
    uint32_t elapsed_ticks;
    uint32_t saved_irq_state;
    bool response_bits[10];
    size_t response_index;
} mock_soft_uart_t;

static bool port_prepare(void* context, uint8_t port) {
    (void)context;
    return port == 0U;
}

static bool set_output(void* context, uint8_t port, bool high) {
    mock_soft_uart_t* mock = context;
    if (port != 0U) {
        return false;
    }
    mock->output_mode = true;
    mock->output_high = high;
    ++mock->output_changes;
    return true;
}

static bool set_input(void* context, uint8_t port) {
    mock_soft_uart_t* mock = context;
    if (port != 0U) {
        return false;
    }
    mock->output_mode = false;
    return true;
}

static bool read_input(void* context, uint8_t port, bool* high) {
    mock_soft_uart_t* mock = context;
    if (port != 0U || high == NULL) {
        return false;
    }
    if (mock->response_index < sizeof(mock->response_bits) /
                                   sizeof(mock->response_bits[0])) {
        *high = mock->response_bits[mock->response_index++];
    } else {
        *high = true;
    }
    return true;
}

static void delay_ticks(void* context, uint32_t ticks) {
    mock_soft_uart_t* mock = context;
    mock->elapsed_ticks += ticks;
}

static uint32_t irq_save_disable(void* context) {
    mock_soft_uart_t* mock = context;
    const uint32_t previous = mock->saved_irq_state;
    mock->saved_irq_state = 1U;
    return previous;
}

static void irq_restore(void* context, uint32_t state) {
    mock_soft_uart_t* mock = context;
    mock->saved_irq_state = state;
}

int main(void) {
    mock_soft_uart_t mock;
    memset(&mock, 0, sizeof(mock));
    /* 起始位低，随后返回 0xA5 的 LSB-first 数据位和停止位高。 */
    const bool response[] = {
        false, true, false, true, false,
        false, true, false, true, true,
    };
    memcpy(mock.response_bits, response, sizeof(response));

    const rbsp_soft_half_duplex_uart_hal_t hal = {
        .context = &mock,
        .port_prepare = port_prepare,
        .set_output = set_output,
        .set_input = set_input,
        .read_input = read_input,
        .timing_hz = 72000000U,
        .delay_ticks = delay_ticks,
        .irq_save_disable = irq_save_disable,
        .irq_restore = irq_restore,
    };
    rbsp_soft_half_duplex_uart_t uart;
    assert(rbsp_soft_half_duplex_uart_init(&uart, &hal));
    assert(!rbsp_soft_half_duplex_uart_configure(
        &uart, 1U, CONFIG_SOFT_HALF_DUPLEX_UART_BAUD, 8U, 1U, 0U));
    assert(rbsp_soft_half_duplex_uart_configure(
        &uart, 0U, CONFIG_SOFT_HALF_DUPLEX_UART_BAUD, 8U, 1U, 0U));

    const uint8_t request[] = {0x05U};
    assert(rbsp_soft_half_duplex_uart_write(
        &uart, 0U, request, sizeof(request)));
    assert(mock.output_changes >= 11U);
    assert(mock.elapsed_ticks >=
           (72000000U / CONFIG_SOFT_HALF_DUPLEX_UART_BAUD) * 12U);
    assert(mock.saved_irq_state == 0U);
    assert(!mock.output_mode);

    uint8_t response_byte = 0U;
    assert(rbsp_soft_half_duplex_uart_read(
        &uart, 0U, &response_byte, 1U) == 1U);
    assert(response_byte == 0xA5U);
    assert(rbsp_soft_half_duplex_uart_configure(
        &uart, 0U, CONFIG_SOFT_HALF_DUPLEX_UART_BAUD, 8U, 1U, 0U));
    assert(rbsp_soft_half_duplex_uart_write(
        &uart, 0U, request, sizeof(request)));
    assert(rbsp_soft_half_duplex_uart_reset(&uart, 0U));
    assert(!mock.output_mode);
    assert(rbsp_soft_half_duplex_uart_read(
               &uart, 0U, &response_byte, 1U) == 0U);
    assert(!rbsp_soft_half_duplex_uart_write(
        &uart, 0U, request, sizeof(request)));
    assert(!rbsp_soft_half_duplex_uart_reset(&uart, 1U));
    return 0;
}
