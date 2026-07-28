#include "stm32f1xx_hal.h"

#include "remotebsp_embedded/board_config.h"
#include "remotebsp_embedded/byte_ring.h"
#include "remotebsp_embedded/core.h"

#include <string.h>

#if CONFIG_SYSTEM_CLOCK_HZ != 72000000
#error "STM32F103CBT6 当前时钟方案固定为 72MHz"
#endif

#if HSE_VALUE != 8000000U
#error "STM32F103CBT6 当前时钟方案要求 8MHz 外部晶振"
#endif

#if (36000000U % (CONFIG_CAN_NOMINAL_BITRATE * 18U)) != 0
#error "当前 F103 CAN 位时序无法精确生成所选仲裁段波特率"
#endif

#define CAN_PRESCALER \
    (36000000U / (CONFIG_CAN_NOMINAL_BITRATE * 18U))

#if CAN_PRESCALER < 1 || CAN_PRESCALER > 1024
#error "F103 CAN 分频系数超出硬件范围"
#endif

#if CONFIG_UART_RESOURCE_COUNT > 0
#if !defined(CONFIG_UART0_PINS_PA9_PA10) && \
    !defined(CONFIG_UART0_PINS_PB6_PB7)
#error "启用 F103 UART 时必须选择 USART1 引脚组"
#endif
#if CONFIG_UART_RX_BUFFER_SIZE < 2 || CONFIG_UART_TX_BUFFER_SIZE < 2
#error "UART 环形缓冲至少需要两个字节"
#endif
#endif

static CAN_HandleTypeDef can_handle;
static rbsp_core_t remote_core;

#if CONFIG_UART_RESOURCE_COUNT > 0
static UART_HandleTypeDef uart0_handle;
static rbsp_byte_ring_t uart0_rx_ring;
static rbsp_byte_ring_t uart0_tx_ring;
static uint8_t uart0_rx_storage[CONFIG_UART_RX_BUFFER_SIZE];
static uint8_t uart0_tx_storage[CONFIG_UART_TX_BUFFER_SIZE];
static volatile bool uart0_configured;
#endif

/*
 * 实机联调诊断计数器。它们不参与协议，也不改变运行逻辑；调试器可以在
 * 系统运行时只读查看，用于区分物理接收、远程核心响应和发送失败。
 */
volatile uint32_t rbsp_diag_can_rx_frames;
volatile uint32_t rbsp_diag_can_tx_frames;
volatile uint32_t rbsp_diag_can_tx_failures;
volatile uint32_t rbsp_diag_can_rx_failures;
#if CONFIG_UART_RESOURCE_COUNT > 0
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

