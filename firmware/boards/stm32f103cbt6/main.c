#include "stm32f1xx_hal.h"

#include "remotebsp_embedded/board_config.h"
#if defined(CONFIG_REMOTEBSP_PWM) || defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
#include "remotebsp_embedded/board_waveform.h"
#endif
#include "remotebsp_embedded/byte_ring.h"
#include "remotebsp_embedded/core.h"
#ifdef CONFIG_REMOTEBSP_DEVICE_PARAMS
#include "remotebsp_embedded/device_parameter_flash.h"
#endif
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
#include "remotebsp_embedded/soft_half_duplex_uart.h"
#endif
#include "remotebsp_embedded/startup_gpio.h"
#include <string.h>

#ifdef CONFIG_F103_CLOCK_HSE_8MHZ
#if CONFIG_SYSTEM_CLOCK_HZ != 72000000 || HSE_VALUE != 8000000U
#error "F103 外部时钟方案要求 8MHz HSE，并产生 72MHz SYSCLK"
#endif
#define F103_CAN_CLOCK_HZ 36000000U
#define F103_CAN_TIME_QUANTA 18U
#define F103_CAN_BS1 CAN_BS1_15TQ
#else
#if CONFIG_SYSTEM_CLOCK_HZ != 64000000 || HSI_VALUE != 8000000U
#error "F103 内部时钟方案要求 8MHz HSI，并产生 64MHz SYSCLK"
#endif
#define F103_CAN_CLOCK_HZ 32000000U
#define F103_CAN_TIME_QUANTA 16U
#define F103_CAN_BS1 CAN_BS1_13TQ
#endif

#if (F103_CAN_CLOCK_HZ % (CONFIG_CAN_NOMINAL_BITRATE * F103_CAN_TIME_QUANTA)) != 0
#error "当前 F103 CAN 位时序无法精确生成所选仲裁段波特率"
#endif

#define CAN_PRESCALER \
    (F103_CAN_CLOCK_HZ / (CONFIG_CAN_NOMINAL_BITRATE * F103_CAN_TIME_QUANTA))

#if CAN_PRESCALER < 1 || CAN_PRESCALER > 1024
#error "F103 CAN 分频系数超出硬件范围"
#endif

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 0
#define RBSP_HARDWARE_UART_ENABLED 1
#endif

#ifdef RBSP_HARDWARE_UART_ENABLED
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 3
#error "F103 最多提供 USART1、USART2、USART3 三路硬件 UART"
#endif
#if !defined(CONFIG_UART0_PINS_PA9_PA10) && \
    !defined(CONFIG_UART0_PINS_PB6_PB7)
#error "启用 F103 UART 时必须选择 USART1 引脚组"
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1 && \
    !defined(CONFIG_UART1_PINS_PA2_PA3)
#error "启用 F103 UART 1 时必须使用 USART2 PA2/PA3"
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2 && \
    !defined(CONFIG_UART2_PINS_PB10_PB11)
#error "启用 F103 UART 2 时必须使用 USART3 PB10/PB11"
#endif
#if CONFIG_UART_RX_BUFFER_SIZE < 2 || CONFIG_UART_TX_BUFFER_SIZE < 2
#error "UART 环形缓冲至少需要两个字节"
#endif
#endif

static CAN_HandleTypeDef can_handle;
static rbsp_core_t remote_core;

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

#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
#define WEACT_BUTTON_ENCODED_PIN 0U
#define WEACT_LED_ENCODED_PIN 18U
#endif

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
#endif

/*
 * 实机联调诊断计数器。它们不参与协议，也不改变运行逻辑；调试器可以在
 * 系统运行时只读查看，用于区分物理接收、远程核心响应和发送失败。
 */
