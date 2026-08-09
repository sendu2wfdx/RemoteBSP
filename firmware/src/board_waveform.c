#include "remotebsp_embedded/board_waveform.h"

#include "remotebsp_config.h"

#if defined(STM32F072xB)
#include "stm32f0xx_hal.h"
#elif defined(STM32F103xB)
#include "stm32f1xx_hal.h"
#elif defined(STM32G431xx)
#include "stm32g4xx_hal.h"
#else
#error "波形后端只支持当前三款 STM32 目标"
#endif

#if defined(CONFIG_REMOTEBSP_PWM)
static TIM_HandleTypeDef pwm_timer;
static uint32_t pwm_period_ticks;

static void pwm_gpio_init(void) {
#if defined(CONFIG_PWM0_PIN_PC6)
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_6;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    pin.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOC, &pin);
#else
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_6;
    pin.Mode = GPIO_MODE_AF_PP;
#if defined(STM32F103xB)
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
#else
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    pin.Alternate = GPIO_AF1_TIM3;
#endif
    HAL_GPIO_Init(GPIOA, &pin);
#endif
}

bool rbsp_board_pwm_configure(uint8_t channel, uint32_t frequency_hz,
                              uint16_t duty, bool active_low) {
    if (channel != 0U || frequency_hz == 0U || duty > 10000U) {
        return false;
    }
    const uint32_t timer_hz = CONFIG_SYSTEM_CLOCK_HZ;
    uint32_t divider = timer_hz / frequency_hz;
    if (divider < 2U) {
        return false;
    }
    uint32_t prescaler = (divider - 1U) / 65536U;
    if (prescaler > 65535U) {
        return false;
    }
    divider = timer_hz / ((prescaler + 1U) * frequency_hz);
    if (divider < 2U || divider > 65536U) {
        return false;
    }

    (void)HAL_TIM_PWM_Stop(&pwm_timer, TIM_CHANNEL_1);
    __HAL_RCC_TIM3_CLK_ENABLE();
    pwm_gpio_init();
    pwm_period_ticks = divider;
    pwm_timer.Instance = TIM3;
    pwm_timer.Init.Prescaler = prescaler;
    pwm_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    pwm_timer.Init.Period = divider - 1U;
    pwm_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
#if !defined(STM32F103xB)
    pwm_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
#endif
    if (HAL_TIM_PWM_Init(&pwm_timer) != HAL_OK) {
        return false;
    }
    TIM_OC_InitTypeDef output = {0};
    output.OCMode = TIM_OCMODE_PWM1;
    output.Pulse = (divider * duty) / 10000U;
    output.OCPolarity = active_low ? TIM_OCPOLARITY_LOW
                                   : TIM_OCPOLARITY_HIGH;
    output.OCFastMode = TIM_OCFAST_DISABLE;
    return HAL_TIM_PWM_ConfigChannel(
               &pwm_timer, &output, TIM_CHANNEL_1) == HAL_OK &&
           HAL_TIM_PWM_Start(&pwm_timer, TIM_CHANNEL_1) == HAL_OK;
}

bool rbsp_board_pwm_write(uint8_t channel, uint16_t duty) {
    if (channel != 0U || duty > 10000U || pwm_period_ticks == 0U) {
        return false;
    }
    __HAL_TIM_SET_COMPARE(&pwm_timer, TIM_CHANNEL_1,
                          (pwm_period_ticks * duty) / 10000U);
    return true;
}

bool rbsp_board_pwm_stop(uint8_t channel) {
    if (channel != 0U) {
        return false;
    }
    __HAL_TIM_SET_COMPARE(&pwm_timer, TIM_CHANNEL_1, 0U);
    return HAL_TIM_PWM_Stop(&pwm_timer, TIM_CHANNEL_1) == HAL_OK;
}
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
enum { RBSP_TIMED_RESET_SLOTS_MAX = 256U };

static TIM_HandleTypeDef timed_timer;
static DMA_HandleTypeDef timed_dma;
static uint16_t timed_dma_buffer[
    CONFIG_TIMED_BITSTREAM_MAX_BITS + RBSP_TIMED_RESET_SLOTS_MAX];
static uint16_t timed_period_ticks;
static uint16_t timed_zero_ticks;
static uint16_t timed_one_ticks;
static uint16_t timed_reset_slots;
static volatile bool timed_busy;

static void timed_gpio_init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_8;
    pin.Mode = GPIO_MODE_AF_PP;
#if defined(STM32F103xB)
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
#elif defined(STM32F072xB)
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    pin.Alternate = GPIO_AF2_TIM1;
#else
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    pin.Alternate = GPIO_AF6_TIM1;
#endif
    HAL_GPIO_Init(GPIOA, &pin);
}

static bool timed_dma_init(void) {
    __HAL_RCC_DMA1_CLK_ENABLE();
#if defined(STM32F072xB)
    timed_dma.Instance = DMA1_Channel2;
#elif defined(STM32F103xB)
    timed_dma.Instance = DMA1_Channel2;
#else
    timed_dma.Instance = DMA1_Channel1;
    timed_dma.Init.Request = DMA_REQUEST_TIM1_CH1;
#endif
    timed_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    timed_dma.Init.PeriphInc = DMA_PINC_DISABLE;
    timed_dma.Init.MemInc = DMA_MINC_ENABLE;
    timed_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    timed_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    timed_dma.Init.Mode = DMA_NORMAL;
    timed_dma.Init.Priority = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&timed_dma) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&timed_timer, hdma[TIM_DMA_ID_CC1], timed_dma);
