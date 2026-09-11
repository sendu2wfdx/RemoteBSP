#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
#ifndef CONFIG_GPIO_EXTI_MAILBOX_CAPACITY
#define CONFIG_GPIO_EXTI_MAILBOX_CAPACITY 2
#endif

/* ISR 只投递静态编码引脚；去抖、采样与协议发送仍由主循环完成。 */
typedef struct {
    volatile uint8_t begin;
    volatile uint8_t count;
    volatile uint32_t dropped;
    uint16_t pins[CONFIG_GPIO_EXTI_MAILBOX_CAPACITY];
} rbsp_gpio_exti_mailbox_t;

static inline void rbsp_gpio_exti_mailbox_init(
    rbsp_gpio_exti_mailbox_t* mailbox) {
    mailbox->begin = 0U;
    mailbox->count = 0U;
    mailbox->dropped = 0U;
}

static inline void rbsp_gpio_exti_mailbox_push_isr(
    rbsp_gpio_exti_mailbox_t* mailbox, uint16_t pin) {
    if (mailbox->count >= CONFIG_GPIO_EXTI_MAILBOX_CAPACITY) {
        if (mailbox->dropped != UINT32_MAX) ++mailbox->dropped;
        return;
    }
    const uint8_t position = (uint8_t)(
        (mailbox->begin + mailbox->count) % CONFIG_GPIO_EXTI_MAILBOX_CAPACITY);
    mailbox->pins[position] = pin;
    ++mailbox->count;
}

/* 调用方须在极短临界区内消费，避免与对应 EXTI ISR 并发。 */
static inline bool rbsp_gpio_exti_mailbox_pop(
    rbsp_gpio_exti_mailbox_t* mailbox, uint16_t* pin) {
    if (mailbox->count == 0U || pin == 0) return false;
    *pin = mailbox->pins[mailbox->begin];
    mailbox->begin = (uint8_t)(
        (mailbox->begin + 1U) % CONFIG_GPIO_EXTI_MAILBOX_CAPACITY);
    --mailbox->count;
    return true;
}
#endif
