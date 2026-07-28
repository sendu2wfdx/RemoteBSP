#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remotebsp_config.h"

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

typedef struct {
    bool used;
    uint32_t object_id;
    uint8_t port;
} rbsp_uart_object_t;

typedef struct {
    rbsp_hal_t hal;
    rbsp_can_mode_t can_mode;
    rbsp_node_info_t info;
    uint32_t node_id;
    uint32_t next_object_id;
    uint16_t next_transfer_id;
    uint8_t cache_cursor;
    uint32_t last_heartbeat_ms;
    rbsp_reassembly_slot_t reassembly[CONFIG_REMOTE_REASSEMBLY_SLOTS];
    rbsp_request_cache_entry_t cache[CONFIG_REMOTE_REQUEST_CACHE_ENTRIES];
    rbsp_gpio_object_t gpio_objects[CONFIG_GPIO_RESOURCE_COUNT];
#if CONFIG_UART_RESOURCE_COUNT > 0
    rbsp_uart_object_t uart_objects[CONFIG_UART_RESOURCE_COUNT];
#endif
    uint8_t tx_packet[CONFIG_REMOTE_MAX_PACKET_SIZE];
} rbsp_core_t;

bool rbsp_core_init(rbsp_core_t* core, const rbsp_hal_t* hal,
                    rbsp_can_mode_t mode, const rbsp_node_info_t* info);
void rbsp_core_poll(rbsp_core_t* core);
void rbsp_core_accept_can(rbsp_core_t* core,
                          const rbsp_can_frame_t* frame);

#ifdef __cplusplus
}
#endif