static void system_clock_configure(void) {
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};

    /*
     * F103 的 HSI 经 PLL 最多只能得到 64MHz。这里必须使用核心板上的
     * 8MHz HSE，经 9 倍频得到 72MHz；APB1 再二分频，为 bxCAN 提供
     * 与下方位时序计算一致的 36MHz 时钟。
     */
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_ON;
    oscillator.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL9;
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
        HAL_RCC_GetPCLK1Freq() != 36000000U) {
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

static bool gpio_pin_allowed(uint16_t encoded_pin) {
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
#if defined(CONFIG_CAN_PINS_PA11_PA12)
    if (port == 0U && (pin == 11U || pin == 12U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 8U || pin == 9U)) {
        return false;
    }
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
#if defined(CONFIG_UART0_PINS_PA9_PA10)
    if (port == 0U && (pin == 9U || pin == 10U)) {
        return false;
    }
#else
    if (port == 1U && (pin == 6U || pin == 7U)) {
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

#if CONFIG_UART_RESOURCE_COUNT > 0
void HAL_UART_MspInit(UART_HandleTypeDef* handle) {
    if (handle->Instance != USART1) {
        return;
    }
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    GPIO_InitTypeDef init = {0};
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
}

static bool board_uart_configure(uint8_t port, uint32_t baud_rate,
                                 uint8_t data_bits, uint8_t stop_bits,
                                 uint8_t parity) {
    if (port != 0U || baud_rate < 300U || baud_rate > 4500000U ||
        stop_bits < 1U || stop_bits > 2U || parity > 2U) {
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

    HAL_NVIC_DisableIRQ(USART1_IRQn);
    uart0_configured = false;
    if (uart0_handle.Instance == USART1) {
        (void)HAL_UART_DeInit(&uart0_handle);
    }
    rbsp_byte_ring_clear(&uart0_rx_ring);
    rbsp_byte_ring_clear(&uart0_tx_ring);

    uart0_handle.Instance = USART1;
    uart0_handle.Init.BaudRate = baud_rate;
    uart0_handle.Init.WordLength = word_length;
    uart0_handle.Init.StopBits =
        stop_bits == 1U ? UART_STOPBITS_1 : UART_STOPBITS_2;
    uart0_handle.Init.Parity =
        parity == 0U ? UART_PARITY_NONE
                     : (parity == 1U ? UART_PARITY_ODD
                                     : UART_PARITY_EVEN);
    uart0_handle.Init.Mode = UART_MODE_TX_RX;
    uart0_handle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart0_handle.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&uart0_handle) != HAL_OK) {
        return false;
    }

    __HAL_UART_CLEAR_OREFLAG(&uart0_handle);
    __HAL_UART_ENABLE_IT(&uart0_handle, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&uart0_handle, UART_IT_ERR);
    uart0_configured = true;
    HAL_NVIC_SetPriority(USART1_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    return true;
}

static size_t board_uart_read(uint8_t port, uint8_t* data,
                              size_t capacity) {
    if (port != 0U || !uart0_configured || data == NULL) {
        return 0U;
    }
    const uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    const size_t count =
        rbsp_byte_ring_read(&uart0_rx_ring, data, capacity);
    if (interrupt_state == 0U) {
        __enable_irq();
    }
    return count;
}

static bool board_uart_write(uint8_t port, const uint8_t* data,
                             size_t length) {
    if (port != 0U || !uart0_configured || data == NULL ||
        length == 0U) {
        return false;
    }
    const uint32_t interrupt_state = __get_PRIMASK();
    __disable_irq();
    const bool accepted =
        rbsp_byte_ring_write_exact(&uart0_tx_ring, data, length);
    if (accepted) {
        __HAL_UART_ENABLE_IT(&uart0_handle, UART_IT_TXE);
    }
    if (interrupt_state == 0U) {
        __enable_irq();
    }
    return accepted;
}
#endif

static void can_configure(void) {
    can_handle.Instance = CAN1;
    can_handle.Init.Prescaler = CAN_PRESCALER;
    can_handle.Init.Mode = CAN_MODE_NORMAL;
    can_handle.Init.SyncJumpWidth = CAN_SJW_1TQ;
    can_handle.Init.TimeSeg1 = CAN_BS1_15TQ;
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
    HAL_Init();
    system_clock_configure();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();
    transceiver_enable();
    can_configure();
#if CONFIG_UART_RESOURCE_COUNT > 0
    if (!rbsp_byte_ring_init(&uart0_rx_ring, uart0_rx_storage,
                             sizeof(uart0_rx_storage)) ||
        !rbsp_byte_ring_init(&uart0_tx_ring, uart0_tx_storage,
                             sizeof(uart0_tx_storage))) {
        fatal_error();
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
#if CONFIG_UART_RESOURCE_COUNT > 0
        .uart_configure = board_uart_configure,
        .uart_read = board_uart_read,
        .uart_write = board_uart_write,
#endif
    };
    if (!rbsp_core_init(&remote_core, &hal, RBSP_CAN_CLASSICAL,
                        &info)) {
        fatal_error();
    }

    for (;;) {
        receive_can_frames();
        rbsp_core_poll(&remote_core);
    }
}

void SysTick_Handler(void) {
    HAL_IncTick();
}

#if CONFIG_UART_RESOURCE_COUNT > 0
void USART1_IRQHandler(void) {
    if (!uart0_configured) {
        return;
    }

    const uint32_t status = uart0_handle.Instance->SR;
    const uint32_t receive_flags =
        USART_SR_RXNE | USART_SR_ORE | USART_SR_NE |
        USART_SR_FE | USART_SR_PE;
    if ((status & receive_flags) != 0U) {
        /*
         * F1 通过依次读取 SR 和 DR 清除 RXNE 及接收错误标志。
         * 即使同时出现 ORE，只要 RXNE 有效仍保留当前 DR 字节。
         */
        const uint8_t value = (uint8_t)uart0_handle.Instance->DR;
        if ((status & USART_SR_RXNE) != 0U) {
            if (rbsp_byte_ring_push(&uart0_rx_ring, value)) {
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

    if ((uart0_handle.Instance->SR & USART_SR_TXE) != 0U &&
        (uart0_handle.Instance->CR1 & USART_CR1_TXEIE) != 0U) {
        uint8_t value;
        if (rbsp_byte_ring_pop(&uart0_tx_ring, &value)) {
            uart0_handle.Instance->DR = value;
            ++rbsp_diag_uart_tx_bytes;
        } else {
            __HAL_UART_DISABLE_IT(&uart0_handle, UART_IT_TXE);
        }
    }
}
#endif
