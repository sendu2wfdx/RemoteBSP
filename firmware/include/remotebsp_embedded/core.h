#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "remotebsp_config.h"
#ifndef CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY
#define CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY 2
#endif
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
#include "remotebsp/device_params/store.h"
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
#include "remotebsp_embedded/motion.h"
#include "remotebsp_embedded/motion_group.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_PROTOCOL_VERSION 1U
#define RBSP_HEADER_SIZE 24U
#define RBSP_FRAGMENT_HEADER_SIZE 5U
#define RBSP_MAX_WIRE_PACKET_SIZE 2048U
#define RBSP_ROUTE_DISCOVERY 0x700U
#define RBSP_ROUTE_REQUEST_BASE 0x600U
#define RBSP_ROUTE_RESPONSE_BASE 0x580U
#define RBSP_ROUTE_EVENT_BASE 0x500U
#define RBSP_ROUTE_PROVISIONAL_BASE 0x480U

/* 兼容现有 CAN 板级代码；Remote Core 只解释逻辑路由，不解释 CAN ID。 */
#define RBSP_CAN_ID_DISCOVERY RBSP_ROUTE_DISCOVERY
#define RBSP_CAN_ID_REQUEST_BASE RBSP_ROUTE_REQUEST_BASE
#define RBSP_CAN_ID_RESPONSE_BASE RBSP_ROUTE_RESPONSE_BASE
#define RBSP_CAN_ID_EVENT_BASE RBSP_ROUTE_EVENT_BASE
#define RBSP_CAN_ID_PROVISIONAL_BASE RBSP_ROUTE_PROVISIONAL_BASE

#if CONFIG_REMOTE_MAX_PACKET_SIZE > RBSP_MAX_WIRE_PACKET_SIZE
#error "配置的远程包长度超过协议上限"
#endif

#if CONFIG_REMOTE_CACHE_RESPONSE_SIZE <= RBSP_HEADER_SIZE
#error "去重缓存必须至少能够容纳协议头和一个状态字节"
#endif


typedef enum {
    RBSP_CAN_CLASSICAL = 0,
    RBSP_CAN_FD = 1,
    RBSP_USB = 2,
} rbsp_link_mode_t;

typedef rbsp_link_mode_t rbsp_can_mode_t;

#if defined(CONFIG_REMOTEBSP_MOTION)
typedef struct {
    bool active;
    uint64_t lease_id;
    uint32_t owner_session_id;
    uint32_t granted_duration_ms;
    uint32_t expires_at_ms;
} rbsp_stepgen_lease_t;
#endif

typedef enum {
    RBSP_GPIO_INPUT = 0,
    RBSP_GPIO_OUTPUT = 1,
} rbsp_gpio_direction_t;

typedef enum {
    RBSP_GPIO_FLOATING = 0,
    RBSP_GPIO_PULL_UP = 1,
    RBSP_GPIO_PULL_DOWN = 2,
} rbsp_gpio_pull_t;

#if defined(CONFIG_REMOTEBSP_BUS)
typedef enum {
    RBSP_BUS_I2C_BUS = 1,
    RBSP_BUS_I2C_DEVICE = 2,
    RBSP_BUS_SPI_BUS = 3,
    RBSP_BUS_SPI_DEVICE = 4,
} rbsp_bus_resource_kind_t;

typedef enum {
    RBSP_BUS_TRANSACTION_OK = 0,
    RBSP_BUS_TRANSACTION_NACK = 1,
    RBSP_BUS_TRANSACTION_TIMEOUT = 2,
    RBSP_BUS_TRANSACTION_BUSY = 3,
    RBSP_BUS_TRANSACTION_FAULT = 4,
    RBSP_BUS_TRANSACTION_LIMIT_EXCEEDED = 5,
} rbsp_bus_transaction_status_t;

enum {
    RBSP_BUS_CONTRACT_I2C_REPEATED_START = 1U << 0,
    RBSP_BUS_CONTRACT_I2C_RECOVERY = 1U << 1,
    RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX = 1U << 2,
    RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT = 1U << 3,
    /* SPI 事务标志与合同能力位使用不同的线协议位值。 */
    RBSP_BUS_SPI_TRANSFER_KEEP_CHIP_SELECT = 1U << 0,
};

/*
 * 此表必须由生成配置或经过板级审核的只读板卡代码提供。device_value 对 I2C
 * 是 7-bit 地址，对 SPI 是板级片选符号；Remote Core 不解释 GPIO/AF。
 */
