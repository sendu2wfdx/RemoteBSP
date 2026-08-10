#include "stm32g4xx_hal.h"

#include "remotebsp_embedded/board_config.h"
#if defined(CONFIG_REMOTEBSP_PWM) || defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
#include "remotebsp_embedded/board_waveform.h"
#endif
#include "remotebsp_embedded/core.h"
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
#include "remotebsp_embedded/soft_half_duplex_uart.h"
#endif
#include "remotebsp_embedded/startup_gpio.h"
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
#include "remotebsp_embedded/usb_device_link.h"
#endif
#include <string.h>

#if CONFIG_SYSTEM_CLOCK_HZ != 170000000
#error "STM32G431CBU6 当前时钟方案固定为 170MHz"
#endif
#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
#if (170000000U % (CONFIG_CAN_NOMINAL_BITRATE * 34U)) != 0
#error "当前 G431 FDCAN 位时序无法精确生成所选仲裁段波特率"
#endif

#define FDCAN_NOMINAL_PRESCALER \
    (170000000U / (CONFIG_CAN_NOMINAL_BITRATE * 34U))

#ifdef CONFIG_CAN_FD_ENABLE
#if (170000000U % (CONFIG_CAN_FD_DATA_BITRATE * 17U)) != 0
#error "当前 G431 FDCAN 位时序无法精确生成所选数据段波特率"
#endif
#define FDCAN_DATA_PRESCALER \
    (170000000U / (CONFIG_CAN_FD_DATA_BITRATE * 17U))
#else
#define FDCAN_DATA_PRESCALER 10U
#endif
#endif /* CONFIG_REMOTEBSP_TRANSPORT_CAN */

#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
static FDCAN_HandleTypeDef fdcan_handle;
#endif
static rbsp_core_t remote_core;
static void fatal_error(void);

#ifdef CONFIG_REMOTEBSP_MOTION
#ifdef CONFIG_MOTION_SLOT0_ENABLED
#define MOTION_SLOT0_AXIS_COUNT 1U
#else
#define MOTION_SLOT0_AXIS_COUNT 0U
#endif
#ifdef CONFIG_MOTION_SLOT1_ENABLED
#define MOTION_SLOT1_AXIS_COUNT 1U
#else
#define MOTION_SLOT1_AXIS_COUNT 0U
#endif
#ifdef CONFIG_MOTION_SLOT2_ENABLED
#define MOTION_SLOT2_AXIS_COUNT 1U
#else
#define MOTION_SLOT2_AXIS_COUNT 0U
#endif
#ifdef CONFIG_MOTION_SLOT3_ENABLED
#define MOTION_SLOT3_AXIS_COUNT 1U
#else
#define MOTION_SLOT3_AXIS_COUNT 0U
#endif
#ifdef CONFIG_MOTION_SLOT4_ENABLED
#define MOTION_SLOT4_AXIS_COUNT 1U
#else
#define MOTION_SLOT4_AXIS_COUNT 0U
#endif

#define MOTION_SLOT_AXIS_COUNT \
    (MOTION_SLOT0_AXIS_COUNT + MOTION_SLOT1_AXIS_COUNT + \
     MOTION_SLOT2_AXIS_COUNT + MOTION_SLOT3_AXIS_COUNT + \
     MOTION_SLOT4_AXIS_COUNT)

#if MOTION_SLOT_AXIS_COUNT == 0U
#error "启用运动模块时至少要启用一个本板运动槽"
#endif
#if MOTION_SLOT_AXIS_COUNT > CONFIG_MOTION_MAX_AXES
#error "已启用的本板运动槽数量不能超过 MOTION_MAX_AXES"
#endif

typedef struct {
    uint8_t port;
    uint8_t pin;
} motion_pin_t;

typedef struct {
    motion_pin_t step;
    motion_pin_t direction;
    motion_pin_t enable;
    bool direction_inverted;
    bool enable_active_low;
} motion_axis_pins_t;

/* 槽位本身没有板名；所有引脚均由 menuconfig 生成。 */
static const motion_axis_pins_t motion_axis_pins[MOTION_SLOT_AXIS_COUNT] = {
#ifdef CONFIG_MOTION_SLOT0_ENABLED
    {{CONFIG_MOTION_SLOT0_STEP_PIN / 16U, CONFIG_MOTION_SLOT0_STEP_PIN % 16U},
     {CONFIG_MOTION_SLOT0_DIR_PIN / 16U, CONFIG_MOTION_SLOT0_DIR_PIN % 16U},
     {CONFIG_MOTION_SLOT0_ENABLE_PIN / 16U, CONFIG_MOTION_SLOT0_ENABLE_PIN % 16U},
#ifdef CONFIG_MOTION_SLOT0_DIR_INVERTED
     true,
#else
     false,
#endif
#ifdef CONFIG_MOTION_SLOT0_ENABLE_ACTIVE_LOW
     true},
#else
     false},
#endif
#endif
#ifdef CONFIG_MOTION_SLOT1_ENABLED
    {{CONFIG_MOTION_SLOT1_STEP_PIN / 16U, CONFIG_MOTION_SLOT1_STEP_PIN % 16U},
     {CONFIG_MOTION_SLOT1_DIR_PIN / 16U, CONFIG_MOTION_SLOT1_DIR_PIN % 16U},
     {CONFIG_MOTION_SLOT1_ENABLE_PIN / 16U, CONFIG_MOTION_SLOT1_ENABLE_PIN % 16U},
#ifdef CONFIG_MOTION_SLOT1_DIR_INVERTED
     true,
#else
     false,
#endif
#ifdef CONFIG_MOTION_SLOT1_ENABLE_ACTIVE_LOW
     true},
#else
     false},
#endif
#endif
#ifdef CONFIG_MOTION_SLOT2_ENABLED
    {{CONFIG_MOTION_SLOT2_STEP_PIN / 16U, CONFIG_MOTION_SLOT2_STEP_PIN % 16U},
     {CONFIG_MOTION_SLOT2_DIR_PIN / 16U, CONFIG_MOTION_SLOT2_DIR_PIN % 16U},
     {CONFIG_MOTION_SLOT2_ENABLE_PIN / 16U, CONFIG_MOTION_SLOT2_ENABLE_PIN % 16U},
#ifdef CONFIG_MOTION_SLOT2_DIR_INVERTED
     true,
#else
     false,
#endif
#ifdef CONFIG_MOTION_SLOT2_ENABLE_ACTIVE_LOW
     true},
