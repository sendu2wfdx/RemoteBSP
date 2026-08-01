#include "remotebsp_embedded/startup_gpio.h"

#include <stddef.h>

static bool separator(char value) {
    return value == ',' || value == ';' || value == ' ' ||
           value == '\t' || value == '\r' || value == '\n';
}

static bool parse_list(const char* text, rbsp_startup_gpio_mode_t mode,
                       bool used[128], rbsp_startup_gpio_apply_fn apply) {
    if (text == NULL) {
        return false;
    }
    const char* cursor = text;
    while (*cursor != '\0') {
        while (separator(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != 'P' || cursor[1] < 'A' || cursor[1] > 'H') {
            return false;
        }
        const uint8_t port = (uint8_t)(cursor[1] - 'A');
        cursor += 2;
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        uint8_t pin = (uint8_t)(*cursor++ - '0');
        if (*cursor >= '0' && *cursor <= '9') {
            pin = (uint8_t)(pin * 10U + (uint8_t)(*cursor++ - '0'));
        }
        if (pin > 15U || (*cursor != '\0' && !separator(*cursor))) {
            return false;
        }
        const uint16_t encoded_pin = (uint16_t)(port * 16U + pin);
        if (used[encoded_pin] || !apply(encoded_pin, mode)) {
            return false;
        }
        used[encoded_pin] = true;
    }
    return true;
}

bool rbsp_startup_gpio_apply(
    const char* output_low, const char* output_high,
    const char* input_floating, const char* input_pullup,
    const char* input_pulldown, rbsp_startup_gpio_apply_fn apply) {
    if (apply == NULL) {
        return false;
    }
    bool used[128] = {false};
    return parse_list(output_low, RBSP_STARTUP_GPIO_OUTPUT_LOW,
                      used, apply) &&
           parse_list(output_high, RBSP_STARTUP_GPIO_OUTPUT_HIGH,
                      used, apply) &&
           parse_list(input_floating, RBSP_STARTUP_GPIO_INPUT_FLOATING,
                      used, apply) &&
           parse_list(input_pullup, RBSP_STARTUP_GPIO_INPUT_PULLUP,
                      used, apply) &&
           parse_list(input_pulldown, RBSP_STARTUP_GPIO_INPUT_PULLDOWN,
                      used, apply);
}
