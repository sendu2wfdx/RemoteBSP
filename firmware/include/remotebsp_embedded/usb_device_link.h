#pragma once

#include "remotebsp_embedded/core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* STM32 USB Device Vendor Bulk 板级后端。协议解析仍由 Remote Core 完成。 */
bool rbsp_usb_device_link_init(void);
bool rbsp_usb_device_link_send(const rbsp_link_frame_t* frame);
bool rbsp_usb_device_link_receive(rbsp_link_frame_t* frame);
void rbsp_usb_device_link_poll(void);
bool rbsp_usb_device_link_configured(void);
uint32_t rbsp_usb_device_link_dropped_bytes(void);
void rbsp_usb_device_link_irq_handler(void);

#ifdef __cplusplus
}
#endif
