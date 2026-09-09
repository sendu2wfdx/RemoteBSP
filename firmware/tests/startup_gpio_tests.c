#include "remotebsp_embedded/startup_gpio.h"
#include "remotebsp_embedded/static_resources.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static rbsp_startup_gpio_mode_t modes[128];
static bool applied[128];
static unsigned int apply_count;

static bool apply_pin(uint16_t pin, rbsp_startup_gpio_mode_t mode) {
    if (pin >= 64U || pin == 13U) {
        return false;
    }
    applied[pin] = true;
    modes[pin] = mode;
    ++apply_count;
    return true;
}

static void reset_state(void) {
    memset(applied, 0, sizeof(applied));
    memset(modes, 0, sizeof(modes));
    apply_count = 0U;
}

int main(void) {
    reset_state();
    assert(rbsp_startup_gpio_apply(
        "PA0, PB2", "PC13", "", "PA1", "PB3", apply_pin));
    assert(applied[0] && modes[0] == RBSP_STARTUP_GPIO_OUTPUT_LOW);
    assert(applied[18] && modes[18] == RBSP_STARTUP_GPIO_OUTPUT_LOW);
    assert(applied[45] && modes[45] == RBSP_STARTUP_GPIO_OUTPUT_HIGH);
    assert(applied[1] && modes[1] == RBSP_STARTUP_GPIO_INPUT_PULLUP);
    assert(applied[19] && modes[19] == RBSP_STARTUP_GPIO_INPUT_PULLDOWN);
    rbsp_startup_gpio_mode_t mode = RBSP_STARTUP_GPIO_INPUT_FLOATING;
    assert(rbsp_startup_gpio_find(
        "PA0, PB2", "PC13", "", "PA1", "PB3", 18U, &mode));
    assert(mode == RBSP_STARTUP_GPIO_OUTPUT_LOW);
    assert(rbsp_startup_gpio_find(
        "PA0, PB2", "PC13", "", "PA1", "PB3", 19U, &mode));
    assert(mode == RBSP_STARTUP_GPIO_INPUT_PULLDOWN);
    assert(!rbsp_startup_gpio_find(
        "PA0, PB2", "PC13", "", "PA1", "PB3", 20U, &mode));
    assert(!rbsp_startup_gpio_find(
        "PA0", "", "", "", "", 18U, NULL));

    reset_state();
    assert(!rbsp_startup_gpio_apply(
        "PA0", "PA0", "", "", "", apply_pin));
    assert(!rbsp_startup_gpio_apply(
        "PA16", "", "", "", "", apply_pin));
    assert(!rbsp_startup_gpio_apply(
        "A0", "", "", "", "", apply_pin));
    assert(!rbsp_startup_gpio_apply(
        "PA13", "", "", "", "", apply_pin));
    assert(!rbsp_startup_gpio_apply(
        "PI0", "", "", "", "", apply_pin));
    assert(!rbsp_startup_gpio_apply(
        "", "", "", "", "", NULL));

    const rbsp_startup_gpio_entry_t table[] = {
        {0U, RBSP_STARTUP_GPIO_OUTPUT_HIGH},
        {18U, RBSP_STARTUP_GPIO_INPUT_PULLDOWN},
    };
    reset_state();
    assert(rbsp_startup_gpio_apply_table(table, 2U, apply_pin));
    assert(applied[0] && modes[0] == RBSP_STARTUP_GPIO_OUTPUT_HIGH);
    assert(applied[18] && modes[18] == RBSP_STARTUP_GPIO_INPUT_PULLDOWN);
    assert(rbsp_startup_gpio_find_table(table, 2U, 18U, &mode));
    assert(mode == RBSP_STARTUP_GPIO_INPUT_PULLDOWN);
    assert(!rbsp_startup_gpio_find_table(table, 2U, 19U, &mode));
    assert(rbsp_startup_gpio_apply_table(NULL, 0U, apply_pin));
    assert(!rbsp_startup_gpio_apply_table(NULL, 1U, apply_pin));
    assert(!rbsp_startup_gpio_apply_table(table, 2U, NULL));
    const rbsp_startup_gpio_entry_t duplicate[] = {
        {1U, RBSP_STARTUP_GPIO_OUTPUT_LOW},
        {1U, RBSP_STARTUP_GPIO_OUTPUT_HIGH},
    };
    reset_state();
    assert(!rbsp_startup_gpio_apply_table(duplicate, 2U, apply_pin));
    assert(apply_count == 0U);
    assert(!rbsp_startup_gpio_find_table(duplicate, 2U, 1U, &mode));
    const rbsp_startup_gpio_entry_t invalid[] = {
        {128U, RBSP_STARTUP_GPIO_OUTPUT_LOW},
    };
    reset_state();
    assert(!rbsp_startup_gpio_apply_table(invalid, 1U, apply_pin));
    assert(apply_count == 0U);
    assert(!rbsp_startup_gpio_find_table(invalid, 1U, 1U, &mode));
    const rbsp_startup_gpio_entry_t invalid_mode[] = {
        {1U, (rbsp_startup_gpio_mode_t)99},
    };
    reset_state();
    assert(!rbsp_startup_gpio_apply_table(invalid_mode, 1U, apply_pin));
    assert(apply_count == 0U);

    const rbsp_static_uart_entry_t uart[] = {
        {0U, 10U, 9U, 300U, 4500000U},
        {1U, 20U, 21U, 300U, 2250000U},
    };
    const rbsp_static_waveform_entry_t pwm[] = {
        {0U, 22U, 20000U},
    };
    const rbsp_static_waveform_entry_t timed[] = {
        {0U, 8U, 768U},
    };
    assert(rbsp_static_resources_validate(
        table, 2U, uart, 2U, pwm, 1U, timed, 1U));
    assert(rbsp_static_uart_allows(uart, 2U, 0U, 300U));
    assert(rbsp_static_uart_allows(uart, 2U, 1U, 2250000U));
    assert(!rbsp_static_uart_allows(uart, 2U, 1U, 2250001U));
    assert(!rbsp_static_uart_allows(uart, 2U, 2U, 115200U));
    assert(rbsp_static_waveform_allows(pwm, 1U, 0U, 20000U));
    assert(!rbsp_static_waveform_allows(pwm, 1U, 0U, 20001U));
    assert(rbsp_static_waveform_allows(timed, 1U, 0U, 768U));
    assert(!rbsp_static_waveform_allows(timed, 1U, 1U, 24U));

    const rbsp_static_uart_entry_t duplicate_port[] = {
        {0U, 10U, 9U, 300U, 4500000U},
        {0U, 20U, 21U, 300U, 2250000U},
    };
    assert(!rbsp_static_resources_validate(
        NULL, 0U, duplicate_port, 2U, NULL, 0U, NULL, 0U));
    const rbsp_static_uart_entry_t conflicting_pin[] = {
        {0U, 0U, 9U, 300U, 4500000U},
    };
    assert(!rbsp_static_resources_validate(
        table, 2U, conflicting_pin, 1U, NULL, 0U, NULL, 0U));
    const rbsp_static_waveform_entry_t duplicate_channel[] = {
        {0U, 22U, 20000U}, {0U, 23U, 20000U},
    };
    assert(!rbsp_static_resources_validate(
        NULL, 0U, NULL, 0U, duplicate_channel, 2U, NULL, 0U));
    return 0;
}