volatile uint32_t rbsp_diag_can_rx_frames;
volatile uint32_t rbsp_diag_can_tx_frames;
volatile uint32_t rbsp_diag_can_tx_failures;
volatile uint32_t rbsp_diag_can_rx_failures;
#ifdef RBSP_HARDWARE_UART_ENABLED
volatile uint32_t rbsp_diag_uart_rx_bytes;
volatile uint32_t rbsp_diag_uart_tx_bytes;
volatile uint32_t rbsp_diag_uart_rx_overflows;
volatile uint32_t rbsp_diag_uart_errors;
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
    const uint32_t ram_end = SRAM_BASE + (20U * 1024U);
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

    /* 外部 8MHz 优先得到 72MHz；无 HSE 的通用板可退回 HSI/2×16=64MHz。 */
#ifdef CONFIG_F103_CLOCK_HSE_8MHZ
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_ON;
    oscillator.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
#else
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
#endif
    oscillator.PLL.PLLState = RCC_PLL_ON;
#ifdef CONFIG_F103_CLOCK_HSE_8MHZ
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL9;
#else
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL16;
#endif
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        fatal_error();
    }

    clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV2;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK) {
        fatal_error();
    }

    /*
     * 时钟配置与 CAN 位时序不可分离。运行期再次核对，避免以后修改
     * 时钟源后仍生成一个“能启动但波特率错误”的固件。
     */
    SystemCoreClockUpdate();
    if (SystemCoreClock != CONFIG_SYSTEM_CLOCK_HZ ||
        HAL_RCC_GetPCLK1Freq() != F103_CAN_CLOCK_HZ) {
        fatal_error();
    }
}

static GPIO_TypeDef* gpio_port_from_index(uint8_t index) {
    switch (index) {
        case 0U:
            return GPIOA;
        case 1U:
            return GPIOB;
        case 2U:
            return GPIOC;
        case 3U:
            return GPIOD;
        default:
            return NULL;
    }
}

static void enable_gpio_clock(uint8_t index) {
    switch (index) {
        case 0U:
            __HAL_RCC_GPIOA_CLK_ENABLE();
            break;
        case 1U:
            __HAL_RCC_GPIOB_CLK_ENABLE();
            break;
        case 2U:
            __HAL_RCC_GPIOC_CLK_ENABLE();
            break;
        case 3U:
            __HAL_RCC_GPIOD_CLK_ENABLE();
            break;
        default:
            break;
    }
}

static bool gpio_pin_present(uint16_t encoded_pin) {
    const uint8_t port = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin = (uint8_t)(encoded_pin % 16U);
    if (port > 3U) {
        return false;
    }
    /* LQFP48 上 GPIOC 仅引出 13..15，GPIOD 仅引出 0..1。 */
    if ((port == 2U && pin < 13U) ||
        (port == 3U && pin > 1U)) {
        return false;
    }
    /* PA13/PA14 永久保留给 SWD，CAN 引脚也不允许被远程重配置。 */
    if (port == 0U && (pin == 13U || pin == 14U)) {
        return false;
    }
#ifdef CONFIG_F103_CLOCK_HSE_8MHZ
    /* PD0/PD1 是 OSC_IN/OSC_OUT，外部高速晶振启用时不得复用。 */
    if (port == 3U && (pin == 0U || pin == 1U)) {
        return false;
    }
#endif
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    /* 板载 USB 接口固定占用 PA11/PA12，不能作为远程 GPIO 重新配置。 */
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#endif
#ifdef CONFIG_BOARD_HAS_LSE_32768
    /* PC14/PC15 已连接 32.768kHz 晶振，留给未来 RTC/守时模块。 */
    if (port == 2U && (pin == 14U || pin == 15U)) {
        return false;
    }
#endif
#if defined(CONFIG_CAN_PINS_PA11_PA12)
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 8U || pin == 9U)) {
        return false;
    }
#endif
#ifdef RBSP_HARDWARE_UART_ENABLED
#if defined(CONFIG_UART0_PINS_PA9_PA10)
    if (port == 0U && (pin == 9U || pin == 10U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 6U || pin == 7U)) {
        return false;
    }
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
    if (port == 0U && (pin == 2U || pin == 3U)) {
        return false;
    }
#endif
#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
    if (port == 1U && (pin == 10U || pin == 11U)) {
        return false;
    }
#endif
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

#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
static bool soft_uart_pin_reserved(uint16_t encoded_pin);
static bool soft_uart_configuration_valid(bool used[64]);
#endif