#else
     false},
#endif
#endif
#ifdef CONFIG_MOTION_SLOT3_ENABLED
    {{CONFIG_MOTION_SLOT3_STEP_PIN / 16U, CONFIG_MOTION_SLOT3_STEP_PIN % 16U},
     {CONFIG_MOTION_SLOT3_DIR_PIN / 16U, CONFIG_MOTION_SLOT3_DIR_PIN % 16U},
     {CONFIG_MOTION_SLOT3_ENABLE_PIN / 16U, CONFIG_MOTION_SLOT3_ENABLE_PIN % 16U},
#ifdef CONFIG_MOTION_SLOT3_DIR_INVERTED
     true,
#else
     false,
#endif
#ifdef CONFIG_MOTION_SLOT3_ENABLE_ACTIVE_LOW
     true},
#else
     false},
#endif
#endif
#ifdef CONFIG_MOTION_SLOT4_ENABLED
    {{CONFIG_MOTION_SLOT4_STEP_PIN / 16U, CONFIG_MOTION_SLOT4_STEP_PIN % 16U},
     {CONFIG_MOTION_SLOT4_DIR_PIN / 16U, CONFIG_MOTION_SLOT4_DIR_PIN % 16U},
     {CONFIG_MOTION_SLOT4_ENABLE_PIN / 16U, CONFIG_MOTION_SLOT4_ENABLE_PIN % 16U},
#ifdef CONFIG_MOTION_SLOT4_DIR_INVERTED
     true,
#else
     false,
#endif
#ifdef CONFIG_MOTION_SLOT4_ENABLE_ACTIVE_LOW
     true},
#else
     false},
#endif
#endif
};

static const uint16_t motion_enable_group_ids[MOTION_SLOT_AXIS_COUNT] = {
#ifdef CONFIG_MOTION_SLOT0_ENABLED
    CONFIG_MOTION_SLOT0_ENABLE_PIN,
#endif
#ifdef CONFIG_MOTION_SLOT1_ENABLED
    CONFIG_MOTION_SLOT1_ENABLE_PIN,
#endif
#ifdef CONFIG_MOTION_SLOT2_ENABLED
    CONFIG_MOTION_SLOT2_ENABLE_PIN,
#endif
#ifdef CONFIG_MOTION_SLOT3_ENABLED
    CONFIG_MOTION_SLOT3_ENABLE_PIN,
#endif
#ifdef CONFIG_MOTION_SLOT4_ENABLED
    CONFIG_MOTION_SLOT4_ENABLE_PIN,
#endif
};
static bool motion_enable_requests[MOTION_SLOT_AXIS_COUNT];

static volatile uint32_t motion_timebase_epochs;
#endif

#ifdef CONFIG_WEACT_G431_PC6_PWM_BREATHING_LED
static TIM_HandleTypeDef led_pwm_timer;

enum {
    LED_PWM_PERIOD = 999U,
    LED_BREATH_PERIOD_MS = 4000U,
    LED_BREATH_HALF_PERIOD_MS = LED_BREATH_PERIOD_MS / 2U,
};

/* TIM3_CH1: PC6 (AF2)，定时器时钟 170MHz / 17 / 1000 = 10kHz。 */
static void led_pwm_configure(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();

    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_6;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_LOW;
    pin.Alternate = GPIO_AF2_TIM3;
    HAL_GPIO_Init(GPIOC, &pin);

    led_pwm_timer.Instance = TIM3;
    led_pwm_timer.Init.Prescaler = 16U;
    led_pwm_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
    led_pwm_timer.Init.Period = LED_PWM_PERIOD;
    led_pwm_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    led_pwm_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&led_pwm_timer) != HAL_OK) {
        fatal_error();
    }

    TIM_OC_InitTypeDef channel = {0};
    channel.OCMode = TIM_OCMODE_PWM1;
    channel.Pulse = 0U;
    channel.OCPolarity = TIM_OCPOLARITY_HIGH;
    channel.OCFastMode = TIM_OCFAST_DISABLE;
    if (HAL_TIM_PWM_ConfigChannel(
            &led_pwm_timer, &channel, TIM_CHANNEL_1) != HAL_OK ||
        HAL_TIM_PWM_Start(&led_pwm_timer, TIM_CHANNEL_1) != HAL_OK) {
        fatal_error();
    }
}

static void led_pwm_poll(void) {
    const uint32_t phase = HAL_GetTick() % LED_BREATH_PERIOD_MS;
    uint32_t linear = phase < LED_BREATH_HALF_PERIOD_MS
                          ? phase
                          : LED_BREATH_PERIOD_MS - phase;
    linear = (linear * LED_PWM_PERIOD) / LED_BREATH_HALF_PERIOD_MS;
    /* 二次曲线改善低亮度区域的视觉平滑度，避免浮点运算。 */
    const uint32_t duty = (linear * linear) / LED_PWM_PERIOD;
    __HAL_TIM_SET_COMPARE(&led_pwm_timer, TIM_CHANNEL_1, duty);
}
#endif
static void put_u32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void fatal_error(void) {
    __disable_irq();
    for (;;) {
    }
}

#ifdef CONFIG_APP_LAYOUT_KATAPULT_8K
static void board_enter_bootloader(rbsp_bootloader_mode_t mode) {
    static const uint64_t can_request_signature =
        UINT64_C(0x5984E3FA6CA1589B);
    static const uint64_t usb_request_signature =
        UINT64_C(0x8F3D6A21C457B09E);
    const uint64_t request_signature =
        mode == RBSP_BOOTLOADER_USB
            ? usb_request_signature : can_request_signature;
    const uint32_t signature_address =
        *(const volatile uint32_t*)FLASH_BASE;
    const uint32_t ram_begin = SRAM_BASE;
    const uint32_t ram_end = SRAM_BASE + (32U * 1024U);
    if ((signature_address & 7U) != 0U ||
        signature_address < ram_begin ||
        signature_address > ram_end - sizeof(request_signature)) {
        fatal_error();
    }
    __disable_irq();
    *(volatile uint64_t*)(uintptr_t)signature_address =
        request_signature;
    __DSB();
    __ISB();
    NVIC_SystemReset();
    for (;;) {
    }
}
#endif

