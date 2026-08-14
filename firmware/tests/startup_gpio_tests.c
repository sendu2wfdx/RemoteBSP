#include "remotebsp_embedded/startup_gpio.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

static rbsp_startup_gpio_mode_t modes[128];
static bool applied[128];

static bool apply_pin(uint16_t pin, rbsp_startup_gpio_mode_t mode) {
    if (pin >= 64U || pin == 13U) {
        return false;
    }
    applied[pin] = true;
    modes[pin] = mode;
    return true;
}

static void reset_state(void) {
    memset(applied, 0, sizeof(applied));
    memset(modes, 0, sizeof(modes));
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
    return 0;
}
