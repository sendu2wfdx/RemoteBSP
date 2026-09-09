#include "remotebsp_embedded/static_resources.h"

#include <stddef.h>

static bool claim_pin(bool used[128], uint16_t pin) {
    if (pin >= 128U || used[pin]) {
        return false;
    }
    used[pin] = true;
    return true;
}

static bool waveform_table_valid(
    const rbsp_static_waveform_entry_t* entries, uint8_t count,
    uint8_t maximum_count, bool used_pins[128]) {
    if (count > maximum_count || (count != 0U && entries == NULL)) {
        return false;
    }
    for (uint8_t index = 0U; index < count; ++index) {
        if (entries[index].maximum_value == 0U ||
            !claim_pin(used_pins, entries[index].pin)) {
            return false;
        }
        for (uint8_t previous = 0U; previous < index; ++previous) {
            if (entries[previous].channel == entries[index].channel) {
                return false;
            }
        }
    }
    return true;
}

bool rbsp_static_resources_validate(
    const rbsp_startup_gpio_entry_t* gpio_entries, uint16_t gpio_count,
    const rbsp_static_uart_entry_t* uart_entries, uint8_t uart_count,
    const rbsp_static_waveform_entry_t* pwm_entries, uint8_t pwm_count,
    const rbsp_static_waveform_entry_t* timed_entries, uint8_t timed_count) {
    if (gpio_count > 128U || uart_count > 3U ||
        (gpio_count != 0U && gpio_entries == NULL) ||
        (uart_count != 0U && uart_entries == NULL)) {
        return false;
    }
    bool used_pins[128] = {false};
    for (uint16_t index = 0U; index < gpio_count; ++index) {
        if ((unsigned int)gpio_entries[index].mode >
                (unsigned int)RBSP_STARTUP_GPIO_INPUT_PULLDOWN ||
            !claim_pin(used_pins, gpio_entries[index].encoded_pin)) {
            return false;
        }
    }
    for (uint8_t index = 0U; index < uart_count; ++index) {
        const rbsp_static_uart_entry_t* entry = &uart_entries[index];
        if (entry->minimum_baud_rate == 0U ||
            entry->minimum_baud_rate > entry->maximum_baud_rate ||
            !claim_pin(used_pins, entry->rx_pin) ||
            !claim_pin(used_pins, entry->tx_pin)) {
            return false;
        }
        for (uint8_t previous = 0U; previous < index; ++previous) {
            if (uart_entries[previous].port == entry->port) {
                return false;
            }
        }
    }
    return waveform_table_valid(
               pwm_entries, pwm_count, 4U, used_pins) &&
           waveform_table_valid(
               timed_entries, timed_count, 2U, used_pins);
}

bool rbsp_static_uart_allows(
    const rbsp_static_uart_entry_t* entries, uint8_t count,
    uint8_t port, uint32_t baud_rate) {
    if (count > 3U || (count != 0U && entries == NULL)) {
        return false;
    }
    for (uint8_t index = 0U; index < count; ++index) {
        if (entries[index].port == port) {
            return baud_rate >= entries[index].minimum_baud_rate &&
                   baud_rate <= entries[index].maximum_baud_rate;
        }
    }
    return false;
}

bool rbsp_static_waveform_allows(
    const rbsp_static_waveform_entry_t* entries, uint8_t count,
    uint8_t channel, uint32_t value) {
    if (count > 4U || value == 0U ||
        (count != 0U && entries == NULL)) {
        return false;
    }
    for (uint8_t index = 0U; index < count; ++index) {
        if (entries[index].channel == channel) {
            return value <= entries[index].maximum_value;
        }
    }
    return false;
}