typedef struct {
    uint32_t resource_id;
    uint32_t parent_bus_resource_id;
    uint32_t maximum_clock_hz;
    uint32_t minimum_timeout_us;
    uint32_t maximum_timeout_us;
    uint32_t maximum_operations_per_second;
    uint16_t maximum_transfer_bytes;
    uint16_t queue_capacity;
    uint16_t device_value;
    uint8_t controller;
    uint8_t kind;
    uint8_t flags;
    uint8_t spi_mode;
    uint8_t bits_per_word;
} rbsp_bus_resource_config_t;

typedef struct {
    bool active;
    uint64_t lease_id;
    uint32_t owner_session_id;
    uint32_t granted_duration_ms;
    uint32_t expires_at_ms;
} rbsp_bus_lease_t;
#endif

typedef enum {
    RBSP_BOOTLOADER_CAN = 0,
    RBSP_BOOTLOADER_USB = 1,
} rbsp_bootloader_mode_t;

typedef struct {
    union {
        uint32_t route;
        uint32_t identifier;
    };
    uint8_t length;
    uint8_t data[64];
} rbsp_link_frame_t;

typedef rbsp_link_frame_t rbsp_can_frame_t;

typedef struct {
    uint8_t uuid[16];
    uint16_t firmware_major;
    uint16_t firmware_minor;
    uint16_t firmware_patch;
    uint32_t board_type;
    uint16_t firmware_identity_available_fields;
    uint8_t project_sha256[32];
    uint8_t config_sha256[32];
    uint8_t firmware_input_sha256[32];
} rbsp_node_info_t;

/* 板级驱动可选上报的瞬时状态；false 表示没有额外数据，并非资源故障。 */
typedef struct {
    uint32_t rx_buffered;
    uint32_t tx_buffered;
    uint32_t rx_overruns;
    uint32_t tx_overruns;
    bool busy;
    bool backend_failed;
} rbsp_resource_runtime_status_t;

enum {
    RBSP_MCU_HEALTH_CPU_LOAD_AVAILABLE = 1U << 0,
    RBSP_MCU_HEALTH_ISR_LOAD_AVAILABLE = 1U << 1,
    RBSP_MCU_HEALTH_STACK_FREE_AVAILABLE = 1U << 2,
    RBSP_MCU_HEALTH_SAMPLE_OVERRUN_AVAILABLE = 1U << 3,
};

/*
 * 板级采样器只填写它能够真实测量的字段。producer_generation 必须在每次
 * MCU 重启后变化且非零；不能满足时回调应返回 false，Core 将明确返回不支持。
 */
typedef struct {
    uint64_t producer_generation;
    uint32_t available_fields;
    uint16_t cpu_load_permille;
    uint16_t isr_load_permille;
    uint32_t minimum_stack_free_bytes;
    uint64_t sample_overrun_total;
} rbsp_mcu_health_sample_t;

#if defined(CONFIG_REMOTEBSP_MOTION)
typedef struct {
    uint32_t logical_id;
    uint32_t enable_group_id;
    uint32_t driver_resource_id;
    uint32_t maximum_step_rate_hz;
    uint16_t step_pin;
    uint16_t direction_pin;
    uint16_t enable_pin;
    uint16_t limit_pin;
    bool enable_present;
    bool direction_inverted;
    bool enable_active_low;
    bool limit_active_low;
    uint8_t driver_type;
} rbsp_runtime_motion_axis_config_t;
#endif

#if defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
typedef struct {
    uint32_t logical_id;
    uint16_t pin;
    uint8_t address;
    uint8_t flags;
} rbsp_runtime_tmc_uart_config_t;
#endif

#if defined(CONFIG_REMOTEBSP_PWM)
typedef struct {
    uint32_t logical_id;
    uint32_t frequency_hz;
    uint16_t pin;
    uint16_t default_duty_permyriad;
    uint8_t timer;
    uint8_t timer_channel;
    bool active_low;
} rbsp_runtime_pwm_config_t;
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
typedef struct {
    uint32_t logical_id;
    uint32_t bit_rate;
    uint16_t pin;
    uint16_t maximum_bits;
    uint8_t timer;
    uint8_t timer_channel;
    uint8_t dma_channel;
} rbsp_runtime_timed_bitstream_config_t;
#endif


/*
 * 这是远程核心与具体 MCU 驱动之间唯一的边界。
 * 中断服务只负责收发字节和维护驱动状态，协议解析始终在主循环中完成。
 */