static void system_clock_configure(void) {
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};

    if (HAL_PWREx_ControlVoltageScaling(
            PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK) {
        fatal_error();
    }

    oscillator.OscillatorType =
#ifdef CONFIG_G431_CLOCK_HSE_8MHZ
        RCC_OSCILLATORTYPE_HSE;
#else
        RCC_OSCILLATORTYPE_HSI;
#endif
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
    oscillator.OscillatorType |= RCC_OSCILLATORTYPE_HSI48;
    oscillator.HSI48State = RCC_HSI48_ON;
#endif
#ifdef CONFIG_G431_CLOCK_HSE_8MHZ
    oscillator.HSEState = RCC_HSE_ON;
#else
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
#endif
    oscillator.PLL.PLLState = RCC_PLL_ON;
#ifdef CONFIG_G431_CLOCK_HSE_8MHZ
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLM = RCC_PLLM_DIV2;
#else
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLM = RCC_PLLM_DIV4;
#endif
    oscillator.PLL.PLLN = 85U;
    oscillator.PLL.PLLP = RCC_PLLP_DIV2;
    oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
    oscillator.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        fatal_error();
    }

    clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV1;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_4) != HAL_OK) {
        fatal_error();
    }

    RCC_PeriphCLKInitTypeDef peripheral_clock = {0};
#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
    peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    peripheral_clock.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;
#else
    peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_USB;
    peripheral_clock.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
#endif
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
        fatal_error();
    }
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {0};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue =
        __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);
#endif
}

static GPIO_TypeDef* gpio_port_from_index(uint8_t index) {
    if (index == 0U) {
        return GPIOA;
    }
    if (index == 1U) {
        return GPIOB;
    }
    if (index == 2U) {
        return GPIOC;
    }
    return NULL;
}

static void enable_gpio_clock(uint8_t index) {
    if (index == 0U) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    } else if (index == 1U) {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    } else if (index == 2U) {
        __HAL_RCC_GPIOC_CLK_ENABLE();
    }
}

static bool gpio_pin_present(uint16_t encoded_pin) {
    const uint8_t port = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin = (uint8_t)(encoded_pin % 16U);
    /* WeAct QFN48 实际引出完整 GPIOA/GPIOB，以及 PC4/PC6/PC10/PC11/PC13。 */
    if (port > 2U) {
        return false;
    }
    if (port == 2U && pin != 4U && pin != 6U && pin != 10U &&
        pin != 11U && pin != 13U) {
        return false;
    }
    if (port == 0U && (pin == 13U || pin == 14U)) {
        return false;
    }
#if defined(CONFIG_REMOTEBSP_TRANSPORT_USB)
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#elif defined(CONFIG_CAN_PINS_PA11_PA12)
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 8U || pin == 9U)) {
        return false;
    }
#endif
#ifdef CONFIG_CAN_TRANSCEIVER_STB_ENABLE
    static const char stb_port[] = CONFIG_CAN_TRANSCEIVER_STB_PORT;
    const uint8_t stb_port_index =
        (uint8_t)(stb_port[0] - 'A');
    if (port == stb_port_index &&
        pin == CONFIG_CAN_TRANSCEIVER_STB_PIN) {
        return false;
    }
#endif
    return true;
}

static bool gpio_pin_reserved_by_board(uint16_t encoded_pin) {
#ifdef CONFIG_WEACT_G431_PC6_PWM_BREATHING_LED
    return encoded_pin == (2U * 16U + 6U);
#else
    (void)encoded_pin;
    return false;
#endif
}

#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
static bool soft_uart_pin_reserved(uint16_t encoded_pin);
static bool soft_uart_configuration_valid(bool used[64]);
#endif

#ifdef CONFIG_REMOTEBSP_MOTION
static bool motion_pin_reserved(uint16_t encoded_pin) {
    const uint8_t port = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin = (uint8_t)(encoded_pin % 16U);
    for (uint8_t axis = 0U; axis < MOTION_SLOT_AXIS_COUNT; ++axis) {
        const motion_axis_pins_t* const pins = &motion_axis_pins[axis];
        if ((pins->step.port == port && pins->step.pin == pin) ||
            (pins->direction.port == port && pins->direction.pin == pin) ||
            (pins->enable.port == port && pins->enable.pin == pin)) {
            return true;
        }
    }
    return false;
}
#endif

