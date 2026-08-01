#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remotebsp_config.h"
#if defined(CONFIG_REMOTEBSP_MOTION)
#include "remotebsp_embedded/motion.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_PROTOCOL_VERSION 1U
#define RBSP_HEADER_SIZE 24U
#define RBSP_FRAGMENT_HEADER_SIZE 5U
#define RBSP_MAX_WIRE_PACKET_SIZE 2048U
#define RBSP_CAN_ID_DISCOVERY 0x700U
#define RBSP_CAN_ID_REQUEST_BASE 0x600U
#define RBSP_CAN_ID_RESPONSE_BASE 0x580U
#define RBSP_CAN_ID_EVENT_BASE 0x500U
#define RBSP_CAN_ID_PROVISIONAL_BASE 0x480U

#if CONFIG_REMOTE_MAX_PACKET_SIZE > RBSP_MAX_WIRE_PACKET_SIZE
#error "配置的远程包长度超过协议上限"
#endif

#if CONFIG_REMOTE_CACHE_RESPONSE_SIZE <= RBSP_HEADER_SIZE
#error "去重缓存必须至少能够容纳协议头和一个状态字节"
#endif

typedef enum {
    RBSP_CAN_CLASSICAL = 0,
    RBSP_CAN_FD = 1,
} rbsp_can_mode_t;

typedef enum {
    RBSP_GPIO_INPUT = 0,
    RBSP_GPIO_OUTPUT = 1,
} rbsp_gpio_direction_t;

typedef enum {
    RBSP_BOOTLOADER_CAN = 0,
    RBSP_BOOTLOADER_USB = 1,
} rbsp_bootloader_mode_t;

typedef struct {
    uint32_t identifier;
    uint8_t length;
    uint8_t data[64];
} rbsp_can_frame_t;

typedef struct {
    uint8_t uuid[16];
    uint16_t firmware_major;
    uint16_t firmware_minor;
    uint16_t firmware_patch;
    uint32_t board_type;
} rbsp_node_info_t;

/*
 * 这是远程核心与具体 MCU 驱动之间唯一的边界。
 * 中断服务只负责收发字节和维护驱动状态，协议解析始终在主循环中完成。
 */
typedef struct {
    bool (*can_send)(const rbsp_can_frame_t* frame);
    uint32_t (*milliseconds)(void);
    bool (*gpio_configure)(uint16_t pin, rbsp_gpio_direction_t direction,
                           bool initial_value);
    bool (*gpio_write)(uint16_t pin, bool value);
    bool (*gpio_read)(uint16_t pin, bool* value);
    bool (*uart_configure)(uint8_t port, uint32_t baud_rate,
                           uint8_t data_bits, uint8_t stop_bits,
                           uint8_t parity);
    size_t (*uart_read)(uint8_t port, uint8_t* data, size_t capacity);
    bool (*uart_write)(uint8_t port, const uint8_t* data, size_t length);
#if defined(CONFIG_REMOTEBSP_MOTION)
    uint64_t (*nanoseconds)(void);
    uint8_t motion_axis_count;
    bool (*motion_set_enable)(uint8_t axis, bool enabled);
    bool (*motion_set_direction)(uint8_t axis, bool positive);
    bool (*motion_set_step)(uint8_t axis, bool high);
    bool (*motion_limit_active)(uint8_t axis, bool* active);
#endif
    void (*enter_bootloader)(rbsp_bootloader_mode_t mode);
} rbsp_hal_t;

typedef struct {
    bool active;
    uint32_t stream_id;
    uint16_t transfer_id;
    uint16_t next_sequence;
    uint16_t size;
    uint32_t last_update_ms;
    uint8_t packet[CONFIG_REMOTE_MAX_PACKET_SIZE];
} rbsp_reassembly_slot_t;

typedef struct {
    bool valid;
    uint32_t session_id;
    uint32_t request_id;
    uint16_t command;
    uint32_t object_id;
    uint16_t flags;
    uint32_t request_crc;
    uint16_t response_size;
    uint8_t response[CONFIG_REMOTE_CACHE_RESPONSE_SIZE];
} rbsp_request_cache_entry_t;

typedef struct {
    bool used;
    uint32_t object_id;
    uint16_t pin;
    rbsp_gpio_direction_t direction;
} rbsp_gpio_object_t;

#if CONFIG_UART_RESOURCE_COUNT > 0
typedef struct {
    bool used;
    uint32_t object_id;
    uint8_t port;
    bool streaming;
    uint16_t pending_length;
    uint32_t first_byte_ms;
    uint32_t event_sequence;
    uint8_t pending[CONFIG_UART_EVENT_CHUNK_SIZE];
} rbsp_uart_object_t;
#endif

typedef struct {
    rbsp_hal_t hal;
    rbsp_can_mode_t can_mode;
    rbsp_node_info_t info;
    uint32_t node_id;
    uint32_t next_object_id;
    uint16_t next_transfer_id;
    uint8_t cache_cursor;
    uint32_t last_heartbeat_ms;
    uint32_t bootloader_request_ms;
    rbsp_bootloader_mode_t bootloader_request_mode;
    bool bootloader_request_pending;
    rbsp_reassembly_slot_t reassembly[CONFIG_REMOTE_REASSEMBLY_SLOTS];
    rbsp_request_cache_entry_t cache[CONFIG_REMOTE_REQUEST_CACHE_ENTRIES];
    rbsp_gpio_object_t gpio_objects[CONFIG_GPIO_RESOURCE_COUNT];
#if CONFIG_UART_RESOURCE_COUNT > 0
    rbsp_uart_object_t uart_objects[CONFIG_UART_RESOURCE_COUNT];
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    rbsp_motion_queue_t motion;
#endif
    uint8_t tx_packet[CONFIG_REMOTE_MAX_PACKET_SIZE];
} rbsp_core_t;

bool rbsp_core_init(rbsp_core_t* core, const rbsp_hal_t* hal,
                    rbsp_can_mode_t mode, const rbsp_node_info_t* info);
void rbsp_core_poll(rbsp_core_t* core);
void rbsp_core_accept_can(rbsp_core_t* core,
                          const rbsp_can_frame_t* frame);
#if defined(CONFIG_REMOTEBSP_MOTION)
bool rbsp_core_motion_tick(rbsp_core_t* core);
#endif

#ifdef __cplusplus
}
#endif
