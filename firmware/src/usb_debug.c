#include "remotebsp_embedded/usb_debug.h"

#include "remotebsp_config.h"
#include "usbd_cdc.h"
#include "usbd_core.h"

#include <string.h>

#if !defined(CONFIG_USB_DEBUG_CDC)
#error "usb_debug.c 只能在启用 CONFIG_USB_DEBUG_CDC 时编译"
#endif

#if !defined(CONFIG_CAN_PINS_PB8_PB9)
#error "USB CDC 使用 PA11/PA12，CAN 必须改用 PB8/PB9"
#endif

#define USB_DEBUG_PACKET_SIZE 64U
#define USB_SERIAL_DESCRIPTOR_SIZE 26U

static PCD_HandleTypeDef pcd_handle;
static USBD_HandleTypeDef usb_device;
static USBD_CDC_HandleTypeDef cdc_class_memory;
static bool cdc_class_memory_used;

static uint8_t rx_packet[USB_DEBUG_PACKET_SIZE];
static uint8_t tx_packet[USB_DEBUG_PACKET_SIZE];
static uint8_t tx_ring[CONFIG_USB_DEBUG_TX_BUFFER_SIZE];
static uint16_t tx_head;
static uint16_t tx_tail;
static volatile bool tx_busy;
static bool usb_initialized;
static volatile uint32_t dropped_bytes;

static uint8_t descriptor_buffer[USBD_MAX_STR_DESC_SIZ];
static uint8_t serial_descriptor[USB_SERIAL_DESCRIPTOR_SIZE] = {
    USB_SERIAL_DESCRIPTOR_SIZE, USB_DESC_TYPE_STRING,
};

static uint8_t device_descriptor[USB_LEN_DEV_DESC] = {
    USB_LEN_DEV_DESC,
    USB_DESC_TYPE_DEVICE,
    0x00U,
    0x02U,
    0x02U,
    0x00U,
    0x00U,
    USB_MAX_EP0_SIZE,
    (uint8_t)(CONFIG_USB_DEBUG_VID & 0xFFU),
    (uint8_t)((CONFIG_USB_DEBUG_VID >> 8U) & 0xFFU),
    (uint8_t)(CONFIG_USB_DEBUG_PID & 0xFFU),
    (uint8_t)((CONFIG_USB_DEBUG_PID >> 8U) & 0xFFU),
    0x00U,
    0x01U,
    USBD_IDX_MFC_STR,
    USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION,
};

static uint8_t language_descriptor[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    0x09U,
    0x04U,
};

static void integer_to_unicode(uint32_t value, uint8_t* output,
                               uint8_t digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint8_t index = 0U; index < digits; ++index) {
        output[index * 2U] =
            (uint8_t)hex[(value >> 28U) & 0x0FU];
        output[index * 2U + 1U] = 0U;
        value <<= 4U;
    }
}

static uint8_t* get_device_descriptor(USBD_SpeedTypeDef speed,
                                      uint16_t* length) {
    (void)speed;
    *length = sizeof(device_descriptor);
    return device_descriptor;
}

static uint8_t* get_language_descriptor(USBD_SpeedTypeDef speed,
                                        uint16_t* length) {
    (void)speed;
    *length = sizeof(language_descriptor);
    return language_descriptor;
}

static uint8_t* make_string_descriptor(const char* text,
                                       uint16_t* length) {
    USBD_GetString((uint8_t*)(uintptr_t)text, descriptor_buffer,
                   length);
    return descriptor_buffer;
}

static uint8_t* get_manufacturer_descriptor(USBD_SpeedTypeDef speed,
                                            uint16_t* length) {
    (void)speed;
    return make_string_descriptor("RemoteBSP", length);
}

static uint8_t* get_product_descriptor(USBD_SpeedTypeDef speed,
                                       uint16_t* length) {
    (void)speed;
    return make_string_descriptor("RemoteBSP USB Debug", length);
}

static uint8_t* get_serial_descriptor(USBD_SpeedTypeDef speed,
                                      uint16_t* length) {
    (void)speed;
    const uint32_t first = *(const uint32_t*)(UID_BASE);
    const uint32_t second = *(const uint32_t*)(UID_BASE + 4U);
    const uint32_t third = *(const uint32_t*)(UID_BASE + 8U);
    integer_to_unicode(first + third, &serial_descriptor[2], 8U);
    integer_to_unicode(second, &serial_descriptor[18], 4U);
    *length = sizeof(serial_descriptor);
    return serial_descriptor;
}