typedef struct {
    bool (*link_send)(const rbsp_link_frame_t* frame);
    /* 兼容旧板级实现；link_send 为空时使用 can_send。 */
    bool (*can_send)(const rbsp_can_frame_t* frame);
    uint32_t (*milliseconds)(void);
    /* 单调微秒时间戳；为空时 Core 使用 milliseconds()*1000。 */
    uint64_t (*microseconds)(void);
    bool (*gpio_configure)(uint16_t pin, rbsp_gpio_direction_t direction,
                           bool initial_value);
    bool (*gpio_configure_pull)(uint16_t pin,
                                rbsp_gpio_direction_t direction,
                                rbsp_gpio_pull_t pull,
                                bool initial_value);
    bool (*gpio_resource_allowed)(uint16_t pin,
                                  rbsp_gpio_direction_t direction);
    bool (*gpio_write)(uint16_t pin, bool value);
    bool (*gpio_read)(uint16_t pin, bool* value);
    bool (*uart_configure)(uint8_t port, uint32_t baud_rate,
                           uint8_t data_bits, uint8_t stop_bits,
                           uint8_t parity);
    size_t (*uart_read)(uint8_t port, uint8_t* data, size_t capacity);
    bool (*uart_write)(uint8_t port, const uint8_t* data, size_t length);
    /* 清空端口收发缓冲与板级故障锁存，不改变静态资源映射。 */
    bool (*uart_reset)(uint8_t port);
    bool (*resource_status)(uint8_t resource_type, uint16_t instance,
                            rbsp_resource_runtime_status_t* status);
    bool (*health_sample)(rbsp_mcu_health_sample_t* sample);
#if defined(CONFIG_REMOTEBSP_BUS)
    const rbsp_bus_resource_config_t* bus_resources;
    uint8_t bus_resource_count;
    rbsp_bus_transaction_status_t (*i2c_transfer)(
        const rbsp_bus_resource_config_t* device,
        uint32_t timeout_us, uint16_t flags,
        const uint8_t* write_data, uint16_t write_length,
        uint8_t* read_data, uint16_t read_length,
        uint16_t* transmitted, uint16_t* received);
    rbsp_bus_transaction_status_t (*spi_transfer)(
        const rbsp_bus_resource_config_t* device,
        uint32_t timeout_us, uint16_t flags, uint8_t dummy_byte,
        const uint8_t* transmit_data, uint16_t transmit_length,
        uint8_t* receive_data, uint16_t receive_length,
        uint16_t* transmitted, uint16_t* received);
    /* 仅复位指定总线资源；不得连带改变同控制器上的其他设备状态。 */
    bool (*bus_reset)(const rbsp_bus_resource_config_t* resource);
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    bool (*pwm_configure)(uint8_t channel, uint32_t frequency_hz,
                          uint16_t duty, bool active_low);
    bool (*pwm_write)(uint8_t channel, uint16_t duty);
    bool (*pwm_stop)(uint8_t channel);
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    bool (*timed_bitstream_configure)(uint8_t channel,
                                      uint32_t bit_period_ns,
                                      uint32_t zero_high_ns,
                                      uint32_t one_high_ns,
                                      uint32_t reset_time_us);
    bool (*timed_bitstream_write)(uint8_t channel,
                                  const uint8_t* data,
                                  uint16_t bit_count);
    bool (*timed_bitstream_busy)(uint8_t channel);
    bool (*timed_bitstream_abort)(uint8_t channel);
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    uint64_t (*nanoseconds)(void);
    /* 必须在每次 MCU 重启后产生不同的非零值；0 表示不支持跨板同步。 */
    uint64_t (*motion_boot_epoch)(void);
    uint8_t motion_axis_count;
    bool (*motion_set_enable)(uint8_t axis, bool enabled);
    bool (*motion_set_direction)(uint8_t axis, bool positive);
    bool (*motion_set_step)(uint8_t axis, bool high);
    bool (*motion_limit_active)(uint8_t axis, bool* active);
    bool (*motion_schedule_compare)(uint64_t deadline_ns);
    void (*motion_cancel_compare)(void);
    uint32_t (*motion_enter_critical)(void);
    void (*motion_exit_critical)(uint32_t state);
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
    uint64_t timestamp_us;
    uint32_t sequence;
    uint8_t edge;
    bool value;
} rbsp_gpio_input_event_t;

typedef struct {
    bool used;
    uint32_t object_id;
    uint32_t owner_session_id;
    uint16_t pin;
    rbsp_gpio_direction_t direction;
    bool input_events_enabled;
    uint8_t input_edge_mask;
    uint16_t input_queue_capacity;
    uint32_t input_debounce_us;
    bool stable_value;
    bool candidate_value;
    bool candidate_active;
    uint64_t candidate_since_us;
    uint32_t input_event_sequence;
    uint32_t input_dropped_events;
    uint16_t input_event_begin;
    uint16_t input_event_count;
    rbsp_gpio_input_event_t input_events[
        CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY];
} rbsp_gpio_object_t;