#if defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
static bool motion_slot_configuration_valid(void) {
    bool used[64] = {false};
#ifdef CONFIG_REMOTEBSP_MOTION
    bool enable_used[64] = {false};
    bool enable_active_low[64] = {false};
    for (uint8_t axis = 0U; axis < MOTION_SLOT_AXIS_COUNT; ++axis) {
        const motion_axis_pins_t* const pins = &motion_axis_pins[axis];
        const uint16_t values[] = {
            (uint16_t)(pins->step.port * 16U + pins->step.pin),
            (uint16_t)(pins->direction.port * 16U + pins->direction.pin),
        };
        for (uint8_t index = 0U; index < 2U; ++index) {
            if (values[index] >= 64U || used[values[index]] ||
                !gpio_pin_present(values[index]) ||
                gpio_pin_reserved_by_board(values[index])) {
                return false;
            }
            used[values[index]] = true;
        }
        const uint16_t enable = (uint16_t)(
            pins->enable.port * 16U + pins->enable.pin);
        if (enable >= 64U || !gpio_pin_present(enable) ||
            gpio_pin_reserved_by_board(enable) ||
            (used[enable] && !enable_used[enable]) ||
            (enable_used[enable] &&
             enable_active_low[enable] != pins->enable_active_low)) {
            return false;
        }
        used[enable] = true;
        enable_used[enable] = true;
        enable_active_low[enable] = pins->enable_active_low;
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    if (!soft_uart_configuration_valid(used)) {
        return false;
    }
#endif
    return true;
}
#endif

static bool gpio_pin_allowed(uint16_t encoded_pin) {
    if (!gpio_pin_present(encoded_pin) ||
        gpio_pin_reserved_by_board(encoded_pin)) {
        return false;
    }
#ifdef CONFIG_REMOTEBSP_MOTION
    if (motion_pin_reserved(encoded_pin)) {
        return false;
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    if (soft_uart_pin_reserved(encoded_pin)) {
        return false;
    }
#endif
    return true;
}

#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
static void transceiver_enable(void) {
#ifdef CONFIG_CAN_TRANSCEIVER_STB_ENABLE
    static const char stb_port_name[] =
        CONFIG_CAN_TRANSCEIVER_STB_PORT;
    if (stb_port_name[0] < 'A' || stb_port_name[0] > 'C' ||
        CONFIG_CAN_TRANSCEIVER_STB_PIN > 15) {
        fatal_error();
    }
    const uint8_t port_index =
        (uint8_t)(stb_port_name[0] - 'A');
    GPIO_TypeDef* port = gpio_port_from_index(port_index);
    enable_gpio_clock(port_index);
    HAL_GPIO_WritePin(
        port, (uint16_t)(1U << CONFIG_CAN_TRANSCEIVER_STB_PIN),
        GPIO_PIN_RESET);
    GPIO_InitTypeDef init = {0};
    init.Pin = (uint16_t)(1U << CONFIG_CAN_TRANSCEIVER_STB_PIN);
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &init);
#endif
}
#endif /* CONFIG_REMOTEBSP_TRANSPORT_CAN */

static bool board_startup_gpio_apply(
    uint16_t encoded_pin, rbsp_startup_gpio_mode_t mode) {
    /* 上电安全状态必须能预置板载 LED 和运动专用输出，但不能碰 CAN/SWD。 */
    if (!gpio_pin_present(encoded_pin)) {
        return false;
    }
    const uint8_t port_index = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin_index = (uint8_t)(encoded_pin % 16U);
    GPIO_TypeDef* port = gpio_port_from_index(port_index);
    enable_gpio_clock(port_index);
    const bool output = mode == RBSP_STARTUP_GPIO_OUTPUT_LOW ||
                        mode == RBSP_STARTUP_GPIO_OUTPUT_HIGH;
    if (output) {
        HAL_GPIO_WritePin(
            port, (uint16_t)(1U << pin_index),
            mode == RBSP_STARTUP_GPIO_OUTPUT_HIGH
                ? GPIO_PIN_SET
                : GPIO_PIN_RESET);
    }
    GPIO_InitTypeDef init = {0};
    init.Pin = (uint16_t)(1U << pin_index);
    init.Mode = output ? GPIO_MODE_OUTPUT_PP : GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    if (mode == RBSP_STARTUP_GPIO_INPUT_PULLUP) {
        init.Pull = GPIO_PULLUP;
    } else if (mode == RBSP_STARTUP_GPIO_INPUT_PULLDOWN) {
        init.Pull = GPIO_PULLDOWN;
    } else if (!output && mode != RBSP_STARTUP_GPIO_INPUT_FLOATING) {
        return false;
    }
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &init);
    return true;
}

static void startup_gpio_configure(void) {
    if (!rbsp_startup_gpio_apply(
            CONFIG_STARTUP_GPIO_OUTPUT_LOW,
            CONFIG_STARTUP_GPIO_OUTPUT_HIGH,
            CONFIG_STARTUP_GPIO_INPUT_FLOATING,
            CONFIG_STARTUP_GPIO_INPUT_PULLUP,
            CONFIG_STARTUP_GPIO_INPUT_PULLDOWN,
            board_startup_gpio_apply)) {
        fatal_error();
    }
}

