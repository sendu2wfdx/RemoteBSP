#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB 调试是旁路功能：初始化失败、主机未连接或发送拥塞都不能阻塞
 * RemoteBSP 主循环。write/poll 应从主循环上下文调用。
 */
bool rbsp_usb_debug_init(void);
size_t rbsp_usb_debug_write(const void* data, size_t length);
size_t rbsp_usb_debug_write_text(const char* text);
void rbsp_usb_debug_poll(void);
uint32_t rbsp_usb_debug_dropped_bytes(void);
bool rbsp_usb_debug_configured(void);
void rbsp_usb_debug_irq_handler(void);

#ifdef __cplusplus
}
#endif
