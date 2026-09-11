#include "stm32g4xx_hal.h"

#include "remotebsp_embedded/board_config.h"
#if defined(CONFIG_REMOTEBSP_PWM) || defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
#include "remotebsp_embedded/board_waveform.h"
#endif
#include "remotebsp_embedded/byte_ring.h"
#include "remotebsp_embedded/core.h"
#include "stm32_health_hal.h"
#ifdef CONFIG_REMOTEBSP_BUS
#include "board_bus.h"
#endif
#ifdef CONFIG_REMOTEBSP_DEVICE_PARAMS
#include "remotebsp_embedded/device_parameter_flash.h"
#endif

#ifdef CONFIG_REMOTEBSP_BUS
#if !defined(CONFIG_G431_BUS_I2C1_PB6_PB7) && \
    !defined(CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15) && \
    !defined(CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12)
#error "G431启用总线核心时必须至少选择一个实体总线端点"
#endif
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
#ifdef CONFIG_UART0_PINS_PB6_PB7
#error "G431 I2C1 PB6/PB7与USART1重映射冲突"
#endif
#endif
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
#if defined(CONFIG_PWM0_PIN_PA6) || defined(CONFIG_TIMED_BITSTREAM0_PIN_PA7)
#error "G431 SPI1 PA5/PA6/PA7与所选波形端点冲突"
#endif
#endif
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
#include "remotebsp_embedded/soft_half_duplex_uart.h"
#endif
#include "remotebsp_embedded/startup_gpio.h"
#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
#include "remotebsp_static_resources.h"
_Static_assert(RBSP_STUDIO_RESOURCE_SCHEMA_VERSION ==
                   RBSP_STATIC_RESOURCE_TABLE_SCHEMA_VERSION,
               "Studio静态资源表schema版本不受固件支持");
_Static_assert(RBSP_STUDIO_RESOURCE_BOARD_TYPE == CONFIG_BOARD_TYPE,
               "Studio静态资源表与固件板型不匹配");
#endif
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

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 0
#define RBSP_HARDWARE_UART_ENABLED 1
#endif

#ifdef RBSP_HARDWARE_UART_ENABLED
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 3
#error "G431 最多提供 USART1、USART2、USART3 三路硬件 UART"
#endif

#if !defined(CONFIG_UART0_PINS_PA9_PA10) && \
    !defined(CONFIG_UART0_PINS_PB6_PB7)
#error "启用 G431 UART 时必须选择 USART1 引脚组"
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1 && \
    !defined(CONFIG_UART1_PINS_PA2_PA3)
#error "启用 G431 UART 1 时必须使用 USART2 PA2/PA3"
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2 && \
    !defined(CONFIG_UART2_PINS_PB10_PB11)
#error "启用 G431 UART 2 时必须使用 USART3 PB10/PB11"
#endif
#if CONFIG_UART_RX_BUFFER_SIZE < 2 || CONFIG_UART_TX_BUFFER_SIZE < 2
#error "UART 环形缓冲至少需要两个字节"
#endif
#endif

#if defined(CONFIG_PWM0_PIN_PB10) || defined(CONFIG_PWM1_PIN_PB11)
#if !defined(CONFIG_REMOTEBSP_PWM) || CONFIG_PWM_RESOURCE_COUNT != 2
#error "G431 PB10/PB11 PWM 映射必须启用两路 PWM 资源"
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2 || defined(CONFIG_UART2_PINS_PB10_PB11)
#error "G431 PB10/PB11 PWM 与 USART3 引脚冲突"
#endif
#ifdef CONFIG_REMOTEBSP_MOTION
#error "G431 PB10/PB11 PWM 与运动模块共用 TIM2，不能同时启用"
#endif
#if !defined(CONFIG_PWM0_PIN_PB10) || !defined(CONFIG_PWM1_PIN_PB11)
#error "G431 双 PWM 必须完整映射 PB10=TIM2_CH3、PB11=TIM2_CH4"
#endif
#endif