#if defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
static bool motion_pin_reserved_by_board(uint16_t encoded_pin) {
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    return encoded_pin == WEACT_BUTTON_ENCODED_PIN ||
           encoded_pin == WEACT_LED_ENCODED_PIN;
#else
    (void)encoded_pin;
    return false;
#endif
}
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
                motion_pin_reserved_by_board(values[index])) {
                return false;
            }
            used[values[index]] = true;
        }
        const uint16_t enable = (uint16_t)(
            pins->enable.port * 16U + pins->enable.pin);
        if (enable >= 64U || !gpio_pin_present(enable) ||
            motion_pin_reserved_by_board(enable) ||
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
    if (!gpio_pin_present(encoded_pin)) {
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


#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
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
            motion_pin_reserved_by_board(encoded_pin)) {
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
    if (high) {
        gpio->BSRR = mask;
    } else {
        gpio->BRR = mask;
    }
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

static void transceiver_enable(void) {
#ifdef CONFIG_CAN_TRANSCEIVER_STB_ENABLE
    static const char stb_port_name[] =
        CONFIG_CAN_TRANSCEIVER_STB_PORT;
    if (stb_port_name[0] < 'A' || stb_port_name[0] > 'D' ||
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

static bool board_startup_gpio_apply(
    uint16_t encoded_pin, rbsp_startup_gpio_mode_t mode) {
    if (!gpio_pin_allowed(encoded_pin)) {
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

static bool board_gpio_configure_pull(uint16_t encoded_pin,
                                      rbsp_gpio_direction_t direction,
                                      rbsp_gpio_pull_t pull,
                                      bool initial_value) {
    if (!gpio_pin_allowed(encoded_pin) || pull > RBSP_GPIO_PULL_DOWN) {
        return false;
    }
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    if ((encoded_pin == WEACT_BUTTON_ENCODED_PIN &&
         direction != RBSP_GPIO_INPUT) ||
        (encoded_pin == WEACT_LED_ENCODED_PIN &&
         direction != RBSP_GPIO_OUTPUT)) {
        return false;
    }
#endif
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
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    if (encoded_pin == WEACT_BUTTON_ENCODED_PIN) {
        init.Pull = GPIO_PULLDOWN;
    }
#endif
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &init);
    return true;
}

#ifdef CONFIG_REMOTEBSP_STATIC_GPIO_MAP
static bool board_gpio_resource_allowed(
    uint16_t pin, rbsp_gpio_direction_t direction) {
    rbsp_startup_gpio_mode_t mode;
    if (!rbsp_startup_gpio_find(
            CONFIG_STARTUP_GPIO_OUTPUT_LOW,
            CONFIG_STARTUP_GPIO_OUTPUT_HIGH,
            CONFIG_STARTUP_GPIO_INPUT_FLOATING,
            CONFIG_STARTUP_GPIO_INPUT_PULLUP,
            CONFIG_STARTUP_GPIO_INPUT_PULLDOWN, pin, &mode)) {
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
    if (!gpio_pin_allowed(encoded_pin)) {
        return false;
    }
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    if (encoded_pin == WEACT_BUTTON_ENCODED_PIN) {
        return false;
    }
#endif
    GPIO_TypeDef* port =
        gpio_port_from_index((uint8_t)(encoded_pin / 16U));
    HAL_GPIO_WritePin(
        port, (uint16_t)(1U << (encoded_pin % 16U)),
        value ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

static bool board_gpio_read(uint16_t encoded_pin, bool* value) {
    if (!gpio_pin_allowed(encoded_pin) || value == NULL) {
        return false;
    }
    GPIO_TypeDef* port =
        gpio_port_from_index((uint8_t)(encoded_pin / 16U));
    *value = HAL_GPIO_ReadPin(
                 port, (uint16_t)(1U << (encoded_pin % 16U))) ==
             GPIO_PIN_SET;
    return true;
}

#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
static void board_fixed_io_configure(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /*
     * BluePill Plus 的 USB D+ 带硬件上拉。CAN APP 不运行 USB 协议栈时主动
     * 拉低 PA12，向主机表示 USB 已断开，避免出现“设备描述符请求失败”。
     * 进入 Katapult 会先复位 MCU，PA12 随后由 USB Bootloader 重新配置。
     */
#if defined(CONFIG_CAN_PINS_PB8_PB9)
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
    GPIO_InitTypeDef usb_disconnect = {0};
    usb_disconnect.Pin = GPIO_PIN_12;
    usb_disconnect.Mode = GPIO_MODE_OUTPUT_PP;
    usb_disconnect.Pull = GPIO_NOPULL;
    usb_disconnect.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &usb_disconnect);
#endif

    GPIO_InitTypeDef button = {0};
    button.Pin = GPIO_PIN_0;
    button.Mode = GPIO_MODE_INPUT;
    button.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &button);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
    GPIO_InitTypeDef led = {0};
    led.Pin = GPIO_PIN_2;
    led.Mode = GPIO_MODE_OUTPUT_PP;
    led.Pull = GPIO_NOPULL;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &led);
}

#ifdef CONFIG_WEACT_BLUEPILL_PLUS_PB2_BREATHING_LED
static void board_breathing_led_poll(void) {
    enum {
        /* PB2 没有可直接使用的硬件 PWM 通道，使用 100Hz、十级软件 PWM。 */
        BREATH_PWM_PERIOD_MS = 10U,
        BREATH_PERIOD_MS = 4000U,
        BREATH_HALF_PERIOD_MS = BREATH_PERIOD_MS / 2U,
    };

    static uint32_t last_tick = 0xFFFFFFFFU;
    const uint32_t now = HAL_GetTick();
    if (now == last_tick) {
        return;
    }
    last_tick = now;
    const uint32_t phase = now % BREATH_PERIOD_MS;
    const uint32_t ramp = phase < BREATH_HALF_PERIOD_MS
                              ? phase
                              : BREATH_PERIOD_MS - phase;
    const uint32_t duty_steps = (ramp * BREATH_PWM_PERIOD_MS) /
                                BREATH_HALF_PERIOD_MS;
    const uint32_t carrier = now % BREATH_PWM_PERIOD_MS;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2,
                      carrier < duty_steps ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
#else
static void board_breathing_led_poll(void) {
}
#endif
#endif

void HAL_CAN_MspInit(CAN_HandleTypeDef* handle) {
    if (handle->Instance != CAN1) {
        return;
    }
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    GPIO_InitTypeDef init = {0};
#if defined(CONFIG_CAN_PINS_PA11_PA12)
    __HAL_RCC_GPIOA_CLK_ENABLE();
    init.Pin = GPIO_PIN_11;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &init);
    init.Pin = GPIO_PIN_12;
    init.Mode = GPIO_MODE_AF_PP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &init);
#else
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_AFIO_REMAP_CAN1_2();
    init.Pin = GPIO_PIN_8;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &init);
    init.Pin = GPIO_PIN_9;
    init.Mode = GPIO_MODE_AF_PP;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &init);
#endif
}

#ifdef RBSP_HARDWARE_UART_ENABLED
void HAL_UART_MspInit(UART_HandleTypeDef* handle) {
    __HAL_RCC_AFIO_CLK_ENABLE();
    GPIO_InitTypeDef init = {0};

    if (handle->Instance == USART1) {
        __HAL_RCC_USART1_CLK_ENABLE();
#if defined(CONFIG_UART0_PINS_PA9_PA10)
        __HAL_RCC_GPIOA_CLK_ENABLE();
        init.Pin = GPIO_PIN_9;
        init.Mode = GPIO_MODE_AF_PP;
        init.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &init);
        init.Pin = GPIO_PIN_10;
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOA, &init);
#else
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_AFIO_REMAP_USART1_ENABLE();
        init.Pin = GPIO_PIN_6;
        init.Mode = GPIO_MODE_AF_PP;
        init.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOB, &init);
        init.Pin = GPIO_PIN_7;
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOB, &init);
#endif
        return;
    }

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 1
    if (handle->Instance == USART2) {
        __HAL_RCC_USART2_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        init.Pin = GPIO_PIN_2;
        init.Mode = GPIO_MODE_AF_PP;
        init.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOA, &init);
        init.Pin = GPIO_PIN_3;
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(GPIOA, &init);
        return;
    }
