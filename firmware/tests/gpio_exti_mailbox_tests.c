#include <assert.h>
#include <stdint.h>

#include "remotebsp_embedded/gpio_exti_mailbox.h"

int main(void) {
    rbsp_gpio_exti_mailbox_t mailbox;
    rbsp_gpio_exti_mailbox_init(&mailbox);
    uint16_t pin = UINT16_MAX;
    assert(!rbsp_gpio_exti_mailbox_pop(&mailbox, &pin));
    rbsp_gpio_exti_mailbox_push_isr(&mailbox, 0U);
    rbsp_gpio_exti_mailbox_push_isr(&mailbox, 45U);
    rbsp_gpio_exti_mailbox_push_isr(&mailbox, 7U);
    assert(mailbox.count == 2U);
    assert(mailbox.dropped == 1U);
    assert(rbsp_gpio_exti_mailbox_pop(&mailbox, &pin) && pin == 0U);
    assert(rbsp_gpio_exti_mailbox_pop(&mailbox, &pin) && pin == 45U);
    assert(!rbsp_gpio_exti_mailbox_pop(&mailbox, &pin));
    return 0;
}