#if defined(CONFIG_REMOTEBSP_PWM)
typedef struct {
    bool used;
    uint32_t object_id;
    uint32_t owner_session_id;
    uint8_t channel;
} rbsp_pwm_object_t;
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
typedef struct {
    bool used;
    uint32_t object_id;
    uint32_t owner_session_id;
    uint8_t channel;
} rbsp_timed_bitstream_object_t;
#endif

#if CONFIG_UART_RESOURCE_COUNT > 0
typedef struct {
    bool used;
    uint32_t object_id;
    uint32_t owner_session_id;
    uint8_t port;
    bool streaming;
    uint16_t pending_length;
    uint32_t first_byte_ms;
    uint32_t event_sequence;
    uint8_t pending[CONFIG_UART_EVENT_CHUNK_SIZE];
} rbsp_uart_object_t;
#endif

typedef struct {
    uint32_t rx_overruns;
    uint32_t tx_overruns;
    bool backend_failed;
} rbsp_core_resource_counters_t;

typedef struct {
    rbsp_hal_t hal;
    rbsp_link_mode_t link_mode;
    rbsp_node_info_t info;
    uint32_t node_id;
    uint32_t next_object_id;
    uint16_t next_transfer_id;
    uint8_t cache_cursor;
    uint32_t last_heartbeat_ms;
    uint32_t bootloader_request_ms;
    rbsp_bootloader_mode_t bootloader_request_mode;
    bool bootloader_request_pending;
    uint64_t health_started_ms;
    uint32_t gpio_clock_last_ms;
    uint64_t gpio_clock_epoch_ms;
    uint64_t health_producer_generation;
    uint64_t health_sample_sequence;
    rbsp_reassembly_slot_t reassembly[CONFIG_REMOTE_REASSEMBLY_SLOTS];
    rbsp_request_cache_entry_t cache[CONFIG_REMOTE_REQUEST_CACHE_ENTRIES];
    rbsp_gpio_object_t gpio_objects[CONFIG_GPIO_RESOURCE_COUNT];
#if CONFIG_UART_RESOURCE_COUNT > 0
    rbsp_uart_object_t uart_objects[CONFIG_UART_RESOURCE_COUNT];
    rbsp_core_resource_counters_t uart_status[CONFIG_UART_RESOURCE_COUNT];
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    rbsp_bus_lease_t bus_leases[CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT];
    rbsp_core_resource_counters_t
        bus_status[CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT];
    uint64_t next_bus_lease_id;
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    rbsp_pwm_object_t pwm_objects[CONFIG_PWM_RESOURCE_COUNT];
    rbsp_core_resource_counters_t pwm_status[CONFIG_PWM_RESOURCE_COUNT];
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    rbsp_timed_bitstream_object_t timed_bitstream_objects[
        CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT];
    rbsp_core_resource_counters_t timed_bitstream_status[
        CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT];
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    rbsp_motion_queue_t motion;
    rbsp_motion_group_participant_t motion_group;
    rbsp_stepgen_lease_t stepgen_leases[CONFIG_MOTION_MAX_AXES];
    uint64_t next_stepgen_lease_id;
    uint64_t motion_resource_mask;
    uint32_t motion_owner_session_id;
    uint8_t default_motion_axis_count;
#endif
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
    rbsp_device_param_store device_params;
    uint32_t device_param_unlock_session;
    uint32_t device_param_unlock_token;
    uint32_t device_param_unlock_expires_ms;
    bool device_params_ready;
    bool device_param_restart_required;
#endif
    uint8_t tx_packet[CONFIG_REMOTE_MAX_PACKET_SIZE];
} rbsp_core_t;

bool rbsp_core_init(rbsp_core_t* core, const rbsp_hal_t* hal,
                    rbsp_link_mode_t mode, const rbsp_node_info_t* info);
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
/* 启动时加载设备参数；若已保存 UUID，同时覆盖发现与 GET_INFO 身份。 */
bool rbsp_core_device_params_init(
    rbsp_core_t* core, const rbsp_device_param_backend* backend);
#endif
void rbsp_core_poll(rbsp_core_t* core);
void rbsp_core_accept_can(rbsp_core_t* core,
                          const rbsp_can_frame_t* frame);
void rbsp_core_accept_link(rbsp_core_t* core,
                           const rbsp_link_frame_t* frame);
#if CONFIG_GPIO_RESOURCE_COUNT > 0 || defined(CONFIG_REMOTEBSP_MOTION) || \
    defined(CONFIG_REMOTEBSP_BUS)
/* 传输层确认会话结束时调用；返回已安全释放的资源租约/对象数。 */
size_t rbsp_core_release_session(rbsp_core_t* core, uint32_t session_id);
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
bool rbsp_core_motion_service(rbsp_core_t* core);
bool rbsp_core_motion_tick(rbsp_core_t* core);
#endif

#ifdef __cplusplus
}
#endif