#endif

#if CONFIG_HARDWARE_UART_RESOURCE_COUNT > 2
    if (handle->Instance == USART3) {
        __HAL_RCC_USART3_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        init.Pin = GPIO_PIN_10;
        init.Mode = GPIO_MODE_AF_PP;
        init.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(GPIOB, &init);
        init.Pin = GPIO_PIN_11;
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLUP;
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
        baud_rate < 300U ||
        stop_bits < 1U || stop_bits > 2U || parity > 2U) {
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
        /* F1 的字长包含校验位：7 数据位 + 1 校验位。 */
        word_length = UART_WORDLENGTH_8B;
    } else if (parity != 0U && data_bits == 8U) {
        /* F1 的字长包含校验位：8 数据位 + 1 校验位。 */
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
                     : (parity == 1U ? UART_PARITY_ODD
                                     : UART_PARITY_EVEN);
    handle->Init.Mode = UART_MODE_TX_RX;
    handle->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    handle->Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(handle) != HAL_OK) {
        return false;
    }

    __HAL_UART_CLEAR_OREFLAG(handle);
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
        rbsp_byte_ring_write_exact(&hardware_uart_tx_rings[port],
                                   data, length);
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
    const uint32_t status = handle->Instance->SR;
    const uint32_t receive_flags =
        USART_SR_RXNE | USART_SR_ORE | USART_SR_NE |
        USART_SR_FE | USART_SR_PE;
    if ((status & receive_flags) != 0U) {
        /* F1通过依次读取SR和DR清除RXNE及接收错误标志。 */
        const uint8_t value = (uint8_t)handle->Instance->DR;
        if ((status & USART_SR_RXNE) != 0U) {
            if (rbsp_byte_ring_push(&hardware_uart_rx_rings[port], value)) {
                ++rbsp_diag_uart_rx_bytes;
            } else {
                ++rbsp_diag_uart_rx_overflows;
            }
        }
        if ((status & (USART_SR_ORE | USART_SR_NE |
                       USART_SR_FE | USART_SR_PE)) != 0U) {
            ++rbsp_diag_uart_errors;
        }
    }

    if ((handle->Instance->SR & USART_SR_TXE) != 0U &&
        (handle->Instance->CR1 & USART_CR1_TXEIE) != 0U) {
        uint8_t value;
        if (rbsp_byte_ring_pop(&hardware_uart_tx_rings[port], &value)) {
            handle->Instance->DR = value;
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
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE),
        data, capacity);
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
        (uint8_t)(port - CONFIG_TMC2209_UART_OBJECT_BASE),
        data, length);