static uint8_t* get_configuration_descriptor(
    USBD_SpeedTypeDef speed, uint16_t* length) {
    (void)speed;
    return make_string_descriptor("RemoteBSP CDC", length);
}

static uint8_t* get_interface_descriptor(USBD_SpeedTypeDef speed,
                                         uint16_t* length) {
    (void)speed;
    return make_string_descriptor("Debug Console", length);
}

static USBD_DescriptorsTypeDef descriptors = {
    get_device_descriptor,
    get_language_descriptor,
    get_manufacturer_descriptor,
    get_product_descriptor,
    get_serial_descriptor,
    get_configuration_descriptor,
    get_interface_descriptor,
};

static int8_t cdc_initialize(void) {
    USBD_CDC_SetTxBuffer(&usb_device, tx_packet, 0U);
    USBD_CDC_SetRxBuffer(&usb_device, rx_packet);
    return USBD_CDC_ReceivePacket(&usb_device) == USBD_OK
               ? (int8_t)USBD_OK
               : (int8_t)USBD_FAIL;
}

static int8_t cdc_deinitialize(void) {
    tx_busy = false;
    return (int8_t)USBD_OK;
}

static int8_t cdc_control(uint8_t command, uint8_t* data,
                          uint16_t length) {
    (void)command;
    (void)data;
    (void)length;
    return (int8_t)USBD_OK;
}

static int8_t cdc_receive(uint8_t* data, uint32_t* length) {
    (void)data;
    (void)length;
    USBD_CDC_SetRxBuffer(&usb_device, rx_packet);
    return USBD_CDC_ReceivePacket(&usb_device) == USBD_OK
               ? (int8_t)USBD_OK
               : (int8_t)USBD_FAIL;
}

static int8_t cdc_transmit_complete(uint8_t* data, uint32_t* length,
                                    uint8_t endpoint) {
    (void)data;
    (void)length;
    (void)endpoint;
    tx_busy = false;
    return (int8_t)USBD_OK;
}

static USBD_CDC_ItfTypeDef cdc_interface = {
    cdc_initialize,
    cdc_deinitialize,
    cdc_control,
    cdc_receive,
    cdc_transmit_complete,
};

void* USBD_static_malloc(uint32_t size) {
    if (cdc_class_memory_used ||
        size > sizeof(cdc_class_memory)) {
        return NULL;
    }
    cdc_class_memory_used = true;
    return &cdc_class_memory;
}

void USBD_static_free(void* memory) {
    if (memory == &cdc_class_memory) {
        cdc_class_memory_used = false;
    }
}