#if defined(STM32F072xB)
    HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);
#elif defined(STM32F103xB)
    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
#else
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
#endif
    return true;
}

bool rbsp_board_timed_bitstream_configure(
    uint8_t channel, uint32_t bit_period_ns, uint32_t zero_high_ns,
    uint32_t one_high_ns, uint32_t reset_time_us) {
    if (channel != 0U || timed_busy || bit_period_ns == 0U ||
        zero_high_ns == 0U || one_high_ns == 0U ||
        zero_high_ns >= bit_period_ns || one_high_ns >= bit_period_ns ||
        reset_time_us == 0U) {
        return false;
    }
    const uint64_t period_ticks =
        ((uint64_t)CONFIG_SYSTEM_CLOCK_HZ * bit_period_ns +
         UINT64_C(500000000)) /
        UINT64_C(1000000000);
    const uint64_t zero_ticks =
        ((uint64_t)CONFIG_SYSTEM_CLOCK_HZ * zero_high_ns +
         UINT64_C(500000000)) /
        UINT64_C(1000000000);
    const uint64_t one_ticks =
        ((uint64_t)CONFIG_SYSTEM_CLOCK_HZ * one_high_ns +
         UINT64_C(500000000)) /
        UINT64_C(1000000000);
    const uint64_t reset_slots =
        ((uint64_t)reset_time_us * 1000U + bit_period_ns - 1U) /
        bit_period_ns;
    if (period_ticks < 2U || period_ticks > 65536U || zero_ticks == 0U ||
        one_ticks == 0U || zero_ticks >= period_ticks ||
        one_ticks >= period_ticks ||
        reset_slots > RBSP_TIMED_RESET_SLOTS_MAX) {
        return false;
    }

    __HAL_RCC_TIM1_CLK_ENABLE();
    timed_gpio_init();
    timed_timer.Instance = TIM1;
    timed_timer.Init.Prescaler = 0U;
    timed_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    timed_timer.Init.Period = (uint32_t)period_ticks - 1U;
    timed_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
#if !defined(STM32F103xB)
    timed_timer.Init.RepetitionCounter = 0U;
    timed_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
#endif
    if (HAL_TIM_PWM_Init(&timed_timer) != HAL_OK) {
        return false;
    }
    TIM_OC_InitTypeDef output = {0};
    output.OCMode = TIM_OCMODE_PWM1;
    output.Pulse = 0U;
    output.OCPolarity = TIM_OCPOLARITY_HIGH;
    output.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(
            &timed_timer, &output, TIM_CHANNEL_1) != HAL_OK ||
        !timed_dma_init()) {
        return false;
    }
    timed_period_ticks = (uint16_t)period_ticks;
    timed_zero_ticks = (uint16_t)zero_ticks;
    timed_one_ticks = (uint16_t)one_ticks;
    timed_reset_slots = (uint16_t)reset_slots;
    return true;
}

bool rbsp_board_timed_bitstream_write(uint8_t channel,
                                      const uint8_t* data,
                                      uint16_t bit_count) {
    if (channel != 0U || data == NULL || bit_count == 0U || timed_busy ||
        bit_count > CONFIG_TIMED_BITSTREAM_MAX_BITS ||
        timed_period_ticks == 0U) {
        return false;
    }
    for (uint16_t bit = 0U; bit < bit_count; ++bit) {
        const bool one =
            (data[bit / 8U] & (uint8_t)(0x80U >> (bit % 8U))) != 0U;
        timed_dma_buffer[bit] = one ? timed_one_ticks : timed_zero_ticks;
    }
    for (uint16_t slot = 0U; slot < timed_reset_slots; ++slot) {
        timed_dma_buffer[bit_count + slot] = 0U;
    }
    timed_busy = true;
    if (HAL_TIM_PWM_Start_DMA(
            &timed_timer, TIM_CHANNEL_1,
            (const uint32_t*)timed_dma_buffer,
            (uint16_t)(bit_count + timed_reset_slots)) != HAL_OK) {
        timed_busy = false;
        return false;
    }
    return true;
}

bool rbsp_board_timed_bitstream_busy(uint8_t channel) {
    return channel == 0U && timed_busy;
}

bool rbsp_board_timed_bitstream_abort(uint8_t channel) {
    if (channel != 0U) {
        return false;
    }
    (void)HAL_TIM_PWM_Stop_DMA(&timed_timer, TIM_CHANNEL_1);
    __HAL_TIM_SET_COMPARE(&timed_timer, TIM_CHANNEL_1, 0U);
    timed_busy = false;
    return true;
}

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef* timer) {
    if (timer != NULL && timer->Instance == TIM1) {
        (void)rbsp_board_timed_bitstream_abort(0U);
    }
}

#if defined(STM32F072xB)
void DMA1_Channel2_3_IRQHandler(void) {
    HAL_DMA_IRQHandler(&timed_dma);
}
#elif defined(STM32F103xB)
void DMA1_Channel2_IRQHandler(void) {
    HAL_DMA_IRQHandler(&timed_dma);
}
#else
void DMA1_Channel1_IRQHandler(void) {
    HAL_DMA_IRQHandler(&timed_dma);
}
#endif
#endif

bool rbsp_board_waveform_init(void) {
#if defined(CONFIG_REMOTEBSP_PWM)
    pwm_period_ticks = 0U;
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    timed_period_ticks = 0U;
    timed_busy = false;
#endif
    return true;
}
