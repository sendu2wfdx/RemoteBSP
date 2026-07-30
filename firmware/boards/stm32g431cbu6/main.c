#include "stm32g4xx_hal.h"

#include "remotebsp_embedded/board_config.h"
#include "remotebsp_embedded/core.h"
#ifdef CONFIG_USB_DEBUG_CDC
#include "remotebsp_embedded/usb_debug.h"
#endif

#include <string.h>

#if CONFIG_SYSTEM_CLOCK_HZ != 170000000
#error "STM32G431CBU6 当前时钟方案固定为 170MHz"
#endif

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

static FDCAN_HandleTypeDef fdcan_handle;
static rbsp_core_t remote_core;

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

    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
#ifdef CONFIG_USB_DEBUG_CDC
    oscillator.OscillatorType |= RCC_OSCILLATORTYPE_HSI48;
    oscillator.HSI48State = RCC_HSI48_ON;
#endif
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLM = RCC_PLLM_DIV4;
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
    peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    peripheral_clock.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;
#ifdef CONFIG_USB_DEBUG_CDC
    peripheral_clock.PeriphClockSelection |= RCC_PERIPHCLK_USB;
    peripheral_clock.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
#endif
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
        fatal_error();
    }
#ifdef CONFIG_USB_DEBUG_CDC
    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {0};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue =
        __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue =
        RCC_CRS_HSI48CALIBRATION_DEFAULT;
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
    return NULL;
}

static void enable_gpio_clock(uint8_t index) {
    if (index == 0U) {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    } else if (index == 1U) {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    }
}

static bool gpio_pin_allowed(uint16_t encoded_pin) {
    const uint8_t port = (uint8_t)(encoded_pin / 16U);
    const uint8_t pin = (uint8_t)(encoded_pin % 16U);
    /*
     * 首版仅开放 UFQFPN48 上完整的 GPIOA/GPIOB，避免把未键合管脚误报
     * 为可用资源；后续资源表会进一步按核心板实际引出情况裁剪。
     */
    if (port > 1U) {
        return false;
    }
    if (port == 0U && (pin == 13U || pin == 14U)) {
        return false;
    }
#if defined(CONFIG_CAN_PINS_PA11_PA12)
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 8U || pin == 9U)) {
        return false;
    }
#endif
#ifdef CONFIG_USB_DEBUG_CDC
    if (port == 0U && (pin == 11U || pin == 12U)) {
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

static void transceiver_enable(void) {
#ifdef CONFIG_CAN_TRANSCEIVER_STB_ENABLE
    static const char stb_port_name[] =
        CONFIG_CAN_TRANSCEIVER_STB_PORT;
    if (stb_port_name[0] < 'A' || stb_port_name[0] > 'B' ||
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

static bool board_gpio_configure(uint16_t encoded_pin,
                                 rbsp_gpio_direction_t direction,
                                 bool initial_value) {
    if (!gpio_pin_allowed(encoded_pin)) {
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
    if (!gpio_pin_allowed(encoded_pin)) {
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
    fdcan_handle.Init.NominalSyncJumpWidth = 6U;
    fdcan_handle.Init.NominalTimeSeg1 = 27U;
    fdcan_handle.Init.NominalTimeSeg2 = 6U;
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
    transceiver_enable();
    fdcan_configure();
#ifdef CONFIG_USB_DEBUG_CDC
    if (rbsp_usb_debug_init()) {
        (void)rbsp_usb_debug_write_text(
            "RemoteBSP STM32G431 APP ready\r\n");
    }
#endif

    rbsp_node_info_t info;
    make_node_info(&info);
    const rbsp_hal_t hal = {
        .can_send = board_can_send,
        .milliseconds = HAL_GetTick,
        .gpio_configure = board_gpio_configure,
        .gpio_write = board_gpio_write,
        .gpio_read = board_gpio_read,
#ifdef CONFIG_APP_LAYOUT_KATAPULT_8K
        .enter_bootloader = board_enter_bootloader,
#endif
    };
#ifdef CONFIG_CAN_FD_ENABLE
    const rbsp_can_mode_t mode = RBSP_CAN_FD;
#else
    const rbsp_can_mode_t mode = RBSP_CAN_CLASSICAL;
#endif
    if (!rbsp_core_init(&remote_core, &hal, mode, &info)) {
        fatal_error();
    }

    for (;;) {
        receive_can_frames();
        rbsp_core_poll(&remote_core);
#ifdef CONFIG_USB_DEBUG_CDC
        rbsp_usb_debug_poll();
#endif
    }
}

void SysTick_Handler(void) {
    HAL_IncTick();
}

#ifdef CONFIG_USB_DEBUG_CDC
void USB_LP_IRQHandler(void) {
    rbsp_usb_debug_irq_handler();
}

void USB_HP_IRQHandler(void) {
    rbsp_usb_debug_irq_handler();
}
#endif