static USBD_StatusTypeDef hal_status(HAL_StatusTypeDef status) {
    return status == HAL_OK ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef* device) {
    memset(&pcd_handle, 0, sizeof(pcd_handle));
    pcd_handle.Instance = USB;
    pcd_handle.Init.dev_endpoints = 8U;
    pcd_handle.Init.speed = PCD_SPEED_FULL;
    pcd_handle.Init.phy_itface = PCD_PHY_EMBEDDED;
    pcd_handle.Init.low_power_enable = DISABLE;
    pcd_handle.Init.lpm_enable = DISABLE;
    pcd_handle.Init.battery_charging_enable = DISABLE;
    pcd_handle.pData = device;
    device->pData = &pcd_handle;

    if (HAL_PCD_Init(&pcd_handle) != HAL_OK) {
        return USBD_FAIL;
    }
    /*
     * F103 USB PMA 的 BTABLE 为 8 个端点预留 0x40 字节。
     * EP0 缓冲区必须放在 BTABLE 之后，否则设备描述符传输会覆盖
     * 端点描述符，Windows 会报告“设备描述符请求失败”。
     */
    if (HAL_PCDEx_PMAConfig(&pcd_handle, 0x00U, PCD_SNG_BUF,
                            0x40U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, 0x80U, PCD_SNG_BUF,
                            0x80U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, CDC_IN_EP, PCD_SNG_BUF,
                            0xC0U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, CDC_CMD_EP, PCD_SNG_BUF,
                            0x100U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, CDC_OUT_EP, PCD_SNG_BUF,
                            0x110U) != HAL_OK) {
        return USBD_FAIL;
    }
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef* device) {
    (void)device;
    return hal_status(HAL_PCD_DeInit(&pcd_handle));
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef* device) {
    (void)device;
    return hal_status(HAL_PCD_Start(&pcd_handle));
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef* device) {
    (void)device;
    return hal_status(HAL_PCD_Stop(&pcd_handle));
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef* device,
                                  uint8_t address, uint8_t type,
                                  uint16_t max_packet) {
    (void)device;
    return hal_status(HAL_PCD_EP_Open(
        &pcd_handle, address, max_packet, type));
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef* device,
                                   uint8_t address) {
    (void)device;
    return hal_status(HAL_PCD_EP_Close(&pcd_handle, address));
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef* device,
                                   uint8_t address) {
    (void)device;
    return hal_status(HAL_PCD_EP_Flush(&pcd_handle, address));
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef* device,
                                   uint8_t address) {
    (void)device;
    return hal_status(HAL_PCD_EP_SetStall(&pcd_handle, address));
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef* device,
                                        uint8_t address) {
    (void)device;
    return hal_status(
        HAL_PCD_EP_ClrStall(&pcd_handle, address));
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef* device,
                          uint8_t address) {
    (void)device;
    if ((address & 0x80U) != 0U) {
        return pcd_handle.IN_ep[address & 0x7FU].is_stall;
    }
    return pcd_handle.OUT_ep[address & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(
    USBD_HandleTypeDef* device, uint8_t address) {
    (void)device;
    return hal_status(
        HAL_PCD_SetAddress(&pcd_handle, address));
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef* device,
                                    uint8_t address, uint8_t* data,
    uint32_t size) {
    (void)device;
    return hal_status(HAL_PCD_EP_Transmit(
        &pcd_handle, address, data, size));
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(
    USBD_HandleTypeDef* device, uint8_t address, uint8_t* data,
    uint32_t size) {
    (void)device;
    return hal_status(HAL_PCD_EP_Receive(
        &pcd_handle, address, data, size));
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef* device,
                               uint8_t address) {
    (void)device;
    return HAL_PCD_EP_GetRxCount(&pcd_handle, address);
}

void USBD_LL_Delay(uint32_t delay) {
    HAL_Delay(delay);
}

void HAL_PCD_MspInit(PCD_HandleTypeDef* handle) {
    if (handle->Instance != USB) {
        return;
    }
    __HAL_RCC_USB_CLK_ENABLE();
#if defined(STM32F103xB)
    HAL_NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    HAL_NVIC_SetPriority(USB_HP_CAN1_TX_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_HP_CAN1_TX_IRQn);
#else
    HAL_NVIC_SetPriority(USB_LP_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
    HAL_NVIC_SetPriority(USB_HP_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_HP_IRQn);
#endif
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* handle) {
    if (handle->Instance != USB) {
        return;
    }
#if defined(STM32F103xB)
    HAL_NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    HAL_NVIC_DisableIRQ(USB_HP_CAN1_TX_IRQn);
#else
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);
    HAL_NVIC_DisableIRQ(USB_HP_IRQn);
#endif
    __HAL_RCC_USB_CLK_DISABLE();
}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_SetupStage(
        (USBD_HandleTypeDef*)handle->pData,
        (uint8_t*)handle->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef* handle,
                                  uint8_t endpoint) {
    (void)USBD_LL_DataOutStage(
        (USBD_HandleTypeDef*)handle->pData, endpoint,
        handle->OUT_ep[endpoint].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef* handle,
                                 uint8_t endpoint) {
    (void)USBD_LL_DataInStage(
        (USBD_HandleTypeDef*)handle->pData, endpoint,
        handle->IN_ep[endpoint].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_SOF((USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef* handle) {
    USBD_HandleTypeDef* device =
        (USBD_HandleTypeDef*)handle->pData;
    (void)USBD_LL_SetSpeed(device, USBD_SPEED_FULL);
    (void)USBD_LL_Reset(device);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_Suspend(
        (USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_Resume(
        (USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(
    PCD_HandleTypeDef* handle, uint8_t endpoint) {
    (void)USBD_LL_IsoOUTIncomplete(
        (USBD_HandleTypeDef*)handle->pData, endpoint);
}

void HAL_PCD_ISOINIncompleteCallback(
    PCD_HandleTypeDef* handle, uint8_t endpoint) {
    (void)USBD_LL_IsoINIncomplete(
        (USBD_HandleTypeDef*)handle->pData, endpoint);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_DevConnected(
        (USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_DevDisconnected(
        (USBD_HandleTypeDef*)handle->pData);
}

bool rbsp_usb_debug_init(void) {
    tx_head = 0U;
    tx_tail = 0U;
    tx_busy = false;
    dropped_bytes = 0U;
    cdc_class_memory_used = false;

#if defined(STM32F103xB)
    /*
     * F103 板卡通常使用外接 D+ 上拉，USB 外设本身不能软件断开。
     * 启动时把 PA12 短暂拉低，确保主机丢弃旧枚举状态后重新读取描述符。
     */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
    GPIO_InitTypeDef disconnect_pin = {0};
    disconnect_pin.Pin = GPIO_PIN_12;
    disconnect_pin.Mode = GPIO_MODE_OUTPUT_PP;
    disconnect_pin.Pull = GPIO_NOPULL;
    disconnect_pin.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &disconnect_pin);
    HAL_Delay(20U);
#endif

    if (USBD_Init(&usb_device, &descriptors, 0U) != USBD_OK) {
        usb_initialized = false;
        return false;
    }
    if (USBD_RegisterClass(&usb_device, USBD_CDC_CLASS) !=
        USBD_OK) {
        usb_initialized = false;
        return false;
    }
    if (USBD_CDC_RegisterInterface(&usb_device, &cdc_interface) !=
        USBD_OK) {
        usb_initialized = false;
        return false;
    }
    usb_initialized = true;
    if (USBD_Start(&usb_device) != USBD_OK) {
        usb_initialized = false;
        return false;
    }
#if defined(STM32F103xB)
    /*
     * 至此协议栈、端点表和中断均已就绪，再释放 PA12 让外部
     * D+ 上拉生效，避免主机在初始化窗口内提前发起枚举。
     */
    GPIO_InitTypeDef usb_pins = {0};
    usb_pins.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    usb_pins.Mode = GPIO_MODE_AF_INPUT;
    usb_pins.Pull = GPIO_NOPULL;
    usb_pins.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &usb_pins);
#endif
    return true;
}

size_t rbsp_usb_debug_write(const void* data, size_t length) {
    if (data == NULL || length == 0U) {
        return 0U;
    }
    const uint8_t* input = (const uint8_t*)data;
    size_t accepted = 0U;
    while (accepted < length) {
        const uint16_t next =
            (uint16_t)((tx_head + 1U) %
                       CONFIG_USB_DEBUG_TX_BUFFER_SIZE);
        if (next == tx_tail) {
            dropped_bytes +=
                (uint32_t)(length - accepted);
            break;
        }
        tx_ring[tx_head] = input[accepted];
        tx_head = next;
        ++accepted;
    }
    return accepted;
}

size_t rbsp_usb_debug_write_text(const char* text) {
    return text == NULL
               ? 0U
               : rbsp_usb_debug_write(text, strlen(text));
}

bool rbsp_usb_debug_configured(void) {
    return usb_initialized &&
           usb_device.dev_state == USBD_STATE_CONFIGURED;
}

void rbsp_usb_debug_poll(void) {
    if (!rbsp_usb_debug_configured() || tx_busy ||
        tx_tail == tx_head) {
        return;
    }

    uint32_t length = 0U;
    uint16_t cursor = tx_tail;
    while (cursor != tx_head &&
           length < sizeof(tx_packet)) {
        tx_packet[length++] = tx_ring[cursor];
        cursor = (uint16_t)(
            (cursor + 1U) % CONFIG_USB_DEBUG_TX_BUFFER_SIZE);
    }

    tx_busy = true;
    USBD_CDC_SetTxBuffer(&usb_device, tx_packet, length);
    if (USBD_CDC_TransmitPacket(&usb_device) == USBD_OK) {
        tx_tail = cursor;
    } else {
        tx_busy = false;
    }
}

uint32_t rbsp_usb_debug_dropped_bytes(void) {
    return dropped_bytes;
}

void rbsp_usb_debug_irq_handler(void) {
    if (usb_initialized) {
        HAL_PCD_IRQHandler(&pcd_handle);
    }
}