static bool board_gpio_configure(uint16_t encoded_pin,
                                 rbsp_gpio_direction_t direction,
                                 bool initial_value) {
    if (!gpio_pin_allowed(encoded_pin) ||
        gpio_pin_reserved_by_board(encoded_pin)) {
        return false;
    }
    const uint8_t port_index = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin_index = (uint8_t)(encoded_pin % 16U);
    GPIO_TypeDef* port = gpio_port_from_index(port_index);
    enable_gpio_clock(port_index);
    if (direction == RBSP_GPIO_OUTPUT) {
        HAL_GPIO_WritePin(port, (uint16_t)(1U << pin_index),
                          initial_value ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
    GPIO_InitTypeDef init = {0};
    init.Pin = (uint16_t)(1U << pin_index);
    init.Mode = direction == RBSP_GPIO_OUTPUT
                    ? GPIO_MODE_OUTPUT_PP
                    : GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &init);
    return true;
}

static bool board_gpio_write(uint16_t encoded_pin, bool value) {
    if (!gpio_pin_allowed(encoded_pin) ||
        gpio_pin_reserved_by_board(encoded_pin)) {
        return false;
    }
    GPIO_TypeDef* port =
        gpio_port_from_index((uint8_t)(encoded_pin / 16U));
    HAL_GPIO_WritePin(
        port, (uint16_t)(1U << (encoded_pin % 16U)),
        value ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

static bool board_gpio_read(uint16_t encoded_pin, bool* value) {
    if (!gpio_pin_allowed(encoded_pin) ||
        gpio_pin_reserved_by_board(encoded_pin) || value == NULL) {
        return false;
    }
    GPIO_TypeDef* port =
        gpio_port_from_index((uint8_t)(encoded_pin / 16U));
    *value = HAL_GPIO_ReadPin(
                 port, (uint16_t)(1U << (encoded_pin % 16U))) ==
             GPIO_PIN_SET;
    return true;
}

#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
/* 每个已启用 TMC2209 槽位各占用一条单线半双工 UART。 */
enum { RBSP_SOFT_UART_SLOT_CAPACITY = 5U };

static rbsp_soft_half_duplex_uart_t soft_uart;
static const bool soft_uart_port_present[RBSP_SOFT_UART_SLOT_CAPACITY] = {
#ifdef CONFIG_MOTION_SLOT0_DRIVER_TMC2209_UART
    true,
#else
    false,
#endif
#ifdef CONFIG_MOTION_SLOT1_DRIVER_TMC2209_UART
    true,
#else
    false,
#endif
#ifdef CONFIG_MOTION_SLOT2_DRIVER_TMC2209_UART
    true,
#else
    false,
#endif
#ifdef CONFIG_MOTION_SLOT3_DRIVER_TMC2209_UART
    true,
#else
    false,
#endif
#ifdef CONFIG_MOTION_SLOT4_DRIVER_TMC2209_UART
    true,
#else
    false,
#endif
};

static const uint8_t soft_uart_gpio_ports[RBSP_SOFT_UART_SLOT_CAPACITY] = {
#ifdef CONFIG_MOTION_SLOT0_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT0_TMC_UART_PIN / 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT1_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT1_TMC_UART_PIN / 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT2_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT2_TMC_UART_PIN / 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT3_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT3_TMC_UART_PIN / 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT4_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT4_TMC_UART_PIN / 16U,
#else
    0U,
#endif
};

static const uint8_t soft_uart_gpio_pins[RBSP_SOFT_UART_SLOT_CAPACITY] = {
#ifdef CONFIG_MOTION_SLOT0_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT0_TMC_UART_PIN % 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT1_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT1_TMC_UART_PIN % 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT2_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT2_TMC_UART_PIN % 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT3_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT3_TMC_UART_PIN % 16U,
#else
    0U,
#endif
#ifdef CONFIG_MOTION_SLOT4_DRIVER_TMC2209_UART
    CONFIG_MOTION_SLOT4_TMC_UART_PIN % 16U,
#else
    0U,
#endif
};

static bool soft_uart_pin_reserved(uint16_t encoded_pin) {
    for (uint8_t port = 0U;
         port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT &&
         port < RBSP_SOFT_UART_SLOT_CAPACITY;
         ++port) {
        if (soft_uart_port_present[port] &&
            encoded_pin == (uint16_t)(soft_uart_gpio_ports[port] * 16U +
                                      soft_uart_gpio_pins[port])) {
            return true;
        }
    }
    return false;
}

static bool soft_uart_configuration_valid(bool used[64]) {
    for (uint8_t port = 0U;
         port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT &&
         port < RBSP_SOFT_UART_SLOT_CAPACITY;
         ++port) {
        if (!soft_uart_port_present[port]) {
            continue;
        }
        const uint16_t encoded_pin =
            (uint16_t)(soft_uart_gpio_ports[port] * 16U +
                       soft_uart_gpio_pins[port]);
        if (encoded_pin >= 64U || used[encoded_pin] ||
            !gpio_pin_present(encoded_pin) ||
            gpio_pin_reserved_by_board(encoded_pin)) {
            return false;
        }
        used[encoded_pin] = true;
    }
    return true;
}

static GPIO_TypeDef* soft_uart_gpio(uint8_t port) {
    return port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT &&
                   port < RBSP_SOFT_UART_SLOT_CAPACITY &&
                   soft_uart_port_present[port]
               ? gpio_port_from_index(soft_uart_gpio_ports[port])
               : NULL;
}

static uint16_t soft_uart_mask(uint8_t port) {
    return port < CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT &&
                   port < RBSP_SOFT_UART_SLOT_CAPACITY &&
                   soft_uart_port_present[port]
               ? (uint16_t)(1U << soft_uart_gpio_pins[port])
               : 0U;
}

static bool soft_uart_port_prepare(void* context, uint8_t port) {
    (void)context;
    if (soft_uart_gpio(port) == NULL || soft_uart_mask(port) == 0U) {
        return false;
    }
    enable_gpio_clock(soft_uart_gpio_ports[port]);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    return true;
}

static bool soft_uart_set_input(void* context, uint8_t port) {
    (void)context;
    GPIO_TypeDef* const gpio = soft_uart_gpio(port);
    const uint16_t mask = soft_uart_mask(port);
    if (gpio == NULL || mask == 0U) {
        return false;
    }
    GPIO_InitTypeDef init = {0};
    init.Pin = mask;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(gpio, &init);
    return true;
}

static bool soft_uart_set_output(void* context, uint8_t port, bool high) {
    (void)context;
    GPIO_TypeDef* const gpio = soft_uart_gpio(port);
    const uint16_t mask = soft_uart_mask(port);
    if (gpio == NULL || mask == 0U) {
        return false;
    }
    gpio->BSRR = high ? mask : (uint32_t)mask << 16U;
    GPIO_InitTypeDef init = {0};
    init.Pin = mask;
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(gpio, &init);
    return true;
}

static bool soft_uart_read_input(void* context, uint8_t port, bool* high) {
    (void)context;
    GPIO_TypeDef* const gpio = soft_uart_gpio(port);
    const uint16_t mask = soft_uart_mask(port);
    if (gpio == NULL || mask == 0U || high == NULL) {
        return false;
    }
    *high = (gpio->IDR & mask) != 0U;
    return true;
}

static void soft_uart_delay_ticks(void* context, uint32_t ticks) {
    (void)context;
    const uint32_t started = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - started) < ticks) {
    }
}

static uint32_t soft_uart_irq_save_disable(void* context) {
    (void)context;
    const uint32_t state = __get_PRIMASK();
    __disable_irq();
    return state;
}

static void soft_uart_irq_restore(void* context, uint32_t state) {
    (void)context;
    if (state == 0U) {
        __enable_irq();
    }
}

static bool board_soft_uart_configure(uint8_t port, uint32_t baud,
                                      uint8_t bits, uint8_t stop,
                                      uint8_t parity) {
    return rbsp_soft_half_duplex_uart_configure(
        &soft_uart, port, baud, bits, stop, parity);
}

static size_t board_soft_uart_read(uint8_t port, uint8_t* data,
                                   size_t capacity) {
    return rbsp_soft_half_duplex_uart_read(&soft_uart, port, data, capacity);
}

static bool board_soft_uart_write(uint8_t port, const uint8_t* data,
                                  size_t length) {
#ifdef CONFIG_REMOTEBSP_MOTION
    if (remote_core.motion.state == RBSP_MOTION_ARMED ||
        remote_core.motion.state == RBSP_MOTION_RUNNING) {
        return false;
    }
#endif
    return rbsp_soft_half_duplex_uart_write(&soft_uart, port, data, length);
}
#endif

#ifdef CONFIG_REMOTEBSP_MOTION
static void motion_write_pin(motion_pin_t pin, bool high) {
    GPIO_TypeDef* const gpio = gpio_port_from_index(pin.port);
    const uint16_t mask = (uint16_t)(1U << pin.pin);
    gpio->BSRR = high ? mask : (uint32_t)mask << 16U;
}

static void motion_configure_output(motion_pin_t pin, bool initial_high) {
    GPIO_TypeDef* const gpio = gpio_port_from_index(pin.port);
    const uint16_t mask = (uint16_t)(1U << pin.pin);
    enable_gpio_clock(pin.port);
    motion_write_pin(pin, initial_high);
    GPIO_InitTypeDef init = {0};
    init.Pin = mask;
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(gpio, &init);
}

/* 上电先关闭每一路 EN，再把 STEP 固定为低，避免复位和初始化期间误动作。 */
static void motion_outputs_configure(void) {
    for (uint8_t axis = 0U; axis < MOTION_SLOT_AXIS_COUNT; ++axis) {
        const motion_axis_pins_t* const pins = &motion_axis_pins[axis];
        motion_configure_output(pins->enable, pins->enable_active_low);
        motion_configure_output(pins->step, false);
        motion_configure_output(pins->direction,
                                pins->direction_inverted);
    }
}

static bool board_motion_set_enable(uint8_t axis, bool enabled) {
    if (axis >= MOTION_SLOT_AXIS_COUNT) {
        return false;
    }
    const motion_axis_pins_t* const pins = &motion_axis_pins[axis];
    bool group_enabled = false;
    if (!rbsp_motion_shared_enable_update(
            motion_enable_group_ids, motion_enable_requests,
            MOTION_SLOT_AXIS_COUNT, axis, enabled, &group_enabled)) {
        return false;
    }
    motion_write_pin(pins->enable,
                     group_enabled != pins->enable_active_low);
    return true;
}

static bool board_motion_set_direction(uint8_t axis, bool positive) {
    if (axis >= MOTION_SLOT_AXIS_COUNT) {
        return false;
    }
    const motion_axis_pins_t* const pins = &motion_axis_pins[axis];
    motion_write_pin(pins->direction,
                     positive != pins->direction_inverted);
    return true;
}

static bool board_motion_set_step(uint8_t axis, bool high) {
    if (axis >= MOTION_SLOT_AXIS_COUNT) {
        return false;
    }
    motion_write_pin(motion_axis_pins[axis].step, high);
    return true;
}

/* TIM2 以 1MHz 自由运行，结合溢出中断构成单调的 64 位运动时钟。 */
static void motion_timebase_configure(void) {
    __HAL_RCC_TIM2_CLK_ENABLE();
    TIM2->CR1 = 0U;
    TIM2->PSC = 169U;
    TIM2->ARR = UINT32_MAX;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CNT = 0U;
    TIM2->SR = 0U;
    TIM2->DIER = TIM_DIER_UIE;
    motion_timebase_epochs = 0U;
    HAL_NVIC_SetPriority(TIM2_IRQn, 0U, 0U);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->CR1 = TIM_CR1_CEN;
}

static uint64_t board_motion_nanoseconds(void) {
    uint32_t before;
    uint32_t after;
    uint32_t counter;
    do {
        before = motion_timebase_epochs;
        counter = TIM2->CNT;
        after = motion_timebase_epochs;
    } while (before != after);
    if ((TIM2->SR & TIM_SR_UIF) != 0U &&
        counter < UINT32_C(0x80000000)) {
        ++after;
    }
    return (((uint64_t)after << 32U) | counter) * 1000ULL;
}

/* TIM2_CH1 只在下一条运动边沿到期时触发，不再占用 TIM4。 */
static bool motion_schedule_compare(uint64_t deadline_ns) {
    uint64_t deadline_us = deadline_ns / 1000ULL;
    if ((deadline_ns % 1000ULL) != 0U && deadline_us != UINT64_MAX) {
        ++deadline_us;
    }
    const uint64_t now_us = board_motion_nanoseconds() / 1000ULL;
    uint64_t delta_us = deadline_us > now_us
                            ? deadline_us - now_us
                            : 0U;
    if (delta_us < CONFIG_MOTION_COMPARE_MIN_LEAD_US) {
        delta_us = CONFIG_MOTION_COMPARE_MIN_LEAD_US;
    }
    if (delta_us > INT32_MAX) {
        delta_us = INT32_MAX;
    }

    TIM2->DIER &= ~TIM_DIER_CC1IE;
    uint32_t target = TIM2->CNT + (uint32_t)delta_us;
    TIM2->CCR1 = target;
    TIM2->SR = ~TIM_SR_CC1IF;
    if ((int32_t)(target - TIM2->CNT) <
        (int32_t)CONFIG_MOTION_COMPARE_MIN_LEAD_US) {
        target = TIM2->CNT + CONFIG_MOTION_COMPARE_MIN_LEAD_US;
        TIM2->CCR1 = target;
        TIM2->SR = ~TIM_SR_CC1IF;
    }
    TIM2->DIER |= TIM_DIER_CC1IE;
    return true;
}

static void motion_cancel_compare(void) {
    TIM2->DIER &= ~TIM_DIER_CC1IE;
    TIM2->SR = ~TIM_SR_CC1IF;
}

static uint32_t motion_enter_critical(void) {
    const uint32_t state = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return state;
}

static void motion_exit_critical(uint32_t state) {
    __DMB();
    if (state == 0U) {
        __enable_irq();
    }
}
#endif

#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef* handle) {
    if (handle->Instance != FDCAN1) {
        return;
    }
    __HAL_RCC_FDCAN_CLK_ENABLE();
    GPIO_InitTypeDef init = {0};
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = GPIO_AF9_FDCAN1;
#if defined(CONFIG_CAN_PINS_PA11_PA12)
    __HAL_RCC_GPIOA_CLK_ENABLE();
    init.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOA, &init);