#endif
    return false;
}
#endif

#ifdef CONFIG_REMOTEBSP_MOTION
static void motion_write_pin(motion_pin_t pin, bool high) {
    GPIO_TypeDef* const gpio = gpio_port_from_index(pin.port);
    const uint16_t mask = (uint16_t)(1U << pin.pin);
    if (high) {
        gpio->BSRR = mask;
    } else {
        gpio->BRR = mask;
    }
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


/* 上电先关闭每一路 EN，再把 STEP 固定为低，避免初始化期间误动作。 */
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
    TIM2->PSC = 71U;
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

/* TIM2_CH1 只在下一条运动边沿到期时触发，不再占用周期定时器。 */
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

static void can_configure(void) {
    can_handle.Instance = CAN1;
    can_handle.Init.Prescaler = CAN_PRESCALER;
    can_handle.Init.Mode = CAN_MODE_NORMAL;
    can_handle.Init.SyncJumpWidth = CAN_SJW_1TQ;
    can_handle.Init.TimeSeg1 = F103_CAN_BS1;
    can_handle.Init.TimeSeg2 = CAN_BS2_2TQ;
    can_handle.Init.TimeTriggeredMode = DISABLE;
    can_handle.Init.AutoBusOff = ENABLE;
    can_handle.Init.AutoWakeUp = DISABLE;
    can_handle.Init.AutoRetransmission = ENABLE;
    can_handle.Init.ReceiveFifoLocked = DISABLE;
    can_handle.Init.TransmitFifoPriority = ENABLE;
    if (HAL_CAN_Init(&can_handle) != HAL_OK) {
        fatal_error();
    }

    CAN_FilterTypeDef filter = {0};
    filter.FilterBank = 0U;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    if (HAL_CAN_ConfigFilter(&can_handle, &filter) != HAL_OK ||
        HAL_CAN_Start(&can_handle) != HAL_OK) {
        fatal_error();
    }
}

static bool board_can_send(const rbsp_can_frame_t* frame) {
    if (frame == NULL || frame->length > 8U ||
        frame->identifier > 0x7FFU) {
        return false;
    }
    const uint32_t started = HAL_GetTick();
    while (HAL_CAN_GetTxMailboxesFreeLevel(&can_handle) == 0U) {
        if ((uint32_t)(HAL_GetTick() - started) >= 20U) {
            return false;
        }
    }
    CAN_TxHeaderTypeDef header = {0};
    uint32_t mailbox = 0U;
    header.StdId = frame->identifier;
    header.IDE = CAN_ID_STD;
    header.RTR = CAN_RTR_DATA;
    header.DLC = frame->length;
    header.TransmitGlobalTime = DISABLE;
    if (HAL_CAN_AddTxMessage(&can_handle, &header,
                             (uint8_t*)frame->data,
                             &mailbox) != HAL_OK) {
        ++rbsp_diag_can_tx_failures;
        return false;
    }
    ++rbsp_diag_can_tx_frames;
    return true;
}

static void receive_can_frames(void) {
    while (HAL_CAN_GetRxFifoFillLevel(&can_handle, CAN_RX_FIFO0) != 0U) {
        CAN_RxHeaderTypeDef header = {0};
        rbsp_can_frame_t frame = {0};
        if (HAL_CAN_GetRxMessage(&can_handle, CAN_RX_FIFO0, &header,
                                 frame.data) != HAL_OK) {
            ++rbsp_diag_can_rx_failures;
            return;
        }
        if (header.IDE != CAN_ID_STD || header.RTR != CAN_RTR_DATA ||
            header.DLC > 8U) {
            continue;
        }
        ++rbsp_diag_can_rx_frames;
        frame.identifier = header.StdId;
        frame.length = (uint8_t)header.DLC;
        rbsp_core_accept_can(&remote_core, &frame);
    }
}

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
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
#ifdef CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART
    soft_uart_load_default_mapping();
#endif
#if defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
    if (!motion_slot_configuration_valid()) {
        fatal_error();
    }
#endif
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
    board_fixed_io_configure();
#endif
    startup_gpio_configure();
#if defined(CONFIG_REMOTEBSP_PWM) || defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    if (!rbsp_board_waveform_init()) {
        fatal_error();
    }
#endif
#ifdef CONFIG_REMOTEBSP_MOTION
    motion_outputs_configure();
    motion_timebase_configure();
#endif
    transceiver_enable();
    can_configure();
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

    rbsp_node_info_t info;
    make_node_info(&info);
    const rbsp_hal_t hal = {
        .can_send = board_can_send,
        .milliseconds = HAL_GetTick,
        .gpio_configure = board_gpio_configure,
        .gpio_configure_pull = board_gpio_configure_pull,
        .gpio_write = board_gpio_write,
        .gpio_read = board_gpio_read,
#ifdef CONFIG_REMOTEBSP_STATIC_GPIO_MAP
        .gpio_resource_allowed = board_gpio_resource_allowed,
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
        .uart_configure = board_uart_configure,
        .uart_read = board_uart_read,
        .uart_write = board_uart_write,
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
    if (!rbsp_core_init(&remote_core, &hal, RBSP_CAN_CLASSICAL,
                        &info)) {
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
#ifdef CONFIG_BOARD_WEACT_BLUEPILL_PLUS
        board_breathing_led_poll();
#endif
        receive_can_frames();
        rbsp_core_poll(&remote_core);
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
#endif

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