#ifdef CONFIG_REMOTEBSP_TRANSPORT_CAN
static FDCAN_HandleTypeDef fdcan_handle;
#endif
static rbsp_core_t remote_core;
static void fatal_error(void);

#ifdef RBSP_HARDWARE_UART_ENABLED
static UART_HandleTypeDef
    hardware_uart_handles[CONFIG_HARDWARE_UART_RESOURCE_COUNT];
static rbsp_byte_ring_t
    hardware_uart_rx_rings[CONFIG_HARDWARE_UART_RESOURCE_COUNT];
static rbsp_byte_ring_t
    hardware_uart_tx_rings[CONFIG_HARDWARE_UART_RESOURCE_COUNT];
static uint8_t hardware_uart_rx_storage
    [CONFIG_HARDWARE_UART_RESOURCE_COUNT][CONFIG_UART_RX_BUFFER_SIZE];
static uint8_t hardware_uart_tx_storage
    [CONFIG_HARDWARE_UART_RESOURCE_COUNT][CONFIG_UART_TX_BUFFER_SIZE];
static volatile bool
    hardware_uart_configured[CONFIG_HARDWARE_UART_RESOURCE_COUNT];
static volatile uint32_t
    hardware_uart_rx_overflows[CONFIG_HARDWARE_UART_RESOURCE_COUNT];

volatile uint32_t rbsp_diag_uart_rx_bytes;
volatile uint32_t rbsp_diag_uart_tx_bytes;
volatile uint32_t rbsp_diag_uart_rx_overflows;
volatile uint32_t rbsp_diag_uart_errors;
#endif

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
/* 活动映射由出厂menuconfig或已提交的运行时清单生成。 */
static rbsp_runtime_motion_axis_config_t
    active_motion_axes[CONFIG_MOTION_MAX_AXES];
static uint16_t active_motion_enable_group_ids[CONFIG_MOTION_MAX_AXES];
static bool motion_enable_requests[CONFIG_MOTION_MAX_AXES];
static uint8_t active_motion_axis_count;

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
    (void)encoded_pin;
#ifdef CONFIG_WEACT_G431_PC6_PWM_BREATHING_LED
    if (encoded_pin == (2U * 16U + 6U)) {
        return true;
    }
#endif
#ifdef CONFIG_REMOTEBSP_BUS
    if (rbsp_g431_bus_pin_reserved(encoded_pin)) {
        return true;
    }
#endif
    return false;
}

#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
static bool soft_uart_pin_reserved(uint16_t encoded_pin);
static bool soft_uart_configuration_valid(bool used[64]);
#endif