#else
    __HAL_RCC_GPIOB_CLK_ENABLE();
    init.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &init);
#endif
}

static void fdcan_configure(void) {
    fdcan_handle.Instance = FDCAN1;
#ifdef CONFIG_CAN_FD_ENABLE
    fdcan_handle.Init.FrameFormat = FDCAN_FRAME_FD_BRS;
#else
    fdcan_handle.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
#endif
    fdcan_handle.Init.Mode = FDCAN_MODE_NORMAL;
    fdcan_handle.Init.AutoRetransmission = ENABLE;
    fdcan_handle.Init.TransmitPause = DISABLE;
    fdcan_handle.Init.ProtocolException = ENABLE;
    fdcan_handle.Init.NominalPrescaler = FDCAN_NOMINAL_PRESCALER;
    /* 34 TQ，采样点为 (1 + 29) / 34 = 88.24%，与常见 USB-CAN 的 87.5% 对齐。 */
    fdcan_handle.Init.NominalSyncJumpWidth = 4U;
    fdcan_handle.Init.NominalTimeSeg1 = 29U;
    fdcan_handle.Init.NominalTimeSeg2 = 4U;
    fdcan_handle.Init.DataPrescaler = FDCAN_DATA_PRESCALER;
    fdcan_handle.Init.DataSyncJumpWidth = 3U;
    fdcan_handle.Init.DataTimeSeg1 = 13U;
    fdcan_handle.Init.DataTimeSeg2 = 3U;
    fdcan_handle.Init.StdFiltersNbr = 1U;
    fdcan_handle.Init.ExtFiltersNbr = 0U;
    fdcan_handle.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    if (HAL_FDCAN_Init(&fdcan_handle) != HAL_OK) {
        fatal_error();
    }

    FDCAN_FilterTypeDef filter = {0};
    filter.IdType = FDCAN_STANDARD_ID;
    filter.FilterIndex = 0U;
    filter.FilterType = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1 = 0U;
    filter.FilterID2 = 0U;
    if (HAL_FDCAN_ConfigFilter(&fdcan_handle, &filter) != HAL_OK ||
        HAL_FDCAN_ConfigGlobalFilter(
            &fdcan_handle, FDCAN_REJECT, FDCAN_REJECT,
            FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK ||
        HAL_FDCAN_Start(&fdcan_handle) != HAL_OK) {
        fatal_error();
    }
}

static uint32_t fdcan_dlc_from_length(uint8_t length) {
    static const uint32_t dlc[] = {
        FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2,
        FDCAN_DLC_BYTES_3, FDCAN_DLC_BYTES_4, FDCAN_DLC_BYTES_5,
        FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7, FDCAN_DLC_BYTES_8};
    if (length <= 8U) {
        return dlc[length];
    }
    switch (length) {
        case 12U:
            return FDCAN_DLC_BYTES_12;
        case 16U:
            return FDCAN_DLC_BYTES_16;
        case 20U:
            return FDCAN_DLC_BYTES_20;
        case 24U:
            return FDCAN_DLC_BYTES_24;
        case 32U:
            return FDCAN_DLC_BYTES_32;
        case 48U:
            return FDCAN_DLC_BYTES_48;
        case 64U:
            return FDCAN_DLC_BYTES_64;
        default:
            return UINT32_MAX;
    }
}

static uint8_t fdcan_length_from_dlc(uint32_t dlc) {
    static const uint8_t lengths[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U,
        8U, 12U, 16U, 20U, 24U, 32U, 48U, 64U};
    return dlc < sizeof(lengths) ? lengths[dlc] : 0xFFU;
}

static bool board_can_send(const rbsp_can_frame_t* frame) {
    if (frame == NULL || frame->identifier > 0x7FFU) {
        return false;
    }
    const uint32_t dlc = fdcan_dlc_from_length(frame->length);
    if (dlc == UINT32_MAX) {
        return false;
    }
    const uint32_t started = HAL_GetTick();
    while (HAL_FDCAN_GetTxFifoFreeLevel(&fdcan_handle) == 0U) {
        if ((uint32_t)(HAL_GetTick() - started) >= 20U) {
            return false;
        }
    }

    FDCAN_TxHeaderTypeDef header = {0};
    header.Identifier = frame->identifier;
    header.IdType = FDCAN_STANDARD_ID;
    header.TxFrameType = FDCAN_DATA_FRAME;
    header.DataLength = dlc;
    header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
#ifdef CONFIG_CAN_FD_ENABLE
    header.BitRateSwitch = FDCAN_BRS_ON;
    header.FDFormat = FDCAN_FD_CAN;
#else
    header.BitRateSwitch = FDCAN_BRS_OFF;
    header.FDFormat = FDCAN_CLASSIC_CAN;
#endif
    header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    header.MessageMarker = 0U;
    return HAL_FDCAN_AddMessageToTxFifoQ(
               &fdcan_handle, &header,
               (uint8_t*)frame->data) == HAL_OK;
}

static void receive_can_frames(void) {
    while (HAL_FDCAN_GetRxFifoFillLevel(
               &fdcan_handle, FDCAN_RX_FIFO0) != 0U) {
        FDCAN_RxHeaderTypeDef header = {0};
        rbsp_can_frame_t frame = {0};
        if (HAL_FDCAN_GetRxMessage(&fdcan_handle, FDCAN_RX_FIFO0,
                                   &header, frame.data) != HAL_OK) {
            return;
        }
        const uint8_t length =
            fdcan_length_from_dlc(header.DataLength);
        if (header.IdType != FDCAN_STANDARD_ID ||
            header.RxFrameType != FDCAN_DATA_FRAME ||
            length == 0xFFU) {
            continue;
        }
        frame.identifier = header.Identifier;
        frame.length = length;
        rbsp_core_accept_can(&remote_core, &frame);
    }
}

#endif /* CONFIG_REMOTEBSP_TRANSPORT_CAN */

static void make_node_info(rbsp_node_info_t* info) {
    memset(info, 0, sizeof(*info));
    put_u32(info->uuid + 0U, HAL_GetUIDw0());
    put_u32(info->uuid + 4U, HAL_GetUIDw1());
    put_u32(info->uuid + 8U, HAL_GetUIDw2());
    put_u32(info->uuid + 12U, CONFIG_BOARD_TYPE);
    info->firmware_major = CONFIG_FIRMWARE_VERSION_MAJOR;
    info->firmware_minor = CONFIG_FIRMWARE_VERSION_MINOR;
    info->firmware_patch = CONFIG_FIRMWARE_VERSION_PATCH;
    info->board_type = CONFIG_BOARD_TYPE;
}

int main(void) {
    SCB->VTOR = FLASH_BASE + CONFIG_APPLICATION_FLASH_OFFSET;
    __DSB();
    __ISB();
    HAL_Init();
    system_clock_configure();
#if defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
    if (!motion_slot_configuration_valid()) {
        fatal_error();
    }
#endif
    startup_gpio_configure();
#if defined(CONFIG_REMOTEBSP_PWM) || defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    if (!rbsp_board_waveform_init()) {
        fatal_error();
    }
#endif
#ifdef CONFIG_WEACT_G431_PC6_PWM_BREATHING_LED
    led_pwm_configure();
#endif
#ifdef CONFIG_REMOTEBSP_MOTION
    motion_outputs_configure();
    motion_timebase_configure();
#endif
#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
    transceiver_enable();
    fdcan_configure();
#else
    if (!rbsp_usb_device_link_init()) {
        fatal_error();
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    const rbsp_soft_half_duplex_uart_hal_t soft_uart_hal = {
        .context = NULL,
        .port_prepare = soft_uart_port_prepare,
        .set_output = soft_uart_set_output,
        .set_input = soft_uart_set_input,
        .read_input = soft_uart_read_input,
        .timing_hz = CONFIG_SYSTEM_CLOCK_HZ,
        .delay_ticks = soft_uart_delay_ticks,
        .irq_save_disable = soft_uart_irq_save_disable,
        .irq_restore = soft_uart_irq_restore,
    };
    if (!rbsp_soft_half_duplex_uart_init(&soft_uart, &soft_uart_hal)) {
        fatal_error();
    }
#endif
    rbsp_node_info_t info;
    make_node_info(&info);
    const rbsp_hal_t hal = {
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
        .link_send = rbsp_usb_device_link_send,
#else
        .can_send = board_can_send,
#endif
        .milliseconds = HAL_GetTick,
        .gpio_configure = board_gpio_configure,
        .gpio_write = board_gpio_write,
        .gpio_read = board_gpio_read,
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
        .uart_configure = board_soft_uart_configure,
        .uart_read = board_soft_uart_read,
        .uart_write = board_soft_uart_write,
#endif
#ifdef CONFIG_REMOTEBSP_PWM
        .pwm_configure = rbsp_board_pwm_configure,
        .pwm_write = rbsp_board_pwm_write,
        .pwm_stop = rbsp_board_pwm_stop,
#endif
#ifdef CONFIG_REMOTEBSP_TIMED_BITSTREAM
        .timed_bitstream_configure = rbsp_board_timed_bitstream_configure,
        .timed_bitstream_write = rbsp_board_timed_bitstream_write,
        .timed_bitstream_busy = rbsp_board_timed_bitstream_busy,
        .timed_bitstream_abort = rbsp_board_timed_bitstream_abort,
#endif
#ifdef CONFIG_REMOTEBSP_MOTION
        .nanoseconds = board_motion_nanoseconds,
        .motion_axis_count = MOTION_SLOT_AXIS_COUNT,
        .motion_set_enable = board_motion_set_enable,
        .motion_set_direction = board_motion_set_direction,
        .motion_set_step = board_motion_set_step,
        .motion_schedule_compare = motion_schedule_compare,
        .motion_cancel_compare = motion_cancel_compare,
        .motion_enter_critical = motion_enter_critical,
        .motion_exit_critical = motion_exit_critical,
#endif
#ifdef CONFIG_APP_LAYOUT_KATAPULT_8K
        .enter_bootloader = board_enter_bootloader,
#endif
    };
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
    const rbsp_link_mode_t mode = RBSP_USB;
#elif defined(CONFIG_CAN_FD_ENABLE)
    const rbsp_link_mode_t mode = RBSP_CAN_FD;
#else
    const rbsp_link_mode_t mode = RBSP_CAN_CLASSICAL;
#endif
    if (!rbsp_core_init(&remote_core, &hal, mode, &info)) {
        fatal_error();
    }
    for (;;) {
#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
        rbsp_link_frame_t frame;
        while (rbsp_usb_device_link_receive(&frame)) {
            rbsp_core_accept_link(&remote_core, &frame);
        }
        rbsp_usb_device_link_poll();
#else
        receive_can_frames();
#endif
        rbsp_core_poll(&remote_core);
#ifdef CONFIG_WEACT_G431_PC6_PWM_BREATHING_LED
        led_pwm_poll();
#endif
    }
}

void SysTick_Handler(void) {
    HAL_IncTick();
}

#ifdef CONFIG_REMOTEBSP_MOTION
void TIM2_IRQHandler(void) {
    const uint32_t status = TIM2->SR;
    TIM2->SR = ~(status & (TIM_SR_UIF | TIM_SR_CC1IF));
    if ((status & TIM_SR_UIF) != 0U) {
        ++motion_timebase_epochs;
    }
    if ((status & TIM_SR_CC1IF) != 0U) {
        TIM2->DIER &= ~TIM_DIER_CC1IE;
        (void)rbsp_core_motion_service(&remote_core);
    }
}
#endif /* CONFIG_REMOTEBSP_MOTION */

#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
void USB_LP_IRQHandler(void) {
    rbsp_usb_device_link_irq_handler();
}

void USB_HP_IRQHandler(void) {
    rbsp_usb_device_link_irq_handler();
}
#endif
