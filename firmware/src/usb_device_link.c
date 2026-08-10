#include "remotebsp_embedded/usb_device_link.h"

#include "remotebsp_embedded/usb_link.h"
#include "remotebsp_config.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"

#include <stddef.h>
#include <string.h>

#if !defined(CONFIG_REMOTEBSP_TRANSPORT_USB)
#error "usb_device_link.c 只能用于 USB 主通信链路"
#endif
#if !defined(STM32G431xx)
#error "当前 USB Device 板级后端只完成了 STM32G431CBU6 适配"
#endif

#define RBSP_USB_IN_EP 0x81U
#define RBSP_USB_OUT_EP 0x01U
#define RBSP_USB_PACKET_SIZE 64U
#define RBSP_USB_CONFIG_DESCRIPTOR_SIZE 32U
#define RBSP_USB_SERIAL_DESCRIPTOR_SIZE 26U

static PCD_HandleTypeDef pcd_handle;
static USBD_HandleTypeDef usb_device;
static uint32_t class_memory;
static bool class_memory_used;
static bool usb_initialized;
static volatile bool tx_busy;
static volatile uint32_t dropped_bytes;

static uint8_t rx_packet[RBSP_USB_PACKET_SIZE];
static uint8_t tx_packet[RBSP_USB_PACKET_SIZE];
static uint8_t rx_ring[CONFIG_USB_RX_BUFFER_SIZE];
static uint8_t tx_ring[CONFIG_USB_TX_BUFFER_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static uint16_t tx_head;
static uint16_t tx_tail;
static rbsp_usb_decoder_t decoder;

static uint8_t descriptor_buffer[USBD_MAX_STR_DESC_SIZ];
static uint8_t serial_descriptor[RBSP_USB_SERIAL_DESCRIPTOR_SIZE] = {
    RBSP_USB_SERIAL_DESCRIPTOR_SIZE, USB_DESC_TYPE_STRING,
};

static uint8_t device_descriptor[USB_LEN_DEV_DESC] = {
    USB_LEN_DEV_DESC, USB_DESC_TYPE_DEVICE,
    0x00U, 0x02U, 0x00U, 0x00U, 0x00U, USB_MAX_EP0_SIZE,
    (uint8_t)(CONFIG_USB_VENDOR_ID & 0xFFU),
    (uint8_t)((CONFIG_USB_VENDOR_ID >> 8U) & 0xFFU),
    (uint8_t)(CONFIG_USB_PRODUCT_ID & 0xFFU),
    (uint8_t)((CONFIG_USB_PRODUCT_ID >> 8U) & 0xFFU),
    0x00U, 0x01U, USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR, USBD_MAX_NUM_CONFIGURATION,
};

static uint8_t language_descriptor[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC, USB_DESC_TYPE_STRING, 0x09U, 0x04U,
};

__ALIGN_BEGIN static uint8_t configuration_descriptor[
    RBSP_USB_CONFIG_DESCRIPTOR_SIZE] __ALIGN_END = {
    0x09U, USB_DESC_TYPE_CONFIGURATION,
    RBSP_USB_CONFIG_DESCRIPTOR_SIZE, 0x00U,
    0x01U, 0x01U, 0x00U, 0x80U, USBD_MAX_POWER,
    0x09U, USB_DESC_TYPE_INTERFACE,
    0x00U, 0x00U, 0x02U, 0xFFU, 0x00U, 0x00U, 0x00U,
    0x07U, USB_DESC_TYPE_ENDPOINT, RBSP_USB_IN_EP,
    USBD_EP_TYPE_BULK, RBSP_USB_PACKET_SIZE, 0x00U, 0x00U,
    0x07U, USB_DESC_TYPE_ENDPOINT, RBSP_USB_OUT_EP,
    USBD_EP_TYPE_BULK, RBSP_USB_PACKET_SIZE, 0x00U, 0x00U,
};

__ALIGN_BEGIN static uint8_t device_qualifier_descriptor[
    USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
    USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER,
    0x00U, 0x02U, 0x00U, 0x00U, 0x00U, USB_MAX_EP0_SIZE,
    0x01U, 0x00U,
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
    return make_string_descriptor("RemoteBSP Vendor Bulk", length);
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

static uint8_t* get_configuration_string(USBD_SpeedTypeDef speed,
                                         uint16_t* length) {
    (void)speed;
    return make_string_descriptor("RemoteBSP Link", length);
}

static uint8_t* get_interface_string(USBD_SpeedTypeDef speed,
                                     uint16_t* length) {
    (void)speed;
    return make_string_descriptor("RemoteBSP Vendor Bulk", length);
}

static USBD_DescriptorsTypeDef descriptors = {
    get_device_descriptor,
    get_language_descriptor,
    get_manufacturer_descriptor,
    get_product_descriptor,
    get_serial_descriptor,
    get_configuration_string,
    get_interface_string,
};

static uint8_t class_init(USBD_HandleTypeDef* device,
                          uint8_t configuration) {
    (void)configuration;
    if (USBD_LL_OpenEP(device, RBSP_USB_IN_EP, USBD_EP_TYPE_BULK,
                       RBSP_USB_PACKET_SIZE) != USBD_OK ||
        USBD_LL_OpenEP(device, RBSP_USB_OUT_EP, USBD_EP_TYPE_BULK,
                       RBSP_USB_PACKET_SIZE) != USBD_OK) {
        return (uint8_t)USBD_FAIL;
    }
    device->ep_in[RBSP_USB_IN_EP & 0x0FU].is_used = 1U;
    device->ep_out[RBSP_USB_OUT_EP & 0x0FU].is_used = 1U;
    device->pClassData = &class_memory;
    tx_busy = false;
    return (uint8_t)USBD_LL_PrepareReceive(
        device, RBSP_USB_OUT_EP, rx_packet, sizeof(rx_packet));
}

static uint8_t class_deinit(USBD_HandleTypeDef* device,
                            uint8_t configuration) {
    (void)configuration;
    (void)USBD_LL_CloseEP(device, RBSP_USB_IN_EP);
    (void)USBD_LL_CloseEP(device, RBSP_USB_OUT_EP);
    device->ep_in[RBSP_USB_IN_EP & 0x0FU].is_used = 0U;
    device->ep_out[RBSP_USB_OUT_EP & 0x0FU].is_used = 0U;
    device->pClassData = NULL;
    tx_busy = false;
    return (uint8_t)USBD_OK;
}

static uint8_t class_setup(USBD_HandleTypeDef* device,
                           USBD_SetupReqTypedef* request) {
    static uint16_t status;
    static uint8_t alternate_setting;
    if ((request->bmRequest & USB_REQ_TYPE_MASK) !=
        USB_REQ_TYPE_STANDARD) {
        USBD_CtlError(device, request);
        return (uint8_t)USBD_FAIL;
    }
    switch (request->bRequest) {
        case USB_REQ_GET_STATUS:
            status = 0U;
            (void)USBD_CtlSendData(device, (uint8_t*)&status,
                                   sizeof(status));
            return (uint8_t)USBD_OK;
        case USB_REQ_GET_INTERFACE:
            alternate_setting = 0U;
            (void)USBD_CtlSendData(device, &alternate_setting, 1U);
            return (uint8_t)USBD_OK;
        case USB_REQ_SET_INTERFACE:
        case USB_REQ_CLEAR_FEATURE:
            return (uint8_t)USBD_OK;
        default:
            USBD_CtlError(device, request);
            return (uint8_t)USBD_FAIL;
    }
}

static uint8_t class_data_in(USBD_HandleTypeDef* device,
                             uint8_t endpoint) {
    (void)device;
    if (endpoint == (RBSP_USB_IN_EP & 0x0FU)) {
        tx_busy = false;
    }
    return (uint8_t)USBD_OK;
}

static uint8_t class_data_out(USBD_HandleTypeDef* device,
                              uint8_t endpoint) {
    if (endpoint != (RBSP_USB_OUT_EP & 0x0FU)) {
        return (uint8_t)USBD_FAIL;
    }
    const uint32_t length =
        USBD_LL_GetRxDataSize(device, RBSP_USB_OUT_EP);
    for (uint32_t index = 0U; index < length; ++index) {
        const uint16_t next = (uint16_t)(
            (rx_head + 1U) % CONFIG_USB_RX_BUFFER_SIZE);
        if (next == rx_tail) {
            ++dropped_bytes;
            break;
        }
        rx_ring[rx_head] = rx_packet[index];
        rx_head = next;
    }
    return (uint8_t)USBD_LL_PrepareReceive(
        device, RBSP_USB_OUT_EP, rx_packet, sizeof(rx_packet));
}

static uint8_t* get_configuration_descriptor(uint16_t* length) {
    *length = sizeof(configuration_descriptor);
    return configuration_descriptor;
}

static uint8_t* get_device_qualifier_descriptor(uint16_t* length) {
    *length = sizeof(device_qualifier_descriptor);
    return device_qualifier_descriptor;
}

static USBD_ClassTypeDef vendor_bulk_class = {
    class_init, class_deinit, class_setup,
    NULL, NULL, class_data_in, class_data_out,
    NULL, NULL, NULL,
    get_configuration_descriptor,
    get_configuration_descriptor,
    get_configuration_descriptor,
    get_device_qualifier_descriptor,
};

void* USBD_static_malloc(uint32_t size) {
    if (class_memory_used || size > sizeof(class_memory)) {
        return NULL;
    }
    class_memory_used = true;
    return &class_memory;
}

void USBD_static_free(void* memory) {
    if (memory == &class_memory) {
        class_memory_used = false;
    }
}

static USBD_StatusTypeDef hal_status(HAL_StatusTypeDef status_value) {
    return status_value == HAL_OK ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef* device) {
    memset(&pcd_handle, 0, sizeof(pcd_handle));
    pcd_handle.Instance = USB;
    pcd_handle.Init.dev_endpoints = 4U;
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
    if (HAL_PCDEx_PMAConfig(&pcd_handle, 0x00U, PCD_SNG_BUF,
                            0x40U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, 0x80U, PCD_SNG_BUF,
                            0x80U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, RBSP_USB_OUT_EP,
                            PCD_SNG_BUF, 0xC0U) != HAL_OK ||
        HAL_PCDEx_PMAConfig(&pcd_handle, RBSP_USB_IN_EP,
                            PCD_SNG_BUF, 0x100U) != HAL_OK) {
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
    return hal_status(HAL_PCD_EP_ClrStall(&pcd_handle, address));
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef* device,
                          uint8_t address) {
    (void)device;
    return (address & 0x80U) != 0U
               ? pcd_handle.IN_ep[address & 0x7FU].is_stall
               : pcd_handle.OUT_ep[address & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef* device,
                                         uint8_t address) {
    (void)device;
    return hal_status(HAL_PCD_SetAddress(&pcd_handle, address));
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef* device,
                                    uint8_t address, uint8_t* data,
                                    uint32_t size) {
    (void)device;
    return hal_status(HAL_PCD_EP_Transmit(
        &pcd_handle, address, data, size));
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef* device,
                                          uint8_t address,
                                          uint8_t* data,
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

void USBD_LL_Delay(uint32_t delay) { HAL_Delay(delay); }

void HAL_PCD_MspInit(PCD_HandleTypeDef* handle) {
    if (handle->Instance != USB) {
        return;
    }
    __HAL_RCC_USB_CLK_ENABLE();
    HAL_NVIC_SetPriority(USB_LP_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
    HAL_NVIC_SetPriority(USB_HP_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(USB_HP_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* handle) {
    if (handle->Instance != USB) {
        return;
    }
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);
    HAL_NVIC_DisableIRQ(USB_HP_IRQn);
    __HAL_RCC_USB_CLK_DISABLE();
}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_SetupStage((USBD_HandleTypeDef*)handle->pData,
                             (uint8_t*)handle->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef* handle,
                                  uint8_t endpoint) {
    (void)USBD_LL_DataOutStage((USBD_HandleTypeDef*)handle->pData,
                               endpoint,
                               handle->OUT_ep[endpoint].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef* handle,
                                 uint8_t endpoint) {
    (void)USBD_LL_DataInStage((USBD_HandleTypeDef*)handle->pData,
                              endpoint,
                              handle->IN_ep[endpoint].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_SOF((USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef* handle) {
    USBD_HandleTypeDef* device = (USBD_HandleTypeDef*)handle->pData;
    (void)USBD_LL_SetSpeed(device, USBD_SPEED_FULL);
    (void)USBD_LL_Reset(device);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_Suspend((USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_Resume((USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef* handle,
                                      uint8_t endpoint) {
    (void)USBD_LL_IsoOUTIncomplete(
        (USBD_HandleTypeDef*)handle->pData, endpoint);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef* handle,
                                     uint8_t endpoint) {
    (void)USBD_LL_IsoINIncomplete(
        (USBD_HandleTypeDef*)handle->pData, endpoint);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_DevConnected((USBD_HandleTypeDef*)handle->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef* handle) {
    (void)USBD_LL_DevDisconnected(
        (USBD_HandleTypeDef*)handle->pData);
}

bool rbsp_usb_device_link_init(void) {
    rx_head = 0U;
    rx_tail = 0U;
    tx_head = 0U;
    tx_tail = 0U;
    tx_busy = false;
    dropped_bytes = 0U;
    class_memory_used = false;
    rbsp_usb_decoder_init(&decoder);
    if (USBD_Init(&usb_device, &descriptors, 0U) != USBD_OK ||
        USBD_RegisterClass(&usb_device, &vendor_bulk_class) != USBD_OK) {
        usb_initialized = false;
        return false;
    }
    usb_initialized = true;
    if (USBD_Start(&usb_device) != USBD_OK) {
        usb_initialized = false;
        return false;
    }
    return true;
}

bool rbsp_usb_device_link_configured(void) {
    return usb_initialized &&
           usb_device.dev_state == USBD_STATE_CONFIGURED;
}

bool rbsp_usb_device_link_send(const rbsp_link_frame_t* frame) {
    uint8_t encoded[RBSP_USB_MAX_WIRE_FRAME_SIZE];
    const size_t length = rbsp_usb_encode_frame(
        frame, encoded, sizeof(encoded));
    if (length == 0U || !rbsp_usb_device_link_configured()) {
        return false;
    }
    uint16_t free_bytes;
    if (tx_head >= tx_tail) {
        free_bytes = (uint16_t)(CONFIG_USB_TX_BUFFER_SIZE -
                                (tx_head - tx_tail) - 1U);
    } else {
        free_bytes = (uint16_t)(tx_tail - tx_head - 1U);
    }
    if (length > free_bytes) {
        dropped_bytes += (uint32_t)length;
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        tx_ring[tx_head] = encoded[index];
        tx_head = (uint16_t)((tx_head + 1U) %
                             CONFIG_USB_TX_BUFFER_SIZE);
    }
    return true;
}

bool rbsp_usb_device_link_receive(rbsp_link_frame_t* frame) {
    if (frame == NULL) {
        return false;
    }
    while (rx_tail != rx_head) {
        const uint8_t byte = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) %
                             CONFIG_USB_RX_BUFFER_SIZE);
        if (!rbsp_usb_decoder_append(&decoder, &byte, 1U)) {
            rbsp_usb_decoder_init(&decoder);
            ++dropped_bytes;
        }
        const rbsp_usb_decode_result_t result =
            rbsp_usb_decoder_pop(&decoder, frame);
        if (result == RBSP_USB_DECODE_FRAME) {
            return true;
        }
        if (result == RBSP_USB_DECODE_INVALID) {
            ++dropped_bytes;
        }
    }
    return false;
}

void rbsp_usb_device_link_poll(void) {
    if (!rbsp_usb_device_link_configured() || tx_busy ||
        tx_tail == tx_head) {
        return;
    }
    uint32_t length = 0U;
    while (tx_tail != tx_head && length < sizeof(tx_packet)) {
        tx_packet[length++] = tx_ring[tx_tail];
        tx_tail = (uint16_t)((tx_tail + 1U) %
                             CONFIG_USB_TX_BUFFER_SIZE);
    }
    tx_busy = true;
    if (USBD_LL_Transmit(&usb_device, RBSP_USB_IN_EP,
                         tx_packet, length) != USBD_OK) {
        tx_busy = false;
        dropped_bytes += length;
    }
}

uint32_t rbsp_usb_device_link_dropped_bytes(void) {
    return dropped_bytes;
}

void rbsp_usb_device_link_irq_handler(void) {
    if (usb_initialized) {
        HAL_PCD_IRQHandler(&pcd_handle);
    }
}