#ifdef CONFIG_REMOTEBSP_MOTION
static bool motion_pin_reserved(uint16_t encoded_pin) {
    for (uint8_t axis = 0U; axis < active_motion_axis_count; ++axis) {
        const rbsp_runtime_motion_axis_config_t* const config =
            &active_motion_axes[axis];
        if (config->step_pin == encoded_pin ||
            config->direction_pin == encoded_pin ||
            (config->enable_present && config->enable_pin == encoded_pin) ||
            config->limit_pin == encoded_pin) {
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
#ifdef CONFIG_REMOTEBSP_BUS
    if (rbsp_g431_bus_pin_reserved(encoded_pin)) {
        return false;
    }
#endif
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
#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
    if (!rbsp_static_resources_validate(
            rbsp_studio_gpio_resources, RBSP_STUDIO_GPIO_RESOURCE_COUNT,
            rbsp_studio_uart_resources, RBSP_STUDIO_UART_RESOURCE_COUNT,
            rbsp_studio_pwm_resources, RBSP_STUDIO_PWM_RESOURCE_COUNT,
            rbsp_studio_timed_bitstream_resources,
            RBSP_STUDIO_TIMED_BITSTREAM_RESOURCE_COUNT)) {
        fatal_error();
    }
    if (!rbsp_startup_gpio_apply_table(
            rbsp_studio_gpio_resources, RBSP_STUDIO_GPIO_RESOURCE_COUNT,
            board_startup_gpio_apply)) {
#else
    if (!rbsp_startup_gpio_apply(
            CONFIG_STARTUP_GPIO_OUTPUT_LOW,
            CONFIG_STARTUP_GPIO_OUTPUT_HIGH,
            CONFIG_STARTUP_GPIO_INPUT_FLOATING,
            CONFIG_STARTUP_GPIO_INPUT_PULLUP,
            CONFIG_STARTUP_GPIO_INPUT_PULLDOWN,
            board_startup_gpio_apply)) {
#endif
        fatal_error();
    }
}

static bool board_gpio_configure_pull(uint16_t encoded_pin,
                                      rbsp_gpio_direction_t direction,
                                      rbsp_gpio_pull_t pull,
                                      bool initial_value) {
    if (!gpio_pin_allowed(encoded_pin) ||
        gpio_pin_reserved_by_board(encoded_pin) ||
        pull > RBSP_GPIO_PULL_DOWN) {
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
    init.Pull = pull == RBSP_GPIO_PULL_UP
                    ? GPIO_PULLUP
                    : (pull == RBSP_GPIO_PULL_DOWN ? GPIO_PULLDOWN
                                                   : GPIO_NOPULL);
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &init);
    return true;
}

#ifdef CONFIG_REMOTEBSP_STATIC_GPIO_MAP
static bool board_gpio_resource_allowed(
    uint16_t pin, rbsp_gpio_direction_t direction) {
    rbsp_startup_gpio_mode_t mode;
#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
    if (!rbsp_startup_gpio_find_table(
            rbsp_studio_gpio_resources, RBSP_STUDIO_GPIO_RESOURCE_COUNT,
            pin, &mode)) {
#else
    if (!rbsp_startup_gpio_find(
            CONFIG_STARTUP_GPIO_OUTPUT_LOW,
            CONFIG_STARTUP_GPIO_OUTPUT_HIGH,
            CONFIG_STARTUP_GPIO_INPUT_FLOATING,
            CONFIG_STARTUP_GPIO_INPUT_PULLUP,
            CONFIG_STARTUP_GPIO_INPUT_PULLDOWN, pin, &mode)) {
#endif
        return false;
    }
    return direction == RBSP_GPIO_OUTPUT
        ? mode == RBSP_STARTUP_GPIO_OUTPUT_LOW ||
              mode == RBSP_STARTUP_GPIO_OUTPUT_HIGH
        : mode == RBSP_STARTUP_GPIO_INPUT_FLOATING ||
              mode == RBSP_STARTUP_GPIO_INPUT_PULLUP ||
              mode == RBSP_STARTUP_GPIO_INPUT_PULLDOWN;
}
#endif

static bool board_gpio_configure(uint16_t encoded_pin,
                                 rbsp_gpio_direction_t direction,
                                 bool initial_value) {
    return board_gpio_configure_pull(encoded_pin, direction,
                                     RBSP_GPIO_FLOATING, initial_value);
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

static uint64_t board_monotonic_microseconds(void) {
    static uint32_t previous_ms;
    static uint64_t epoch_ms;
    const uint32_t now_ms = HAL_GetTick();
    if (now_ms < previous_ms) epoch_ms += UINT64_C(1) << 32U;
    previous_ms = now_ms;
    return (epoch_ms + now_ms) * UINT64_C(1000);
}

#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
/* 每个已启用 TMC2209 槽位各占用一条单线半双工 UART。 */
enum { RBSP_SOFT_UART_SLOT_CAPACITY = 5U };

static rbsp_soft_half_duplex_uart_t soft_uart;
static const bool default_soft_uart_port_present[RBSP_SOFT_UART_SLOT_CAPACITY] = {
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

static const uint8_t default_soft_uart_gpio_ports[RBSP_SOFT_UART_SLOT_CAPACITY] = {
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

static const uint8_t default_soft_uart_gpio_pins[RBSP_SOFT_UART_SLOT_CAPACITY] = {
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

static bool soft_uart_port_present[RBSP_SOFT_UART_SLOT_CAPACITY];
static uint8_t soft_uart_gpio_ports[RBSP_SOFT_UART_SLOT_CAPACITY];
static uint8_t soft_uart_gpio_pins[RBSP_SOFT_UART_SLOT_CAPACITY];

static void soft_uart_load_default_mapping(void) {
    memcpy(soft_uart_port_present, default_soft_uart_port_present,
           sizeof(soft_uart_port_present));
    memcpy(soft_uart_gpio_ports, default_soft_uart_gpio_ports,
           sizeof(soft_uart_gpio_ports));
    memcpy(soft_uart_gpio_pins, default_soft_uart_gpio_pins,
           sizeof(soft_uart_gpio_pins));
}

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

static motion_pin_t motion_decode_pin(uint16_t encoded_pin) {
    const motion_pin_t pin = {
        (uint8_t)(encoded_pin / 16U), (uint8_t)(encoded_pin % 16U)};
    return pin;
}

static void motion_configure_limit_input(motion_pin_t pin,
                                         bool active_low) {
    GPIO_TypeDef* const gpio = gpio_port_from_index(pin.port);
    enable_gpio_clock(pin.port);
    GPIO_InitTypeDef init = {0};
    init.Pin = (uint16_t)(1U << pin.pin);
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = active_low ? GPIO_PULLUP : GPIO_PULLDOWN;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(gpio, &init);
}

static void motion_load_default_mapping(void) {
    memset(active_motion_axes, 0, sizeof(active_motion_axes));
    active_motion_axis_count = MOTION_SLOT_AXIS_COUNT;
    for (uint8_t axis = 0U; axis < MOTION_SLOT_AXIS_COUNT; ++axis) {
        const motion_axis_pins_t* const source = &motion_axis_pins[axis];
        rbsp_runtime_motion_axis_config_t* const destination =
            &active_motion_axes[axis];
        destination->logical_id = 0x09000000UL + axis;
        destination->maximum_step_rate_hz =
            CONFIG_MOTION_MAX_STEP_RATE_HZ;
        destination->step_pin = (uint16_t)(
            source->step.port * 16U + source->step.pin);
        destination->direction_pin = (uint16_t)(
            source->direction.port * 16U + source->direction.pin);
        destination->enable_pin = (uint16_t)(
            source->enable.port * 16U + source->enable.pin);
        destination->enable_present = true;
        destination->direction_inverted = source->direction_inverted;
        destination->enable_active_low = source->enable_active_low;
        destination->limit_pin = UINT16_MAX;
        active_motion_enable_group_ids[axis] =
            motion_enable_group_ids[axis];
    }
}

static void motion_apply_active_mapping(void) {
    memset(motion_enable_requests, 0, sizeof(motion_enable_requests));
    for (uint8_t axis = 0U; axis < active_motion_axis_count; ++axis) {
        const rbsp_runtime_motion_axis_config_t* const config =
            &active_motion_axes[axis];
        if (config->enable_present) {
            motion_configure_output(
                motion_decode_pin(config->enable_pin),
                config->enable_active_low);
            active_motion_enable_group_ids[axis] = config->enable_pin;
        } else {
            active_motion_enable_group_ids[axis] = UINT16_MAX;
        }
        motion_configure_output(motion_decode_pin(config->step_pin), false);
        motion_configure_output(
            motion_decode_pin(config->direction_pin),
            config->direction_inverted);
        if (config->limit_pin != UINT16_MAX) {
            motion_configure_limit_input(
                motion_decode_pin(config->limit_pin),
                config->limit_active_low);
        }
    }
}


/* 上电先关闭每一路 EN，再把 STEP 固定为低，避免复位和初始化期间误动作。 */
static void motion_outputs_configure(void) {
    motion_load_default_mapping();
    motion_apply_active_mapping();
}

static bool board_motion_set_enable(uint8_t axis, bool enabled) {
    if (axis >= active_motion_axis_count) {
        return false;
    }
    const rbsp_runtime_motion_axis_config_t* const config =
        &active_motion_axes[axis];
    if (!config->enable_present) {
        return true;
    }
    bool group_enabled = false;
    if (!rbsp_motion_shared_enable_update(
            active_motion_enable_group_ids, motion_enable_requests,
            active_motion_axis_count, axis, enabled, &group_enabled)) {
        return false;
    }
    motion_write_pin(motion_decode_pin(config->enable_pin),
                     group_enabled != config->enable_active_low);
    return true;
}

static bool board_motion_set_direction(uint8_t axis, bool positive) {
    if (axis >= active_motion_axis_count) {
        return false;
    }
    const rbsp_runtime_motion_axis_config_t* const config =
        &active_motion_axes[axis];
    motion_write_pin(motion_decode_pin(config->direction_pin),
                     positive != config->direction_inverted);
    return true;
}

static bool board_motion_set_step(uint8_t axis, bool high) {
    if (axis >= active_motion_axis_count) {
        return false;
    }
    motion_write_pin(
        motion_decode_pin(active_motion_axes[axis].step_pin), high);
    return true;
}

static bool board_motion_limit_active(uint8_t axis, bool* active) {
    if (axis >= active_motion_axis_count || active == NULL) {
        return false;
    }
    const rbsp_runtime_motion_axis_config_t* const config =
        &active_motion_axes[axis];
    if (config->limit_pin == UINT16_MAX) {
        *active = false;
        return true;
    }
    const motion_pin_t pin = motion_decode_pin(config->limit_pin);
    GPIO_TypeDef* const gpio = gpio_port_from_index(pin.port);
    const bool high = (gpio->IDR & (uint16_t)(1U << pin.pin)) != 0U;
    *active = config->limit_active_low ? !high : high;
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

#ifdef RBSP_HARDWARE_UART_ENABLED
void HAL_UART_MspInit(UART_HandleTypeDef* handle) {
    GPIO_InitTypeDef init = {0};
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = GPIO_AF7_USART1;

    if (handle->Instance == USART1) {
        __HAL_RCC_USART1_CLK_ENABLE();
#if defined(CONFIG_UART0_PINS_PA9_PA10)
        __HAL_RCC_GPIOA_CLK_ENABLE();
        init.Pin = GPIO_PIN_9 | GPIO_PIN_10;
        HAL_GPIO_Init(GPIOA, &init);
#else
        __HAL_RCC_GPIOB_CLK_ENABLE();
        init.Pin = GPIO_PIN_6 | GPIO_PIN_7;
        HAL_GPIO_Init(GPIOB, &init);
#endif
        return;
    }

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
    if (handle->Instance == USART2) {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        init.Alternate = GPIO_AF7_USART2;
        init.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        HAL_GPIO_Init(GPIOA, &init);
        return;
    }
#endif

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
    if (handle->Instance == USART3) {
        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        init.Alternate = GPIO_AF7_USART3;
        init.Pin = GPIO_PIN_10 | GPIO_PIN_11;
        HAL_GPIO_Init(GPIOB, &init);
    }
#endif
}

static USART_TypeDef* hardware_uart_instance(uint8_t port) {
    switch (port) {
        case 0U:
            return USART1;
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
        case 1U:
            return USART2;
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
        case 2U:
            return USART3;
#endif
        default:
            return NULL;
    }
}

static IRQn_Type hardware_uart_irq(uint8_t port) {
    switch (port) {
        case 0U:
            return USART1_IRQn;
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
        case 1U:
            return USART2_IRQn;
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
        case 2U:
            return USART3_IRQn;
#endif
        default:
            return NonMaskableInt_IRQn;
    }
}

static void board_hardware_uart_stop(uint8_t port) {
    if (port >= CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
        return;
    }
    UART_HandleTypeDef* const handle = &hardware_uart_handles[port];
    HAL_NVIC_DisableIRQ(hardware_uart_irq(port));
    hardware_uart_configured[port] = false;
    if (handle->Instance == hardware_uart_instance(port)) {
        (void)HAL_UART_DeInit(handle);
    }
    rbsp_byte_ring_clear(&hardware_uart_rx_rings[port]);
    rbsp_byte_ring_clear(&hardware_uart_tx_rings[port]);
}

static bool board_hardware_uart_configure(uint8_t port, uint32_t baud_rate,
                                          uint8_t data_bits,
                                          uint8_t stop_bits,
                                          uint8_t parity) {
    if (port >= CONFIG_HARDWARE_UART_RESOURCE_COUNT ||
        baud_rate < 300U || stop_bits < 1U || stop_bits > 2U || parity > 2U) {
        return false;
    }

    const uint32_t peripheral_clock =
        port == 0U ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
    if (baud_rate > peripheral_clock / 16U) {
        return false;
    }

    uint32_t word_length;
    if (parity == 0U && data_bits == 8U) {
        word_length = UART_WORDLENGTH_8B;
    } else if (parity != 0U && data_bits == 7U) {
        /* STM32 的字长包含校验位：7 数据位 + 1 校验位。 */
        word_length = UART_WORDLENGTH_8B;
    } else if (parity != 0U && data_bits == 8U) {
        /* STM32 的字长包含校验位：8 数据位 + 1 校验位。 */
        word_length = UART_WORDLENGTH_9B;
    } else {
        return false;
    }

    board_hardware_uart_stop(port);
    UART_HandleTypeDef* const handle = &hardware_uart_handles[port];
    memset(handle, 0, sizeof(*handle));
    handle->Instance = hardware_uart_instance(port);
    handle->Init.BaudRate = baud_rate;
    handle->Init.WordLength = word_length;
    handle->Init.StopBits =
        stop_bits == 1U ? UART_STOPBITS_1 : UART_STOPBITS_2;
    handle->Init.Parity =
        parity == 0U ? UART_PARITY_NONE
                     : (parity == 1U ? UART_PARITY_ODD : UART_PARITY_EVEN);
    handle->Init.Mode = UART_MODE_TX_RX;
    handle->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    handle->Init.OverSampling = UART_OVERSAMPLING_16;
    handle->Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    handle->Init.ClockPrescaler = UART_PRESCALER_DIV1;
    handle->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(handle) != HAL_OK) {
        return false;
    }

    __HAL_UART_CLEAR_FLAG(
        handle, UART_CLEAR_OREF | UART_CLEAR_NEF |
                    UART_CLEAR_FEF | UART_CLEAR_PEF);
    __HAL_UART_ENABLE_IT(handle, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(handle, UART_IT_ERR);
    hardware_uart_configured[port] = true;
    const IRQn_Type irq = hardware_uart_irq(port);
    HAL_NVIC_SetPriority(irq, 1U, 0U);
    HAL_NVIC_EnableIRQ(irq);
    return true;
}

static size_t board_hardware_uart_read(uint8_t port, uint8_t* data,
                                       size_t capacity) {
    if (port >= CONFIG_HARDWARE_UART_RESOURCE_COUNT ||
        !hardware_uart_configured[port] || data == NULL) {
        return 0U;
    }
    const uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    const size_t count =
        rbsp_byte_ring_read(&hardware_uart_rx_rings[port], data, capacity);
    if (interrupt_state == 0U) {
        __enable_irq();
    }
    return count;
}

static bool board_hardware_uart_write(uint8_t port, const uint8_t* data,
                                      size_t length) {
    if (port >= CONFIG_HARDWARE_UART_RESOURCE_COUNT ||
        !hardware_uart_configured[port] || data == NULL || length == 0U) {
        return false;
    }
    const uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    const bool accepted =
        rbsp_byte_ring_write_exact(&hardware_uart_tx_rings[port], data, length);
    if (accepted) {
        __HAL_UART_ENABLE_IT(&hardware_uart_handles[port], UART_IT_TXE);
    }
    if (interrupt_state == 0U) {
        __enable_irq();
    }
    return accepted;
}

static void hardware_uart_irq_service(uint8_t port) {
    if (port >= CONFIG_HARDWARE_UART_RESOURCE_COUNT ||
        !hardware_uart_configured[port]) {
        return;
    }

    UART_HandleTypeDef* const handle = &hardware_uart_handles[port];
    const uint32_t status = handle->Instance->ISR;
    const uint32_t receive_flags =
        USART_ISR_RXNE_RXFNE | USART_ISR_ORE | USART_ISR_NE |
        USART_ISR_FE | USART_ISR_PE;
    if ((status & receive_flags) != 0U) {
        if ((status & USART_ISR_RXNE_RXFNE) != 0U) {
            const uint8_t value = (uint8_t)handle->Instance->RDR;
            if (rbsp_byte_ring_push(&hardware_uart_rx_rings[port], value)) {
                ++rbsp_diag_uart_rx_bytes;
            } else {
                ++rbsp_diag_uart_rx_overflows;
                ++hardware_uart_rx_overflows[port];
            }
        }
        if ((status & (USART_ISR_ORE | USART_ISR_NE |
                       USART_ISR_FE | USART_ISR_PE)) != 0U) {
            handle->Instance->ICR =
                USART_ICR_ORECF | USART_ICR_NECF |
                USART_ICR_FECF | USART_ICR_PECF;
            ++rbsp_diag_uart_errors;
            ++hardware_uart_rx_overflows[port];
        }
    }

    if ((handle->Instance->ISR & USART_ISR_TXE_TXFNF) != 0U &&
        (handle->Instance->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U) {
        uint8_t value;
        if (rbsp_byte_ring_pop(&hardware_uart_tx_rings[port], &value)) {
            handle->Instance->TDR = value;
            ++rbsp_diag_uart_tx_bytes;
        } else {
            __HAL_UART_DISABLE_IT(handle, UART_IT_TXE);
        }
    }
}
#endif

#if CONFIG_UART_RESOURCE_COUNT > 0
static bool board_uart_configure(uint8_t port, uint32_t baud_rate,
                                 uint8_t data_bits, uint8_t stop_bits,
                                 uint8_t parity) {
#ifdef RBSP_HARDWARE_UART_ENABLED
    if (port < CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
        if (!rbsp_static_uart_allows(
                rbsp_studio_uart_resources,
                RBSP_STUDIO_UART_RESOURCE_COUNT, port, baud_rate)) {
            return false;
        }
#endif
        return board_hardware_uart_configure(
            port, baud_rate, data_bits, stop_bits, parity);
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    return board_soft_uart_configure(
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE),
        baud_rate, data_bits, stop_bits, parity);
#endif
    return false;
}

static size_t board_uart_read(uint8_t port, uint8_t* data,
                              size_t capacity) {
#ifdef RBSP_HARDWARE_UART_ENABLED
    if (port < CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
        return board_hardware_uart_read(port, data, capacity);
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    return board_soft_uart_read(
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE), data, capacity);
#endif
    return 0U;
}

static bool board_uart_write(uint8_t port, const uint8_t* data,
                             size_t length) {
#ifdef RBSP_HARDWARE_UART_ENABLED
    if (port < CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
        return board_hardware_uart_write(port, data, length);
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    return board_soft_uart_write(
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE), data, length);
#endif
    return false;
}

static bool board_uart_reset(uint8_t port) {
#ifdef RBSP_HARDWARE_UART_ENABLED
    if (port < CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
        board_hardware_uart_stop(port);
        hardware_uart_rx_overflows[port] = 0U;
        return true;
    }
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    return rbsp_soft_half_duplex_uart_reset(
        &soft_uart,
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE));
#else
    return false;
#endif
}

static bool board_uart_resource_status(
    uint8_t resource_type, uint16_t instance,
    rbsp_resource_runtime_status_t* status) {
    if (status == NULL) {
        return false;
    }
    if (resource_type != 2U) {
        return false;
    }
#ifdef RBSP_HARDWARE_UART_ENABLED
    if (instance < CONFIG_HARDWARE_UART_RESOURCE_COUNT) {
        const uint32_t interrupt_state = __get_PRIMASK();
        __disable_irq();
        status->rx_buffered = (uint32_t)rbsp_byte_ring_size(
            &hardware_uart_rx_rings[instance]);
        status->tx_buffered = (uint32_t)rbsp_byte_ring_size(
            &hardware_uart_tx_rings[instance]);
        status->rx_overruns = hardware_uart_rx_overflows[instance];
        if (interrupt_state == 0U) {
            __enable_irq();
        }
        return true;
    }
#endif
    return false;
}
#endif

#if CONFIG_UART_RESOURCE_COUNT > 0 || defined(CONFIG_REMOTEBSP_BUS)
static bool board_resource_status(
    uint8_t resource_type, uint16_t instance,
    rbsp_resource_runtime_status_t* status) {
#ifdef CONFIG_REMOTEBSP_BUS
    if (rbsp_g431_bus_resource_status(resource_type, instance, status)) {
        return true;
    }
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
    return board_uart_resource_status(resource_type, instance, status);
#else
    (void)resource_type;
    (void)instance;
    (void)status;
    return false;
#endif
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
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    soft_uart_load_default_mapping();
#endif
#if defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
    if (!motion_slot_configuration_valid()) {
        fatal_error();
    }
#endif
    startup_gpio_configure();
#ifdef CONFIG_REMOTEBSP_BUS
    if (!rbsp_g431_bus_init()) {
        fatal_error();
    }
#endif
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
#ifdef RBSP_HARDWARE_UART_ENABLED
    for (uint8_t port = 0U;
         port < CONFIG_HARDWARE_UART_RESOURCE_COUNT; ++port) {
        if (!rbsp_byte_ring_init(
                &hardware_uart_rx_rings[port],
                hardware_uart_rx_storage[port],
                sizeof(hardware_uart_rx_storage[port])) ||
            !rbsp_byte_ring_init(
                &hardware_uart_tx_rings[port],
                hardware_uart_tx_storage[port],
                sizeof(hardware_uart_tx_storage[port]))) {
            fatal_error();
        }
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
    (void)rbsp_stm32_health_init();
#ifdef CONFIG_REMOTEBSP_MOTION
    if (!rbsp_stm32_motion_epoch_init()) { fatal_error(); }
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
        .health_sample = rbsp_stm32_health_sample,
#ifdef CONFIG_REMOTEBSP_MOTION
        .motion_boot_epoch = rbsp_stm32_motion_boot_epoch,
#endif
        .microseconds = board_monotonic_microseconds,
        .gpio_configure = board_gpio_configure,
        .gpio_configure_pull = board_gpio_configure_pull,
        .gpio_write = board_gpio_write,
        .gpio_read = board_gpio_read,
#ifdef CONFIG_REMOTEBSP_BUS
        .bus_resources = rbsp_g431_bus_resources(),
        .bus_resource_count = rbsp_g431_bus_resource_count(),
        .i2c_transfer = rbsp_g431_i2c_transfer,
        .spi_transfer = rbsp_g431_spi_transfer,
#endif
#ifdef CONFIG_REMOTEBSP_STATIC_GPIO_MAP
        .gpio_resource_allowed = board_gpio_resource_allowed,
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0 || defined(CONFIG_REMOTEBSP_BUS)
        .resource_status = board_resource_status,
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
        .uart_configure = board_uart_configure,
        .uart_read = board_uart_read,
        .uart_write = board_uart_write,
        .uart_reset = board_uart_reset,
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
        .motion_limit_active = board_motion_limit_active,
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
#ifdef CONFIG_REMOTEBSP_DEVICE_PARAMS
    rbsp_device_param_backend device_param_backend;
    if (!rbsp_device_parameter_flash_backend_init(
            &device_param_backend) ||
        !rbsp_core_device_params_init(
            &remote_core, &device_param_backend)) {
        fatal_error();
    }
#endif
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

#ifdef RBSP_HARDWARE_UART_ENABLED
void USART1_IRQHandler(void) {
    hardware_uart_irq_service(0U);
}

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
void USART2_IRQHandler(void) {
    hardware_uart_irq_service(1U);
}
#endif

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
void USART3_IRQHandler(void) {
    hardware_uart_irq_service(2U);
}
#endif
#endif

#ifdef CONFIG_REMOTEBSP_TRANSPORT_USB
void USB_LP_IRQHandler(void) {
    rbsp_usb_device_link_irq_handler();
}

void USB_HP_IRQHandler(void) {
    rbsp_usb_device_link_irq_handler();
}
#endif
