#include "remotebsp_embedded/core.h"

#include <string.h>

#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS) && \
    !defined(CONFIG_REMOTEBSP_DEVICE_PARAM_COMMIT_BUDGET)
#define CONFIG_REMOTEBSP_DEVICE_PARAM_COMMIT_BUDGET \
    RBSP_DEVICE_PARAM_STORE_DEFAULT_COMMIT_BUDGET
#endif

#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
#include "remotebsp_static_resources.h"
#endif

enum {
    RBSP_MESSAGE_REQUEST = 1,
    RBSP_MESSAGE_RESPONSE = 2,
    RBSP_MESSAGE_EVENT = 3,
    RBSP_COMMAND_DISCOVERY_REQUEST = 0x0001,
    RBSP_COMMAND_DISCOVERY_RESPONSE = 0x0002,
    RBSP_COMMAND_NODE_ASSIGN = 0x0003,
    RBSP_COMMAND_HEARTBEAT = 0x0004,
    RBSP_COMMAND_GET_INFO = 0x0010,
    RBSP_COMMAND_GET_CAPABILITY = 0x0011,
    RBSP_COMMAND_PING = 0x0012,
    RBSP_COMMAND_BOOTLOADER_ENTER = 0x0013,
    RBSP_COMMAND_BOOTLOADER_ENTER_USB = 0x0014,
    RBSP_COMMAND_FIRMWARE_IDENTITY = 0x0015,
    RBSP_COMMAND_TIME_SYNC = 0x0020,
    RBSP_COMMAND_HEALTH_SNAPSHOT = 0x0021,
    RBSP_COMMAND_RESOURCE_ENUM = 0x0030,
    RBSP_COMMAND_RESOURCE_DESCRIBE = 0x0031,
    RBSP_COMMAND_RESOURCE_STATUS = 0x0032,
    RBSP_COMMAND_RESOURCE_RESET = 0x0033,
    RBSP_COMMAND_RESOURCE_CONTRACT = 0x0034,
    RBSP_COMMAND_RESOURCE_ACQUIRE = 0x0035,
    RBSP_COMMAND_RESOURCE_RENEW = 0x0036,
    RBSP_COMMAND_RESOURCE_RELEASE = 0x0037,
    RBSP_COMMAND_RESOURCE_LEASE_STATUS = 0x0038,
    RBSP_COMMAND_RUNTIME_CONFIG_STATUS = 0x0040,
    RBSP_COMMAND_RUNTIME_CONFIG_READ = 0x0041,
    RBSP_COMMAND_RUNTIME_CONFIG_VALIDATE = 0x0042,
    RBSP_COMMAND_RUNTIME_CONFIG_STAGE = 0x0043,
    RBSP_COMMAND_RUNTIME_CONFIG_COMMIT = 0x0044,
    RBSP_COMMAND_RUNTIME_CONFIG_ROLLBACK = 0x0045,
    RBSP_COMMAND_DEVICE_PARAMETER_STATUS = 0x0050,
    RBSP_COMMAND_DEVICE_PARAMETER_LIST = 0x0051,
    RBSP_COMMAND_DEVICE_PARAMETER_READ = 0x0052,
    RBSP_COMMAND_DEVICE_PARAMETER_UNLOCK = 0x0053,
    RBSP_COMMAND_DEVICE_PARAMETER_WRITE = 0x0054,
    RBSP_COMMAND_DEVICE_PARAMETER_LOCK = 0x0055,
    RBSP_COMMAND_GPIO_CREATE = 0x0100,
    RBSP_COMMAND_GPIO_READ = 0x0101,
    RBSP_COMMAND_GPIO_WRITE = 0x0102,
    RBSP_COMMAND_GPIO_CLOSE = 0x0103,
    RBSP_COMMAND_GPIO_INPUT_SUBSCRIBE = 0x0104,
    RBSP_COMMAND_GPIO_INPUT_EVENT_STATUS = 0x0105,
    RBSP_COMMAND_GPIO_INPUT_EVENT = 0x0180,
    RBSP_COMMAND_UART_CREATE = 0x0200,
    RBSP_COMMAND_UART_READ = 0x0201,
    RBSP_COMMAND_UART_WRITE = 0x0202,
    RBSP_COMMAND_UART_RX_EVENT = 0x0280,
    RBSP_COMMAND_I2C_CONTRACT = 0x0300,
    RBSP_COMMAND_I2C_TRANSFER = 0x0301,
    RBSP_COMMAND_SPI_CONTRACT = 0x0400,
    RBSP_COMMAND_SPI_TRANSFER = 0x0401,
    RBSP_COMMAND_ADC_CONTRACT = 0x0500,
    RBSP_COMMAND_ADC_SAMPLE = 0x0501,
    RBSP_COMMAND_TIMER_CONTRACT = 0x0B00,
    RBSP_COMMAND_TIMER_EXECUTE = 0x0B01,
    RBSP_COMMAND_PWM_CREATE = 0x0600,
    RBSP_COMMAND_PWM_WRITE = 0x0601,
    RBSP_COMMAND_PWM_STOP = 0x0602,
    RBSP_COMMAND_TIMED_BITSTREAM_CREATE = 0x0700,
    RBSP_COMMAND_TIMED_BITSTREAM_WRITE = 0x0701,
    RBSP_COMMAND_TIMED_BITSTREAM_ABORT = 0x0702,
    RBSP_COMMAND_STORAGE_CONTRACT = 0x0800,
    RBSP_COMMAND_STORAGE_READ = 0x0801,
    RBSP_COMMAND_STORAGE_ERASE = 0x0802,
    RBSP_COMMAND_STORAGE_PROGRAM = 0x0803,
    RBSP_COMMAND_MOTION_ENQUEUE = 0x0900,
    RBSP_COMMAND_MOTION_STATUS = 0x0901,
    RBSP_COMMAND_MOTION_ABORT = 0x0902,
    RBSP_COMMAND_MOTION_CLEAR_FAULT = 0x0903,
    RBSP_COMMAND_MOTION_CONTRACT = 0x0904,
    RBSP_COMMAND_MOTION_GROUP_PREPARE = 0x0910,
    RBSP_COMMAND_MOTION_GROUP_COMMIT = 0x0911,
    RBSP_COMMAND_MOTION_GROUP_ABORT = 0x0912,
    RBSP_STATUS_OK = 0,
    RBSP_STATUS_UNKNOWN_COMMAND = 1,
    RBSP_STATUS_INVALID_PAYLOAD = 2,
    RBSP_STATUS_OBJECT_NOT_FOUND = 3,
    RBSP_STATUS_ACCESS_DENIED = 4,
    RBSP_STATUS_RESOURCE_EXHAUSTED = 5,
    RBSP_STATUS_UNSUPPORTED_CAPABILITY = 6,
    RBSP_STATUS_RESOURCE_FAILED = 7,
    RBSP_STATUS_RESOURCE_BUSY = 8,
    RBSP_RESPONSE_ERROR_FLAG = 0x0001,
    RBSP_RESOURCE_DESCRIPTOR_SIZE = 17,
    RBSP_RESOURCE_STATUS_SIZE = 25,
    RBSP_RESOURCE_CONTRACT_SIZE = 32,
    RBSP_RESOURCE_FLAG_NATIVE = 1U << 0,
    RBSP_RESOURCE_TYPE_UART = 2,
    RBSP_RESOURCE_TYPE_ADC = 5,
    RBSP_RESOURCE_TYPE_TIMER = 7,
    RBSP_RESOURCE_TYPE_PWM = 6,
    RBSP_RESOURCE_TYPE_STORAGE = 8,
    RBSP_RESOURCE_TYPE_STEPGEN_AXIS = 9,
    RBSP_RESOURCE_TYPE_TIMED_BITSTREAM = 10,
    RBSP_RESOURCE_TYPE_I2C_BUS = 11,
    RBSP_RESOURCE_TYPE_I2C_DEVICE = 12,
    RBSP_RESOURCE_TYPE_SPI_BUS = 13,
    RBSP_RESOURCE_TYPE_SPI_DEVICE = 14,
    RBSP_RESOURCE_HEALTH_NORMAL = 0,
    RBSP_RESOURCE_HEALTH_BUSY = 1,
    RBSP_RESOURCE_HEALTH_DEGRADED = 2,
    RBSP_RESOURCE_HEALTH_FAILED = 3,
    RBSP_RESOURCE_ERROR_RX_OVERFLOW = 1U << 0,
    RBSP_RESOURCE_ERROR_TX_OVERFLOW = 1U << 1,
    RBSP_RESOURCE_ERROR_BACKEND_FAILURE = 1U << 2,
    RBSP_FRAGMENT_FIRST = 0x01,
    RBSP_FRAGMENT_LAST = 0x02,
    RBSP_FRAGMENT_FLAG_MASK = 0x03,
    RBSP_FRAGMENT_LENGTH_SHIFT = 2,
    RBSP_BOOTLOADER_RESET_DELAY_MS = 100,
    RBSP_GPIO_INPUT_EVENT_VERSION = 1,
    RBSP_GPIO_INPUT_EVENT_STATUS_VERSION = 2,
    RBSP_GPIO_EDGE_RISING = 1U << 0,
    RBSP_GPIO_EDGE_FALLING = 1U << 1,
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
    RBSP_DEVICE_PARAM_PROTOCOL_VERSION = 1,
    RBSP_DEVICE_PARAM_STATUS_SIZE = 32,
    RBSP_DEVICE_PARAM_UNLOCK_CONFIRMATION = 0x50564252,
    RBSP_DEVICE_PARAM_UNLOCK_TIME_MS = 60000,
    RBSP_DEVICE_PARAM_STATUS_UNLOCKED = 1U << 0,
    RBSP_DEVICE_PARAM_STATUS_RESTART_REQUIRED = 1U << 1,
#endif
    RBSP_RESOURCE_ACCESS_READABLE = 1U << 0,
    RBSP_RESOURCE_ACCESS_WRITABLE = 1U << 1,
    RBSP_RESOURCE_ACCESS_SHARED_READ = 1U << 2,
    RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE = 1U << 3,
    RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED = 1U << 4,
    RBSP_RESOURCE_ACCESS_LEASE_REQUIRED = 1U << 5,
#if defined(CONFIG_REMOTEBSP_MOTION) || defined(CONFIG_REMOTEBSP_BUS) || \
    defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
    RBSP_RESOURCE_LEASE_MINIMUM_MS = 100,
    RBSP_RESOURCE_LEASE_MAXIMUM_MS = 60000,
    RBSP_RESOURCE_LEASE_SHARED = 1,
    RBSP_RESOURCE_LEASE_EXCLUSIVE = 2,
#endif
};

enum {
    RBSP_FIRMWARE_IDENTITY_SCHEMA_VERSION = 1U,
    RBSP_FIRMWARE_IDENTITY_PROJECT_SHA256 = 1U << 0U,
    RBSP_FIRMWARE_IDENTITY_CONFIG_SHA256 = 1U << 1U,
    RBSP_FIRMWARE_IDENTITY_INPUT_SHA256 = 1U << 2U,
};

enum {
    RBSP_HEALTH_SOURCE_MCU = 1U,
    RBSP_OVERALL_HEALTH_UNKNOWN = 0U,
    RBSP_OVERALL_HEALTH_FAULT = 3U,
    RBSP_METRIC_AVAILABLE = 1U,
    RBSP_METRIC_UNAVAILABLE = 2U,
    RBSP_METRIC_UNIT_COUNT = 1U,
    RBSP_METRIC_UNIT_BYTES = 2U,
    RBSP_METRIC_UNIT_PERMILLE = 3U,
    RBSP_METRIC_UNIT_MILLISECONDS = 4U,
    RBSP_HEALTH_HEADER_SIZE = 36U,
    RBSP_HEALTH_METRIC_SIZE = 12U,
};

#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
static int hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_sha256_literal(const char* text, uint8_t output[32]) {
    if (text == NULL || output == NULL || strlen(text) != 64U) {
        return false;
    }
    for (uint8_t index = 0U; index < 32U; ++index) {
        const int high = hex_nibble(text[index * 2U]);
        const int low = hex_nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}
#endif

#if defined(CONFIG_REMOTEBSP_BUS)
#if CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US > CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US
#error "总线最短超时不能大于最长超时"
#endif
#if CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES + 30 > CONFIG_REMOTE_CACHE_RESPONSE_SIZE
#error "总线最大读取响应必须完整放入去重缓存"
#endif
#if CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES + 39 > CONFIG_REMOTE_MAX_PACKET_SIZE
#error "总线最大请求或响应超过远程协议包预算"
#endif
#endif

static const uint8_t bootloader_confirmation[8] = {
    'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};

enum {
    RBSP_UART_RECEIVE_POLLING = 0,
    RBSP_UART_RECEIVE_STREAMING = 1,
};

typedef struct {
    uint16_t command;
    uint32_t session_id;
    uint32_t request_id;
    uint32_t object_id;
    uint16_t payload_length;
    uint16_t flags;
    uint32_t crc;
    const uint8_t* payload;
} rbsp_request_t;

static void put_u16(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t* output, uint64_t value) {
    for (unsigned index = 0; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t get_u16(const uint8_t* input) {
    return (uint16_t)((uint16_t)input[0] |
                      ((uint16_t)input[1] << 8U));
}

static uint32_t get_u32(const uint8_t* input) {
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static uint64_t gpio_monotonic_us(rbsp_core_t* core, uint32_t now_ms) {
    if (core->hal.microseconds != NULL) {
        return core->hal.microseconds();
    }
    if (now_ms < core->gpio_clock_last_ms) {
        core->gpio_clock_epoch_ms += UINT64_C(1) << 32U;
    }
    core->gpio_clock_last_ms = now_ms;
    return (core->gpio_clock_epoch_ms + now_ms) * UINT64_C(1000);
}

static void put_health_metric(uint8_t* output, uint16_t id,
                              uint8_t availability, uint8_t unit,
                              uint64_t value) {
    put_u16(output, id);
    output[2U] = availability;
    output[3U] = unit;
    put_u64(output + 4U,
            availability == RBSP_METRIC_AVAILABLE ? value : 0U);
}

static uint64_t active_lease_count(const rbsp_core_t* core) {
    uint64_t count = 0U;
    (void)core;
#if defined(CONFIG_REMOTEBSP_BUS)
    for (size_t index = 0U; index < CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT;
         ++index) {
        if (core->bus_leases[index].active) ++count;
    }
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    for (size_t index = 0U; index < CONFIG_MOTION_MAX_AXES; ++index) {
        if (core->stepgen_leases[index].active) ++count;
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMER)
    for (size_t index = 0U; index < CONFIG_TIMER_RESOURCE_COUNT; ++index)
        if (core->timer_leases[index].active) ++count;
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
    for (size_t index = 0U; index < CONFIG_STORAGE_RESOURCE_COUNT; ++index)
        if (core->storage_leases[index].active) ++count;
#endif
    return count;
}

static uint64_t resource_fault_count(const rbsp_core_t* core) {
    uint64_t count = 0U;
    (void)core;
#if defined(CONFIG_REMOTEBSP_BUS)
    for (size_t index = 0U; index < CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT;
         ++index) {
        if (core->bus_status[index].backend_failed) ++count;
    }
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
    for (size_t index = 0U; index < CONFIG_UART_RESOURCE_COUNT; ++index) {
        if (core->uart_status[index].backend_failed) ++count;
    }
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    for (size_t index = 0U; index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
        if (core->pwm_status[index].backend_failed) ++count;
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    for (size_t index = 0U; index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT;
         ++index) {
        if (core->timed_bitstream_status[index].backend_failed) ++count;
    }
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    if (core->motion.fault != RBSP_MOTION_FAULT_NONE) ++count;
#endif
    return count;
}

#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
static bool device_param_unlock_active(const rbsp_core_t* core,
                                       uint32_t session_id,
                                       uint32_t now_ms) {
    return core->device_params_ready &&
           core->device_param_unlock_token != 0U &&
           core->device_param_unlock_session == session_id &&
           (int32_t)(core->device_param_unlock_expires_ms - now_ms) > 0;
}

static void encode_device_param_status(const rbsp_core_t* core,
                                       uint32_t session_id,
                                       uint32_t now_ms,
                                       uint8_t output[
                                           RBSP_DEVICE_PARAM_STATUS_SIZE]) {
    uint16_t flags = 0U;
    memset(output, 0, RBSP_DEVICE_PARAM_STATUS_SIZE);
    if (device_param_unlock_active(core, session_id, now_ms)) {
        flags |= RBSP_DEVICE_PARAM_STATUS_UNLOCKED;
    }
    if (core->device_param_restart_required) {
        flags |= RBSP_DEVICE_PARAM_STATUS_RESTART_REQUIRED;
    }
    put_u16(output, RBSP_DEVICE_PARAM_PROTOCOL_VERSION);
    put_u16(output + 2U, flags);
    put_u32(output + 4U, core->device_params.generation);
    put_u16(output + 8U, core->device_params.record_count);
    put_u16(output + 10U,
            (uint16_t)rbsp_device_param_definition_count());
    output[12U] = (uint8_t)core->device_params.last_error;
    output[13U] = 1U; /* store health字段版本 */
    output[14U] = core->device_params.bad_page_mask;
    put_u32(output + 16U, core->device_params.commit_budget);
    put_u32(output + 20U, core->device_params.write_attempts);
    put_u32(output + 24U, core->device_params.successful_commits);
    put_u32(output + 28U, core->device_params.io_failures);
}
#endif

#if defined(CONFIG_REMOTEBSP_MOTION) || defined(CONFIG_REMOTEBSP_BUS) || \
    defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
static uint64_t get_u64(const uint8_t* input) {
    uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= (uint64_t)input[index] << (index * 8U);
    }
    return value;
}
#endif

#if defined(CONFIG_REMOTEBSP_BUS)
static uint16_t make_status_response(
    rbsp_core_t* core, const rbsp_request_t* request,
    uint8_t status, uint32_t object_id,
    const uint8_t* data, uint16_t data_length);
static const rbsp_bus_resource_config_t* find_bus_resource(
    const rbsp_core_t* core, uint32_t resource_id, size_t* resource_index);
static bool bus_has_device_kind(const rbsp_core_t* core, uint8_t kind);
static bool bus_lease_active(rbsp_core_t* core, size_t resource_index,
                             uint32_t session_id);
static void encode_bus_contract(
    uint8_t output[32U], const rbsp_bus_resource_config_t* item);

static bool handle_bus_command(rbsp_core_t* core,
                               const rbsp_request_t* request,
                               uint16_t* encoded_response_size) {
    uint16_t response_size = 0U;
    uint8_t* payload = core->tx_packet + RBSP_HEADER_SIZE;
    switch (request->command) {
        case RBSP_COMMAND_I2C_CONTRACT:
        case RBSP_COMMAND_SPI_CONTRACT: {
            const uint8_t expected_kind =
                request->command == RBSP_COMMAND_I2C_CONTRACT
                    ? RBSP_BUS_I2C_DEVICE
                    : RBSP_BUS_SPI_DEVICE;
            const uint32_t resource_id = request->payload_length == 4U
                                             ? get_u32(request->payload)
                                             : 0U;
            const rbsp_bus_resource_config_t* resource =
                request->payload_length == 4U
                    ? find_bus_resource(core, resource_id, NULL)
                    : NULL;
            if (request->object_id != 0U ||
                request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!bus_has_device_kind(core, expected_kind)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (resource == NULL || resource->kind != expected_kind) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t data[32U];
                encode_bus_contract(data, resource);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_I2C_TRANSFER: {
            const uint16_t write_length = request->payload_length >= 14U
                                              ? get_u16(request->payload + 12U)
                                              : 0U;
            const uint16_t read_length = request->payload_length >= 14U
                                             ? get_u16(request->payload + 10U)
                                             : 0U;
            const uint16_t flags = request->payload_length >= 14U
                                       ? get_u16(request->payload + 8U)
                                       : 0U;
            const uint32_t timeout_us = request->payload_length >= 14U
                                            ? get_u32(request->payload + 4U)
                                            : 0U;
            size_t resource_index = 0U;
            const rbsp_bus_resource_config_t* resource =
                request->payload_length >= 14U
                    ? find_bus_resource(
                          core, get_u32(request->payload), &resource_index)
                    : NULL;
            const uint32_t total = (uint32_t)write_length + read_length;
            if (request->object_id != 0U ||
                request->payload_length < 14U ||
                request->payload_length != (uint16_t)(14U + write_length) ||
                total == 0U || total > CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!bus_has_device_kind(core, RBSP_BUS_I2C_DEVICE)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (resource == NULL ||
                       resource->kind != RBSP_BUS_I2C_DEVICE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else if (timeout_us < resource->minimum_timeout_us ||
                       timeout_us > resource->maximum_timeout_us ||
                       total > resource->maximum_transfer_bytes ||
                       (flags & (uint16_t)~(
                           RBSP_BUS_CONTRACT_I2C_REPEATED_START |
                           RBSP_BUS_CONTRACT_I2C_RECOVERY)) != 0U ||
                       (flags & (uint16_t)~resource->flags) != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!bus_lease_active(
                           core, resource_index, request->session_id)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else {
                uint16_t transmitted = 0U;
                uint16_t received = 0U;
                uint8_t* result = payload + 1U;
                /* HAL 失败时即使错误地上报 received，也不得泄露旧响应字节。 */
                memset(result + 5U, 0, read_length);
                const rbsp_bus_transaction_status_t status =
                    core->hal.i2c_transfer(
                        resource, timeout_us, flags,
                        request->payload + 14U, write_length,
                        result + 5U, read_length,
                        &transmitted, &received);
                if (status > RBSP_BUS_TRANSACTION_LIMIT_EXCEEDED ||
                    transmitted > write_length || received > read_length ||
                    (status == RBSP_BUS_TRANSACTION_OK &&
                     (transmitted != write_length || received != read_length))) {
                    core->bus_status[resource_index].backend_failed = true;
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_FAILED,
                        0U, NULL, 0U);
                } else {
                    if (status == RBSP_BUS_TRANSACTION_FAULT) {
                        core->bus_status[resource_index].backend_failed = true;
                    }
                    result[0U] = (uint8_t)status;
                    put_u16(result + 1U, transmitted);
                    put_u16(result + 3U, received);
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK, 0U,
                        result, (uint16_t)(5U + received));
                }
            }
            break;
        }

        case RBSP_COMMAND_SPI_TRANSFER: {
            const uint16_t transmit_length = request->payload_length >= 15U
                                                 ? get_u16(request->payload + 13U)
                                                 : 0U;
            const uint16_t receive_length = request->payload_length >= 15U
                                                ? get_u16(request->payload + 10U)
                                                : 0U;
            const uint16_t flags = request->payload_length >= 15U
                                       ? get_u16(request->payload + 8U)
                                       : 0U;
            const uint32_t timeout_us = request->payload_length >= 15U
                                            ? get_u32(request->payload + 4U)
                                            : 0U;
            size_t resource_index = 0U;
            const rbsp_bus_resource_config_t* resource =
                request->payload_length >= 15U
                    ? find_bus_resource(
                          core, get_u32(request->payload), &resource_index)
                    : NULL;
            const uint16_t maximum_length = transmit_length > receive_length
                                                ? transmit_length
                                                : receive_length;
            if (request->object_id != 0U ||
                request->payload_length < 15U ||
                request->payload_length !=
                    (uint16_t)(15U + transmit_length) ||
                maximum_length == 0U ||
                maximum_length > CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!bus_has_device_kind(core, RBSP_BUS_SPI_DEVICE)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (resource == NULL ||
                       resource->kind != RBSP_BUS_SPI_DEVICE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else if (timeout_us < resource->minimum_timeout_us ||
                timeout_us > resource->maximum_timeout_us ||
                maximum_length > resource->maximum_transfer_bytes ||
                (resource->bits_per_word == 16U &&
                 ((transmit_length & 1U) != 0U ||
                  (receive_length & 1U) != 0U)) ||
                (transmit_length != 0U && receive_length != 0U &&
                 (resource->flags &
                  RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX) == 0U) ||
                (flags & (uint16_t)~
                           RBSP_BUS_SPI_TRANSFER_KEEP_CHIP_SELECT) != 0U ||
                       ((flags &
                         RBSP_BUS_SPI_TRANSFER_KEEP_CHIP_SELECT) != 0U &&
                        (resource->flags &
                         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT) == 0U)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!bus_lease_active(
                           core, resource_index, request->session_id)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else {
                uint16_t transmitted = 0U;
                uint16_t received = 0U;
                uint8_t* result = payload + 1U;
                /* HAL 失败时即使错误地上报 received，也不得泄露旧响应字节。 */
                memset(result + 5U, 0, receive_length);
                const rbsp_bus_transaction_status_t status =
                    core->hal.spi_transfer(
                        resource, timeout_us, flags, request->payload[12U],
                        request->payload + 15U, transmit_length,
                        result + 5U, receive_length,
                        &transmitted, &received);
                if (status > RBSP_BUS_TRANSACTION_LIMIT_EXCEEDED ||
                    transmitted > transmit_length ||
                    received > receive_length ||
                    (status == RBSP_BUS_TRANSACTION_OK &&
                     (transmitted != transmit_length ||
                      received != receive_length)) ||
                    (resource->bits_per_word == 16U &&
                     ((transmitted & 1U) != 0U ||
                      (received & 1U) != 0U))) {
                    core->bus_status[resource_index].backend_failed = true;
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_FAILED,
                        0U, NULL, 0U);
                } else {
                    if (status == RBSP_BUS_TRANSACTION_FAULT) {
                        core->bus_status[resource_index].backend_failed = true;
                    }
                    result[0U] = (uint8_t)status;
                    put_u16(result + 1U, transmitted);
                    put_u16(result + 3U, received);
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK, 0U,
                        result, (uint16_t)(5U + received));
                }
            }
            break;
        }
        default:
            return false;
    }
    *encoded_response_size = response_size;
    return true;
}
#endif

#if defined(CONFIG_REMOTEBSP_PWM)
static rbsp_pwm_object_t* find_pwm_object(rbsp_core_t* core,
                                          uint32_t object_id) {
    for (size_t index = 0; index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
        if (core->pwm_objects[index].used &&
            core->pwm_objects[index].object_id == object_id) {
            return &core->pwm_objects[index];
        }
    }
    return NULL;
}
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
static rbsp_timed_bitstream_object_t* find_timed_bitstream_object(
    rbsp_core_t* core, uint32_t object_id) {
    for (size_t index = 0;
         index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT; ++index) {
        if (core->timed_bitstream_objects[index].used &&
            core->timed_bitstream_objects[index].object_id == object_id) {
            return &core->timed_bitstream_objects[index];
        }
    }
    return NULL;
}
#endif

static uint32_t crc32_update(uint32_t crc, const uint8_t* data,
                             size_t size) {
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static uint32_t packet_crc(const uint8_t* packet, uint16_t size) {
    uint32_t crc = crc32_update(0xFFFFFFFFU, packet, 20U);
    crc = crc32_update(crc, packet + RBSP_HEADER_SIZE,
                       (size_t)size - RBSP_HEADER_SIZE);
    return ~crc;
}

static bool decode_request(const uint8_t* packet, uint16_t size,
                           rbsp_request_t* request) {
    if (size < RBSP_HEADER_SIZE ||
        packet[0] != RBSP_PROTOCOL_VERSION ||
        packet[1] != RBSP_MESSAGE_REQUEST) {
        return false;
    }
    const uint16_t payload_length = get_u16(packet + 16U);
    if ((uint32_t)RBSP_HEADER_SIZE + payload_length != size ||
        packet_crc(packet, size) != get_u32(packet + 20U)) {
        return false;
    }
    request->command = get_u16(packet + 2U);
    request->session_id = get_u32(packet + 4U);
    request->request_id = get_u32(packet + 8U);
    request->object_id = get_u32(packet + 12U);
    request->payload_length = payload_length;
    request->flags = get_u16(packet + 18U);
    request->crc = get_u32(packet + 20U);
    request->payload = packet + RBSP_HEADER_SIZE;
    return true;
}

static uint16_t encode_packet(uint8_t* output, uint8_t message_type,
                              uint16_t command, uint32_t session_id,
                              uint32_t request_id, uint32_t object_id,
                              uint16_t flags, const uint8_t* payload,
                              uint16_t payload_length) {
    const uint16_t size =
        (uint16_t)(RBSP_HEADER_SIZE + payload_length);
    output[0] = RBSP_PROTOCOL_VERSION;
    output[1] = message_type;
    put_u16(output + 2U, command);
    put_u32(output + 4U, session_id);
    put_u32(output + 8U, request_id);
    put_u32(output + 12U, object_id);
    put_u16(output + 16U, payload_length);
    put_u16(output + 18U, flags);
    put_u32(output + 20U, 0U);
    if (payload_length != 0U && payload != output + RBSP_HEADER_SIZE) {
        memmove(output + RBSP_HEADER_SIZE, payload, payload_length);
    }
    put_u32(output + 20U, packet_crc(output, size));
    return size;
}

static uint8_t canonical_fd_length(uint8_t length) {
    static const uint8_t lengths[] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U,
        12U, 16U, 20U, 24U, 32U, 48U, 64U};
    for (size_t index = 0;
         index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        if (length <= lengths[index]) {
            return lengths[index];
        }
    }
    return 64U;
}

static uint8_t link_mtu(rbsp_link_mode_t mode) {
    return mode == RBSP_CAN_CLASSICAL ? 8U : 64U;
}

static bool send_link_frame(rbsp_core_t* core,
                            const rbsp_link_frame_t* frame) {
    if (core->hal.link_send != NULL) {
        return core->hal.link_send(frame);
    }
    return core->hal.can_send != NULL && core->hal.can_send(frame);
}

static bool send_packet(rbsp_core_t* core, const uint8_t* packet,
                        uint16_t packet_size, uint16_t transfer_id,
                        uint32_t route) {
    const uint8_t mtu = link_mtu(core->link_mode);
    const uint8_t capacity =
        (uint8_t)(mtu - RBSP_FRAGMENT_HEADER_SIZE);
    uint16_t sequence = 0U;
    uint16_t offset = 0U;

    while (offset < packet_size) {
        const uint16_t remaining = (uint16_t)(packet_size - offset);
        const uint8_t length =
            remaining > capacity ? capacity : (uint8_t)remaining;
        rbsp_link_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.route = route;
        put_u16(frame.data, transfer_id);
        put_u16(frame.data + 2U, sequence);
        frame.data[4] =
            (uint8_t)(length << RBSP_FRAGMENT_LENGTH_SHIFT);
        if (sequence == 0U) {
            frame.data[4] |= RBSP_FRAGMENT_FIRST;
        }
        if ((uint16_t)(offset + length) == packet_size) {
            frame.data[4] |= RBSP_FRAGMENT_LAST;
        }
        memcpy(frame.data + RBSP_FRAGMENT_HEADER_SIZE,
               packet + offset, length);
        frame.length =
            (uint8_t)(RBSP_FRAGMENT_HEADER_SIZE + length);
        if (core->link_mode == RBSP_CAN_FD) {
            frame.length = canonical_fd_length(frame.length);
        }
        if (!send_link_frame(core, &frame)) {
            return false;
        }
        offset = (uint16_t)(offset + length);
        ++sequence;
    }
    return true;
}

static uint16_t allocate_transfer_id(rbsp_core_t* core) {
    const uint16_t result = core->next_transfer_id++;
    if (core->next_transfer_id == 0U) {
        core->next_transfer_id = 1U;
    }
    return result;
}

static uint32_t response_can_id(const rbsp_core_t* core) {
    if (core->node_id == 0U) {
        return RBSP_ROUTE_PROVISIONAL_BASE +
               CONFIG_NODE_PROVISIONAL_ID;
    }
    return RBSP_ROUTE_RESPONSE_BASE + core->node_id;
}

static bool same_cached_request(
    const rbsp_request_cache_entry_t* entry,
    const rbsp_request_t* request) {
    return entry->command == request->command &&
           entry->object_id == request->object_id &&
           entry->flags == request->flags &&
           entry->request_crc == request->crc;
}

static rbsp_request_cache_entry_t* find_cache(
    rbsp_core_t* core, const rbsp_request_t* request) {
    for (size_t index = 0;
         index < CONFIG_REMOTE_REQUEST_CACHE_ENTRIES; ++index) {
        rbsp_request_cache_entry_t* entry = &core->cache[index];
        if (entry->valid &&
            entry->session_id == request->session_id &&
            entry->request_id == request->request_id) {
            return entry;
        }
    }
    return NULL;
}

static void insert_cache(rbsp_core_t* core,
                         const rbsp_request_t* request,
                         const uint8_t* response,
                         uint16_t response_size) {
    if (response_size > CONFIG_REMOTE_CACHE_RESPONSE_SIZE) {
        return;
    }
    rbsp_request_cache_entry_t* entry =
        &core->cache[core->cache_cursor];
    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    entry->session_id = request->session_id;
    entry->request_id = request->request_id;
    entry->command = request->command;
    entry->object_id = request->object_id;
    entry->flags = request->flags;
    entry->request_crc = request->crc;
    entry->response_size = response_size;
    memcpy(entry->response, response, response_size);
    core->cache_cursor =
        (uint8_t)((core->cache_cursor + 1U) %
                  CONFIG_REMOTE_REQUEST_CACHE_ENTRIES);
}

static uint16_t make_status_response(
    rbsp_core_t* core, const rbsp_request_t* request,
    uint8_t status, uint32_t object_id,
    const uint8_t* data, uint16_t data_length) {
    uint8_t* payload = core->tx_packet + RBSP_HEADER_SIZE;
    payload[0] = status;
    if (data_length != 0U && data != payload + 1U) {
        memmove(payload + 1U, data, data_length);
    }
    return encode_packet(
        core->tx_packet, RBSP_MESSAGE_RESPONSE, request->command,
        request->session_id, request->request_id, object_id,
        status == RBSP_STATUS_OK ? 0U : RBSP_RESPONSE_ERROR_FLAG,
        payload, (uint16_t)(data_length + 1U));
}

static rbsp_gpio_object_t* find_gpio_object(rbsp_core_t* core,
                                             uint32_t object_id) {
    for (size_t index = 0; index < CONFIG_GPIO_RESOURCE_COUNT; ++index) {
        if (core->gpio_objects[index].used &&
            core->gpio_objects[index].object_id == object_id) {
            return &core->gpio_objects[index];
        }
    }
    return NULL;
}

static bool configure_gpio(rbsp_core_t* core, uint16_t pin,
                           rbsp_gpio_direction_t direction,
                           rbsp_gpio_pull_t pull,
                           bool initial_value) {
    if (core->hal.gpio_configure_pull != NULL) {
        return core->hal.gpio_configure_pull(
            pin, direction, pull, initial_value);
    }
    return pull == RBSP_GPIO_FLOATING &&
           core->hal.gpio_configure != NULL &&
           core->hal.gpio_configure(pin, direction, initial_value);
}

#if defined(CONFIG_REMOTEBSP_BUS)
static const rbsp_bus_resource_config_t* find_bus_resource(
    const rbsp_core_t* core, uint32_t resource_id, size_t* resource_index) {
    if (core == NULL || core->hal.bus_resources == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < core->hal.bus_resource_count; ++index) {
        if (core->hal.bus_resources[index].resource_id == resource_id) {
            if (resource_index != NULL) {
                *resource_index = index;
            }
            return &core->hal.bus_resources[index];
        }
    }
    return NULL;
}

static bool bus_kind_is_device(uint8_t kind) {
    return kind == RBSP_BUS_I2C_DEVICE || kind == RBSP_BUS_SPI_DEVICE;
}

static bool bus_resource_id_matches_kind(uint32_t resource_id,
                                         uint8_t kind) {
    /* v1 资源 ID 高字节固定为 ResourceType；总线种类 1..4 对应 11..14。 */
    return kind >= RBSP_BUS_I2C_BUS && kind <= RBSP_BUS_SPI_DEVICE &&
           (resource_id >> 24U) == (uint32_t)(kind + 10U);
}

static bool bus_configuration_valid(const rbsp_hal_t* hal) {
    if (hal->bus_resources == NULL || hal->bus_resource_count == 0U ||
        hal->bus_resource_count > CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT) {
        return false;
    }
    bool has_i2c_device = false;
    bool has_spi_device = false;
    for (size_t index = 0U; index < hal->bus_resource_count; ++index) {
        const rbsp_bus_resource_config_t* item = &hal->bus_resources[index];
        if (item->resource_id == 0U ||
            !bus_resource_id_matches_kind(item->resource_id, item->kind) ||
            item->maximum_clock_hz == 0U ||
            item->maximum_transfer_bytes == 0U ||
            item->maximum_transfer_bytes >
                CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES ||
            item->queue_capacity != 1U ||
            item->minimum_timeout_us < CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US ||
            item->maximum_timeout_us > CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US ||
            item->minimum_timeout_us > item->maximum_timeout_us ||
            item->maximum_operations_per_second == 0U) {
            return false;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (hal->bus_resources[previous].resource_id == item->resource_id) {
                return false;
            }
        }
        if (item->kind == RBSP_BUS_I2C_BUS ||
            item->kind == RBSP_BUS_SPI_BUS) {
            const uint8_t allowed = item->kind == RBSP_BUS_I2C_BUS
                                        ? (uint8_t)(
                                              RBSP_BUS_CONTRACT_I2C_REPEATED_START |
                                              RBSP_BUS_CONTRACT_I2C_RECOVERY)
                                        : (uint8_t)(
                                              RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
                                              RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT);
            if (item->parent_bus_resource_id != 0U ||
                item->device_value != 0U || item->controller == 0U ||
                (item->flags & (uint8_t)~allowed) != 0U) {
                return false;
            }
            continue;
        }
        if (!bus_kind_is_device(item->kind) ||
            item->parent_bus_resource_id == 0U) {
            return false;
        }
        const rbsp_bus_resource_config_t* parent = NULL;
        for (size_t candidate = 0U; candidate < hal->bus_resource_count;
             ++candidate) {
            if (hal->bus_resources[candidate].resource_id ==
                item->parent_bus_resource_id) {
                parent = &hal->bus_resources[candidate];
                break;
            }
        }
        if (parent == NULL || parent->controller != item->controller ||
            item->maximum_clock_hz > parent->maximum_clock_hz ||
            item->maximum_transfer_bytes > parent->maximum_transfer_bytes ||
            item->minimum_timeout_us < parent->minimum_timeout_us ||
            item->maximum_timeout_us > parent->maximum_timeout_us ||
            item->queue_capacity > parent->queue_capacity ||
            item->maximum_operations_per_second >
                parent->maximum_operations_per_second) {
            return false;
        }
        if (item->kind == RBSP_BUS_I2C_DEVICE) {
            has_i2c_device = true;
            if (parent->kind != RBSP_BUS_I2C_BUS ||
                item->device_value == 0U || item->device_value > 0x7FU ||
                item->spi_mode != 0U || item->bits_per_word != 0U ||
                (item->flags & (uint8_t)~(
                    RBSP_BUS_CONTRACT_I2C_REPEATED_START |
                    RBSP_BUS_CONTRACT_I2C_RECOVERY)) != 0U) {
                return false;
            }
        } else {
            has_spi_device = true;
            if (parent->kind != RBSP_BUS_SPI_BUS ||
                item->device_value == 0U || item->spi_mode > 3U ||
                (item->bits_per_word != 8U &&
                 item->bits_per_word != 16U) ||
                (item->flags & (uint8_t)~(
                    RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
                    RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT)) != 0U) {
                return false;
            }
        }
        if ((item->flags & (uint8_t)~parent->flags) != 0U) {
            return false;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            const rbsp_bus_resource_config_t* other =
                &hal->bus_resources[previous];
            if (other->kind == item->kind &&
                other->parent_bus_resource_id ==
                    item->parent_bus_resource_id &&
                other->device_value == item->device_value) {
                return false;
            }
        }
    }
    return (!has_i2c_device || hal->i2c_transfer != NULL) &&
           (!has_spi_device || hal->spi_transfer != NULL) &&
           (has_i2c_device || has_spi_device);
}

static bool bus_has_device_kind(const rbsp_core_t* core, uint8_t kind) {
    for (size_t index = 0U; index < core->hal.bus_resource_count; ++index) {
        if (core->hal.bus_resources[index].kind == kind) {
            return true;
        }
    }
    return false;
}

static uint32_t bus_lease_remaining_ms(const rbsp_bus_lease_t* lease,
                                       uint32_t now_ms) {
    return lease != NULL && lease->active &&
                   (int32_t)(lease->expires_at_ms - now_ms) > 0
               ? lease->expires_at_ms - now_ms
               : 0U;
}

static void encode_bus_lease_info(
    uint8_t output[27U], uint32_t resource_id,
    const rbsp_bus_lease_t* lease, uint32_t requester_session_id,
    uint32_t now_ms, bool include_token) {
    memset(output, 0, 27U);
    put_u32(output, resource_id);
    if (lease == NULL || !lease->active) {
        return;
    }
    if (include_token && lease->owner_session_id == requester_session_id) {
        put_u64(output + 4U, lease->lease_id);
    }
    put_u32(output + 12U, lease->owner_session_id);
    put_u32(output + 16U, lease->granted_duration_ms);
    put_u32(output + 20U, bus_lease_remaining_ms(lease, now_ms));
    output[24U] = RBSP_RESOURCE_LEASE_EXCLUSIVE;
    put_u16(output + 25U, 1U);
}

static size_t expire_bus_leases(rbsp_core_t* core, uint32_t now_ms) {
    size_t expired = 0U;
    for (size_t index = 0U; index < core->hal.bus_resource_count; ++index) {
        rbsp_bus_lease_t* lease = &core->bus_leases[index];
        if (lease->active &&
            (int32_t)(now_ms - lease->expires_at_ms) >= 0) {
            memset(lease, 0, sizeof(*lease));
            ++expired;
        }
    }
    return expired;
}

static bool bus_lease_active(rbsp_core_t* core, size_t resource_index,
                             uint32_t session_id) {
    (void)expire_bus_leases(core, core->hal.milliseconds());
    return session_id != 0U &&
           resource_index < core->hal.bus_resource_count &&
           core->bus_leases[resource_index].active &&
           core->bus_leases[resource_index].owner_session_id == session_id;
}

static void encode_bus_contract(uint8_t output[32U],
                                const rbsp_bus_resource_config_t* item) {
    memset(output, 0, 32U);
    put_u32(output, item->resource_id);
    put_u16(output + 4U, 1U);
    output[6U] = item->kind;
    output[7U] = item->flags;
    put_u32(output + 8U, item->parent_bus_resource_id);
    put_u32(output + 12U, item->maximum_clock_hz);
    put_u16(output + 16U, item->maximum_transfer_bytes);
    put_u16(output + 18U, item->queue_capacity);
    put_u32(output + 20U, item->minimum_timeout_us);
    put_u32(output + 24U, item->maximum_timeout_us);
    put_u32(output + 28U, item->maximum_operations_per_second);
}

static bool handle_bus_resource_command(
    rbsp_core_t* core, const rbsp_request_t* request,
    uint16_t* response_size) {
    uint16_t required_length = 0U;
    switch (request->command) {
        case RBSP_COMMAND_RESOURCE_CONTRACT:
        case RBSP_COMMAND_RESOURCE_LEASE_STATUS:
            required_length = 4U;
            break;
        case RBSP_COMMAND_RESOURCE_ACQUIRE:
            required_length = 9U;
            break;
        case RBSP_COMMAND_RESOURCE_RENEW:
        case RBSP_COMMAND_RESOURCE_RELEASE:
            required_length = 16U;
            break;
        default:
            return false;
    }
    if (request->payload_length != required_length) {
        return false;
    }
    size_t resource_index = 0U;
    const uint32_t resource_id = get_u32(request->payload);
    const rbsp_bus_resource_config_t* resource = find_bus_resource(
        core, resource_id, &resource_index);
    if (resource == NULL || !bus_kind_is_device(resource->kind)) {
        return false;
    }
    if (request->object_id != 0U) {
        *response_size = make_status_response(
            core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
        return true;
    }
    const uint32_t now_ms = core->hal.milliseconds();
    rbsp_bus_lease_t* lease = &core->bus_leases[resource_index];
    if (request->command == RBSP_COMMAND_RESOURCE_CONTRACT) {
        uint8_t data[32U];
        memset(data, 0, sizeof(data));
        put_u32(data, resource_id);
        put_u16(data + 4U, 1U);
        put_u16(data + 6U,
                RBSP_RESOURCE_ACCESS_READABLE |
                    RBSP_RESOURCE_ACCESS_WRITABLE |
                    RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE |
                    RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED |
                    RBSP_RESOURCE_ACCESS_LEASE_REQUIRED);
        put_u32(data + 16U, resource->maximum_operations_per_second);
        put_u32(data + 20U, resource->queue_capacity);
        *response_size = make_status_response(
            core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
        return true;
    }
    if (request->command == RBSP_COMMAND_RESOURCE_LEASE_STATUS) {
        uint8_t data[27U];
        encode_bus_lease_info(data, resource_id, lease,
                              request->session_id, now_ms, false);
        *response_size = make_status_response(
            core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
        return true;
    }
    if (request->session_id == 0U) {
        *response_size = make_status_response(
            core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
        return true;
    }
    if (request->command == RBSP_COMMAND_RESOURCE_ACQUIRE) {
        const uint32_t duration_ms = get_u32(request->payload + 4U);
        if (duration_ms < RBSP_RESOURCE_LEASE_MINIMUM_MS ||
            duration_ms > RBSP_RESOURCE_LEASE_MAXIMUM_MS ||
            request->payload[8U] != RBSP_RESOURCE_LEASE_EXCLUSIVE) {
            *response_size = make_status_response(
                core, request,
                request->payload[8U] > RBSP_RESOURCE_LEASE_EXCLUSIVE ||
                        request->payload[8U] == 0U
                    ? RBSP_STATUS_INVALID_PAYLOAD
                    : RBSP_STATUS_ACCESS_DENIED,
                0U, NULL, 0U);
            return true;
        }
        if (lease->active) {
            *response_size = make_status_response(
                core, request, RBSP_STATUS_RESOURCE_BUSY, 0U, NULL, 0U);
            return true;
        }
        lease->active = true;
        lease->lease_id = core->next_bus_lease_id++;
        if (core->next_bus_lease_id == 0U) {
            core->next_bus_lease_id = 1U;
        }
        lease->owner_session_id = request->session_id;
        lease->granted_duration_ms = duration_ms;
        lease->expires_at_ms = now_ms + duration_ms;
        uint8_t data[27U];
        encode_bus_lease_info(data, resource_id, lease,
                              request->session_id, now_ms, true);
        *response_size = make_status_response(
            core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
        return true;
    }
    const uint64_t lease_id = get_u64(request->payload + 4U);
    const uint32_t duration_ms = get_u32(request->payload + 12U);
    const bool release = request->command == RBSP_COMMAND_RESOURCE_RELEASE;
    if (lease_id == 0U ||
        (release ? duration_ms != 0U
                 : duration_ms < RBSP_RESOURCE_LEASE_MINIMUM_MS ||
                       duration_ms > RBSP_RESOURCE_LEASE_MAXIMUM_MS)) {
        *response_size = make_status_response(
            core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
    } else if (!lease->active || lease->lease_id != lease_id ||
               lease->owner_session_id != request->session_id) {
        *response_size = make_status_response(
            core, request, RBSP_STATUS_ACCESS_DENIED, 0U, NULL, 0U);
    } else if (release) {
        memset(lease, 0, sizeof(*lease));
        *response_size = make_status_response(
            core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
    } else {
        lease->granted_duration_ms = duration_ms;
        lease->expires_at_ms = now_ms + duration_ms;
        uint8_t data[27U];
        encode_bus_lease_info(data, resource_id, lease,
                              request->session_id, now_ms, true);
        *response_size = make_status_response(
            core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
    }
    return true;
}
#endif


#if defined(CONFIG_REMOTEBSP_MOTION)
static bool motion_available(const rbsp_core_t* core) {
    return core->hal.motion_axis_count > 0U &&
           core->motion.axis_count == core->hal.motion_axis_count &&
           core->hal.nanoseconds != NULL &&
           core->hal.motion_set_enable != NULL &&
           core->hal.motion_set_direction != NULL &&
           core->hal.motion_set_step != NULL &&
           core->hal.motion_schedule_compare != NULL &&
           core->hal.motion_cancel_compare != NULL &&
           core->hal.motion_enter_critical != NULL &&
           core->hal.motion_exit_critical != NULL;
}

static uint32_t motion_resource_id(const rbsp_core_t* core,
                                   uint8_t axis) {
    (void)core;
    return 0x09000000UL + axis;
}

static bool motion_axis_from_resource(const rbsp_core_t* core,
                                      uint32_t resource_id,
                                      uint8_t* axis) {
    if (core == NULL || axis == NULL) {
        return false;
    }
    for (uint8_t candidate = 0U;
         candidate < core->motion.axis_count; ++candidate) {
        if (motion_resource_id(core, candidate) == resource_id) {
            *axis = candidate;
            return true;
        }
    }
    return false;
}

static bool stepgen_lease_active(const rbsp_core_t* core, uint8_t axis,
                                 uint32_t session_id) {
    return core != NULL && axis < core->motion.axis_count &&
           session_id != 0U && core->stepgen_leases[axis].active &&
           core->stepgen_leases[axis].owner_session_id == session_id;
}

static bool stepgen_mask_leased(const rbsp_core_t* core,
                                uint64_t resource_mask,
                                uint32_t session_id) {
    if (resource_mask == 0U || session_id == 0U) {
        return false;
    }
    for (uint8_t axis = 0U; axis < core->motion.axis_count; ++axis) {
        if ((resource_mask & (UINT64_C(1) << axis)) != 0U &&
            !stepgen_lease_active(core, axis, session_id)) {
            return false;
        }
    }
    return true;
}

static void observe_motion_owner(rbsp_core_t* core) {
    if (core->motion.state == RBSP_MOTION_IDLE && core->motion.size == 0U) {
        core->motion_owner_session_id = 0U;
        core->motion_resource_mask = 0U;
    }
}

static bool stop_for_stepgen_lease_loss(rbsp_core_t* core, uint8_t axis,
                                        uint32_t owner_session_id) {
    bool stopped_motion = false;
    const uint32_t critical_state = core->hal.motion_enter_critical();
    if ((core->motion_group.state == RBSP_MOTION_GROUP_PREPARED ||
         core->motion_group.state == RBSP_MOTION_GROUP_ARMED) &&
        rbsp_motion_group_owned_by(&core->motion_group, owner_session_id) &&
        rbsp_motion_group_uses_axis(&core->motion_group, axis)) {
        (void)rbsp_motion_group_emergency_abort(&core->motion_group);
    }
    if (core->motion_owner_session_id == owner_session_id &&
        (core->motion_resource_mask & (UINT64_C(1) << axis)) != 0U) {
        if (core->motion.size != 0U ||
            core->motion.state == RBSP_MOTION_ARMED ||
            core->motion.state == RBSP_MOTION_RUNNING) {
            rbsp_motion_abort(&core->motion, RBSP_MOTION_FAULT_ABORTED);
            stopped_motion = true;
        }
        core->motion_owner_session_id = 0U;
        core->motion_resource_mask = 0U;
    }
    core->hal.motion_exit_critical(critical_state);
    if (stopped_motion) {
        (void)rbsp_core_motion_service(core);
    }
    return stopped_motion;
}

static size_t expire_stepgen_leases(rbsp_core_t* core, uint32_t now_ms) {
    size_t expired = 0U;
    for (uint8_t axis = 0U; axis < core->motion.axis_count; ++axis) {
        rbsp_stepgen_lease_t* lease = &core->stepgen_leases[axis];
        if (!lease->active ||
            (int32_t)(now_ms - lease->expires_at_ms) < 0) {
            continue;
        }
        const uint32_t owner_session_id = lease->owner_session_id;
        memset(lease, 0, sizeof(*lease));
        (void)stop_for_stepgen_lease_loss(core, axis, owner_session_id);
        ++expired;
    }
    return expired;
}

static uint32_t stepgen_lease_remaining_ms(
    const rbsp_stepgen_lease_t* lease, uint32_t now_ms) {
    return lease != NULL && lease->active &&
                   (int32_t)(lease->expires_at_ms - now_ms) > 0
               ? lease->expires_at_ms - now_ms
               : 0U;
}

static void encode_stepgen_lease_info(
    uint8_t output[27U], uint32_t resource_id,
    const rbsp_stepgen_lease_t* lease, uint32_t requester_session_id,
    uint32_t now_ms, bool include_token) {
    memset(output, 0, 27U);
    put_u32(output, resource_id);
    if (lease == NULL || !lease->active) {
        return;
    }
    if (include_token && lease->owner_session_id == requester_session_id) {
        put_u64(output + 4U, lease->lease_id);
    }
    put_u32(output + 12U, lease->owner_session_id);
    put_u32(output + 16U, lease->granted_duration_ms);
    put_u32(output + 20U, stepgen_lease_remaining_ms(lease, now_ms));
    output[24U] = RBSP_RESOURCE_LEASE_EXCLUSIVE;
    put_u16(output + 25U, 1U);
}

enum {
    RBSP_MOTION_GROUP_IDENTITY_SIZE = 80,
    RBSP_MOTION_GROUP_RESULT_SIZE = 88,
};

static bool decode_motion_group_identity(
    const uint8_t* input, rbsp_motion_group_identity_t* identity) {
    if (get_u16(input) != 1U || get_u16(input + 2U) != 0U ||
        get_u32(input + 20U) != 0U) {
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    identity->transaction_id = get_u64(input + 4U);
    identity->group_id = get_u32(input + 12U);
    identity->plan_generation = get_u32(input + 16U);
    identity->boot_epoch = get_u64(input + 24U);
    identity->clock_model_generation = get_u64(input + 32U);
    identity->node_start_tick = get_u64(input + 40U);
    memcpy(identity->content_digest, input + 48U,
           RBSP_MOTION_GROUP_DIGEST_SIZE);
    uint8_t digest = 0U;
    for (uint8_t index = 0U; index < RBSP_MOTION_GROUP_DIGEST_SIZE;
         ++index) {
        digest |= identity->content_digest[index];
    }
    return identity->transaction_id != 0U && identity->group_id != 0U &&
           identity->plan_generation != 0U && identity->boot_epoch != 0U &&
           identity->clock_model_generation != 0U &&
           identity->node_start_tick != 0U && digest != 0U;
}

static void encode_motion_group_identity(
    uint8_t* output, const rbsp_motion_group_identity_t* identity) {
    memset(output, 0, RBSP_MOTION_GROUP_IDENTITY_SIZE);
    put_u16(output, 1U);
    put_u64(output + 4U, identity->transaction_id);
    put_u32(output + 12U, identity->group_id);
    put_u32(output + 16U, identity->plan_generation);
    put_u64(output + 24U, identity->boot_epoch);
    put_u64(output + 32U, identity->clock_model_generation);
    put_u64(output + 40U, identity->node_start_tick);
    memcpy(output + 48U, identity->content_digest,
           RBSP_MOTION_GROUP_DIGEST_SIZE);
}

static void encode_motion_group_result(
    uint8_t output[RBSP_MOTION_GROUP_RESULT_SIZE],
    const rbsp_motion_group_identity_t* identity, uint8_t code) {
    memset(output, 0, RBSP_MOTION_GROUP_RESULT_SIZE);
    encode_motion_group_identity(output, identity);
    output[RBSP_MOTION_GROUP_IDENTITY_SIZE] = code;
}

static bool decode_motion_group_segment(
    const rbsp_core_t* core, const uint8_t* input, uint16_t length,
    uint64_t node_start_tick, rbsp_motion_segment_t* segment,
    uint64_t* resource_mask) {
    if (length < 24U) {
        return false;
    }
    const uint8_t axis_count = input[21U];
    if (axis_count == 0U || axis_count > core->motion.axis_count ||
        length != (uint16_t)(24U + (uint16_t)axis_count * 8U) ||
        input[20U] > 1U || get_u16(input + 22U) != 0U) {
        return false;
    }
    if (resource_mask == NULL) {
        return false;
    }
    memset(segment, 0, sizeof(*segment));
    *resource_mask = 0U;
    segment->sequence = get_u32(input);
    segment->start_time_ns = node_start_tick;
    segment->duration_ns = get_u64(input + 12U);
    segment->final_segment = input[20U] != 0U;
    segment->axis_count = core->motion.axis_count;
    bool seen[CONFIG_MOTION_MAX_AXES];
    memset(seen, 0, sizeof(seen));
    for (uint8_t index = 0U; index < axis_count; ++index) {
        const uint8_t* entry = input + 24U + (uint16_t)index * 8U;
        const uint32_t resource_id = get_u32(entry);
        uint8_t axis = UINT8_MAX;
        for (uint8_t candidate = 0U;
             candidate < core->motion.axis_count; ++candidate) {
            if (motion_resource_id(core, candidate) == resource_id) {
                axis = candidate;
                break;
            }
        }
        if (axis == UINT8_MAX || seen[axis]) {
            return false;
        }
        seen[axis] = true;
        *resource_mask |= UINT64_C(1) << axis;
        segment->steps[axis] = (int32_t)get_u32(entry + 4U);
    }
    return segment->sequence != 0U && segment->duration_ns != 0U;
}
#endif

#if CONFIG_UART_RESOURCE_COUNT > 0
static bool uart_port_available(const rbsp_core_t* core, uint8_t port) {
    (void)core;
    return port < CONFIG_UART_RESOURCE_COUNT;
}

static bool uart_runtime_baud_allowed(const rbsp_core_t* core,
                                      uint8_t port,
                                      uint32_t baud_rate) {
    (void)core;
    (void)port;
    (void)baud_rate;
    return true;
}

static rbsp_uart_object_t* find_uart_object(rbsp_core_t* core,
                                             uint32_t object_id) {
    for (size_t index = 0; index < CONFIG_UART_RESOURCE_COUNT; ++index) {
        if (core->uart_objects[index].used &&
            core->uart_objects[index].object_id == object_id) {
            return &core->uart_objects[index];
        }
    }
    return NULL;
}
#endif

typedef struct {
    uint32_t resource_id;
    uint8_t type;
    uint16_t instance;
    uint16_t flags;
    uint32_t rx_capacity;
    uint32_t tx_capacity;
} rbsp_static_resource_descriptor_t;

typedef struct {
    uint8_t phase;
    uint16_t index;
} rbsp_static_resource_iterator_t;

#if defined(CONFIG_REMOTEBSP_ADC)
static bool adc_configuration_valid(const rbsp_hal_t* hal);
#endif
#if defined(CONFIG_REMOTEBSP_TIMER)
static bool timer_configuration_valid(const rbsp_hal_t* hal);
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
static bool storage_configuration_valid(const rbsp_hal_t* hal);
#endif

#if defined(CONFIG_REMOTEBSP_BUS)
static uint8_t bus_resource_type(uint8_t kind) {
    switch (kind) {
        case RBSP_BUS_I2C_BUS:
            return RBSP_RESOURCE_TYPE_I2C_BUS;
        case RBSP_BUS_I2C_DEVICE:
            return RBSP_RESOURCE_TYPE_I2C_DEVICE;
        case RBSP_BUS_SPI_BUS:
            return RBSP_RESOURCE_TYPE_SPI_BUS;
        case RBSP_BUS_SPI_DEVICE:
            return RBSP_RESOURCE_TYPE_SPI_DEVICE;
        default:
            return 0U;
    }
}
#endif

static bool next_static_resource(
    const rbsp_core_t* core, rbsp_static_resource_iterator_t* iterator,
    rbsp_static_resource_descriptor_t* descriptor) {
    if (core == NULL || iterator == NULL || descriptor == NULL) {
        return false;
    }
    for (;;) {
        switch (iterator->phase) {
            case 0U:
#if CONFIG_UART_RESOURCE_COUNT > 0
                if (iterator->index < CONFIG_UART_RESOURCE_COUNT) {
                    const uint16_t instance = iterator->index++;
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        0x02000000UL + instance,
                        RBSP_RESOURCE_TYPE_UART,
                        instance,
                        RBSP_RESOURCE_FLAG_NATIVE,
                        CONFIG_UART_RX_BUFFER_SIZE,
                        CONFIG_UART_TX_BUFFER_SIZE};
                    return true;
                }
#endif
                iterator->phase = 1U;
                iterator->index = 0U;
                break;
            case 1U:
#if defined(CONFIG_REMOTEBSP_PWM)
                if (iterator->index < CONFIG_PWM_RESOURCE_COUNT) {
                    const uint16_t instance = iterator->index++;
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        0x06000000UL + instance,
                        RBSP_RESOURCE_TYPE_PWM,
                        instance,
                        RBSP_RESOURCE_FLAG_NATIVE,
                        0U,
                        0U};
                    return true;
                }
#endif
                iterator->phase = 2U;
                iterator->index = 0U;
                break;
            case 2U:
#if defined(CONFIG_REMOTEBSP_MOTION)
                if (iterator->index < core->default_motion_axis_count) {
                    const uint16_t instance = iterator->index++;
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        motion_resource_id(core, (uint8_t)instance),
                        RBSP_RESOURCE_TYPE_STEPGEN_AXIS,
                        instance,
                        RBSP_RESOURCE_FLAG_NATIVE,
                        0U,
                        0U};
                    return true;
                }
#endif
                iterator->phase = 3U;
                iterator->index = 0U;
                break;
            case 3U:
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
                if (iterator->index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT) {
                    const uint16_t instance = iterator->index++;
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        0x0A000000UL + instance,
                        RBSP_RESOURCE_TYPE_TIMED_BITSTREAM,
                        instance,
                        RBSP_RESOURCE_FLAG_NATIVE,
                        0U,
                        (CONFIG_TIMED_BITSTREAM_MAX_BITS + 7U) / 8U};
                    return true;
                }
#endif
                iterator->phase = 4U;
                iterator->index = 0U;
                break;
            case 4U:
#if defined(CONFIG_REMOTEBSP_ADC)
                if (adc_configuration_valid(&core->hal) &&
                    iterator->index < core->hal.adc_resource_count) {
                    const rbsp_adc_resource_config_t* item =
                        &core->hal.adc_resources[iterator->index++];
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        item->resource_id, RBSP_RESOURCE_TYPE_ADC,
                        item->instance, RBSP_RESOURCE_FLAG_NATIVE,
                        item->maximum_batch_samples * 2U, 0U};
                    return true;
                }
#endif
                iterator->phase = 5U;
                iterator->index = 0U;
                break;
            case 5U:
#if defined(CONFIG_REMOTEBSP_STORAGE)
                if (storage_configuration_valid(&core->hal) &&
                    iterator->index < core->hal.storage_resource_count) {
                    const rbsp_storage_resource_config_t* item =
                        &core->hal.storage_resources[iterator->index++];
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        item->resource_id, RBSP_RESOURCE_TYPE_STORAGE,
                        item->instance, RBSP_RESOURCE_FLAG_NATIVE,
                        item->maximum_transfer_bytes,
                        item->maximum_transfer_bytes};
                    return true;
                }
#endif
                iterator->phase = 6U;
                iterator->index = 0U;
                break;
            case 6U:
#if defined(CONFIG_REMOTEBSP_TIMER)
                if (timer_configuration_valid(&core->hal) &&
                    iterator->index < core->hal.timer_resource_count) {
                    const rbsp_timer_resource_config_t* item =
                        &core->hal.timer_resources[iterator->index++];
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        item->resource_id, RBSP_RESOURCE_TYPE_TIMER,
                        item->instance, RBSP_RESOURCE_FLAG_NATIVE, 0U, 0U};
                    return true;
                }
#endif
                iterator->phase = 7U;
                iterator->index = 0U;
                break;
            case 7U:
#if defined(CONFIG_REMOTEBSP_BUS)
                if (iterator->index < core->hal.bus_resource_count) {
                    const rbsp_bus_resource_config_t* item =
                        &core->hal.bus_resources[iterator->index++];
                    const uint8_t type = bus_resource_type(item->kind);
                    const bool device =
                        type == RBSP_RESOURCE_TYPE_I2C_DEVICE ||
                        type == RBSP_RESOURCE_TYPE_SPI_DEVICE;
                    *descriptor = (rbsp_static_resource_descriptor_t){
                        item->resource_id,
                        type,
                        (uint16_t)item->resource_id,
                        RBSP_RESOURCE_FLAG_NATIVE,
                        device ? item->maximum_transfer_bytes : 0U,
                        device ? item->maximum_transfer_bytes : 0U};
                    return true;
                }
#endif
                iterator->phase = 8U;
                iterator->index = 0U;
                break;
            default:
                return false;
        }
    }
}

static void encode_resource_descriptor(
    uint8_t output[RBSP_RESOURCE_DESCRIPTOR_SIZE],
    const rbsp_static_resource_descriptor_t* descriptor) {
    put_u32(output, descriptor->resource_id);
    output[4U] = descriptor->type;
    put_u16(output + 5U, descriptor->instance);
    put_u16(output + 7U, descriptor->flags);
    put_u32(output + 9U, descriptor->rx_capacity);
    put_u32(output + 13U, descriptor->tx_capacity);
}

static bool find_static_resource(
    const rbsp_core_t* core, uint32_t resource_id,
    rbsp_static_resource_descriptor_t* descriptor) {
    rbsp_static_resource_iterator_t iterator = {0U, 0U};
    rbsp_static_resource_descriptor_t candidate;
    while (next_static_resource(core, &iterator, &candidate)) {
        if (candidate.resource_id == resource_id) {
            *descriptor = candidate;
            return true;
        }
    }
    return false;
}

#if defined(CONFIG_REMOTEBSP_ADC)
static bool adc_configuration_valid(const rbsp_hal_t* hal) {
    if (hal->adc_resources == NULL || hal->adc_sample == NULL ||
        hal->adc_resource_count == 0U ||
        hal->adc_resource_count > CONFIG_ADC_RESOURCE_COUNT) return false;
    for (size_t index = 0U; index < hal->adc_resource_count; ++index) {
        const rbsp_adc_resource_config_t* item = &hal->adc_resources[index];
        if ((item->resource_id >> 24U) != RBSP_RESOURCE_TYPE_ADC ||
            item->resolution_bits < 6U || item->resolution_bits > 16U ||
            item->maximum_sample_rate_hz == 0U ||
            item->maximum_sample_rate_hz > 1000000U ||
            item->reference_mv == 0U || item->maximum_batch_samples == 0U ||
            item->maximum_batch_samples > 32U) return false;
        for (size_t previous = 0U; previous < index; ++previous) {
            if (hal->adc_resources[previous].resource_id == item->resource_id ||
                hal->adc_resources[previous].instance == item->instance) {
                return false;
            }
        }
    }
    return true;
}

static const rbsp_adc_resource_config_t* find_adc_resource(
    const rbsp_core_t* core, uint32_t resource_id, size_t* index_out) {
    if (!adc_configuration_valid(&core->hal)) {
        return NULL;
    }
    for (size_t index = 0U; index < core->hal.adc_resource_count; ++index) {
        if (core->hal.adc_resources[index].resource_id == resource_id) {
            if (index_out != NULL) *index_out = index;
            return &core->hal.adc_resources[index];
        }
    }
    return NULL;
}
#endif

#if defined(CONFIG_REMOTEBSP_TIMER)
static bool timer_configuration_valid(const rbsp_hal_t* hal) {
    const uint16_t known_capabilities = 0x0007U;
    if (hal->timer_resources == NULL || hal->timer_execute == NULL ||
        hal->timer_resource_count == 0U ||
        hal->timer_resource_count > CONFIG_TIMER_RESOURCE_COUNT) return false;
    for (size_t index = 0U; index < hal->timer_resource_count; ++index) {
        const rbsp_timer_resource_config_t* item = &hal->timer_resources[index];
        if ((item->resource_id >> 24U) != RBSP_RESOURCE_TYPE_TIMER ||
            item->tick_hz == 0U || item->maximum_operation_us == 0U ||
            item->maximum_operation_us > 1000000U || item->capabilities == 0U ||
            (item->capabilities & (uint16_t)~known_capabilities) != 0U) return false;
        for (size_t previous = 0U; previous < index; ++previous) {
            if (hal->timer_resources[previous].resource_id == item->resource_id ||
                hal->timer_resources[previous].instance == item->instance) return false;
        }
    }
    return true;
}

static const rbsp_timer_resource_config_t* find_timer_resource(
    const rbsp_core_t* core, uint32_t resource_id, size_t* index_out) {
    if (!timer_configuration_valid(&core->hal)) return NULL;
    for (size_t index = 0U; index < core->hal.timer_resource_count; ++index) {
        if (core->hal.timer_resources[index].resource_id == resource_id) {
            if (index_out != NULL) *index_out = index;
            return &core->hal.timer_resources[index];
        }
    }
    return NULL;
}
#endif

#if defined(CONFIG_REMOTEBSP_STORAGE)
static bool storage_configuration_valid(const rbsp_hal_t* hal) {
    if (hal->storage_resources == NULL || hal->storage_read == NULL ||
        hal->storage_erase == NULL || hal->storage_program == NULL ||
        hal->storage_resource_count == 0U ||
        hal->storage_resource_count > CONFIG_STORAGE_RESOURCE_COUNT) {
        return false;
    }
    for (size_t index = 0U; index < hal->storage_resource_count; ++index) {
        const rbsp_storage_resource_config_t* item =
            &hal->storage_resources[index];
        if ((item->resource_id >> 24U) != RBSP_RESOURCE_TYPE_STORAGE ||
            item->capacity_bytes == 0U || item->erase_block_bytes == 0U ||
            item->write_alignment_bytes == 0U ||
            item->maximum_transfer_bytes == 0U ||
            item->maximum_transfer_bytes > 1024U ||
            item->maximum_transfer_bytes >
                CONFIG_REMOTE_MAX_PACKET_SIZE - RBSP_HEADER_SIZE - 16U ||
            item->maximum_transfer_bytes < item->erase_block_bytes ||
            item->capacity_bytes % item->erase_block_bytes != 0U ||
            item->erase_block_bytes % item->write_alignment_bytes != 0U ||
            (item->flags & ~1U) != 0U) {
            return false;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (hal->storage_resources[previous].resource_id ==
                    item->resource_id ||
                hal->storage_resources[previous].instance == item->instance) {
                return false;
            }
        }
    }
    return true;
}

static const rbsp_storage_resource_config_t* find_storage_resource(
    const rbsp_core_t* core, uint32_t resource_id, size_t* index_out) {
    if (!storage_configuration_valid(&core->hal)) return NULL;
    for (size_t index = 0U; index < core->hal.storage_resource_count; ++index) {
        if (core->hal.storage_resources[index].resource_id == resource_id) {
            if (index_out != NULL) *index_out = index;
            return &core->hal.storage_resources[index];
        }
    }
    return NULL;
}
#endif

#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
static rbsp_static_resource_lease_t* find_static_resource_lease(
    rbsp_core_t* core, uint32_t resource_id, uint16_t* access_out) {
#if defined(CONFIG_REMOTEBSP_TIMER)
    size_t timer_index = 0U;
    if (find_timer_resource(core, resource_id, &timer_index) != NULL) {
        if (access_out != NULL) {
            *access_out = RBSP_RESOURCE_ACCESS_READABLE |
                          RBSP_RESOURCE_ACCESS_WRITABLE |
                          RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE |
                          RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED |
                          RBSP_RESOURCE_ACCESS_LEASE_REQUIRED;
        }
        return &core->timer_leases[timer_index];
    }
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
    size_t storage_index = 0U;
    if (find_storage_resource(core, resource_id, &storage_index) != NULL) {
        if (access_out != NULL) {
            *access_out = RBSP_RESOURCE_ACCESS_READABLE |
                          RBSP_RESOURCE_ACCESS_WRITABLE |
                          RBSP_RESOURCE_ACCESS_SHARED_READ |
                          RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE |
                          RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED |
                          RBSP_RESOURCE_ACCESS_LEASE_REQUIRED;
        }
        return &core->storage_leases[storage_index];
    }
#endif
    return NULL;
}

static uint32_t static_lease_remaining_ms(
    const rbsp_static_resource_lease_t* lease, uint32_t now_ms) {
    return lease->active && (int32_t)(lease->expires_at_ms - now_ms) > 0
               ? lease->expires_at_ms - now_ms : 0U;
}

static void expire_static_resource_leases(rbsp_core_t* core, uint32_t now_ms) {
#define RBSP_EXPIRE_STATIC_LEASES(array, count)                               \
    do {                                                                      \
        for (size_t i = 0U; i < (count); ++i) {                              \
            if ((array)[i].active &&                                          \
                (int32_t)(now_ms - (array)[i].expires_at_ms) >= 0)            \
                memset(&(array)[i], 0, sizeof((array)[i]));                   \
        }                                                                     \
    } while (0)
#if defined(CONFIG_REMOTEBSP_TIMER)
    RBSP_EXPIRE_STATIC_LEASES(core->timer_leases, core->hal.timer_resource_count);
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
    RBSP_EXPIRE_STATIC_LEASES(core->storage_leases, core->hal.storage_resource_count);
#endif
#undef RBSP_EXPIRE_STATIC_LEASES
}

static bool static_resource_lease_allows(
    rbsp_core_t* core, uint32_t resource_id, uint32_t session_id,
    uint8_t required_mode) {
    expire_static_resource_leases(core, core->hal.milliseconds());
    rbsp_static_resource_lease_t* lease =
        find_static_resource_lease(core, resource_id, NULL);
    return lease != NULL && lease->active && session_id != 0U &&
           lease->owner_session_id == session_id &&
           (required_mode == RBSP_RESOURCE_LEASE_SHARED ||
            lease->access_mode == RBSP_RESOURCE_LEASE_EXCLUSIVE);
}

static void encode_static_lease_info(
    uint8_t output[27U], uint32_t resource_id,
    const rbsp_static_resource_lease_t* lease, uint32_t requester,
    uint32_t now_ms, bool include_token) {
    memset(output, 0, 27U); put_u32(output, resource_id);
    if (!lease->active) return;
    if (include_token && lease->owner_session_id == requester)
        put_u64(output + 4U, lease->lease_id);
    put_u32(output + 12U, lease->owner_session_id);
    put_u32(output + 16U, lease->granted_duration_ms);
    put_u32(output + 20U, static_lease_remaining_ms(lease, now_ms));
    output[24U] = lease->access_mode;
    put_u16(output + 25U, 1U);
}

static bool handle_static_resource_lease_command(
    rbsp_core_t* core, const rbsp_request_t* request, uint16_t* response_size) {
    uint16_t length = request->command == RBSP_COMMAND_RESOURCE_ACQUIRE ? 9U :
        (request->command == RBSP_COMMAND_RESOURCE_RENEW ||
         request->command == RBSP_COMMAND_RESOURCE_RELEASE) ? 16U : 4U;
    if (request->payload_length != length) return false;
    const uint32_t resource_id = get_u32(request->payload);
    uint16_t access = 0U;
    rbsp_static_resource_lease_t* lease =
        find_static_resource_lease(core, resource_id, &access);
    if (lease == NULL) return false;
    if (request->object_id != 0U) {
        *response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U); return true;
    }
    const uint32_t now_ms = core->hal.milliseconds();
    expire_static_resource_leases(core, now_ms);
    if (request->command == RBSP_COMMAND_RESOURCE_CONTRACT) {
        uint8_t data[32U] = {0U}; put_u32(data, resource_id);
        put_u16(data + 4U, 1U); put_u16(data + 6U, access);
        *response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data)); return true;
    }
    if (request->command == RBSP_COMMAND_RESOURCE_LEASE_STATUS) {
        uint8_t data[27U]; encode_static_lease_info(data, resource_id, lease, request->session_id, now_ms, false);
        *response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data)); return true;
    }
    if (request->session_id == 0U) {
        *response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U); return true;
    }
    if (request->command == RBSP_COMMAND_RESOURCE_ACQUIRE) {
        const uint32_t duration = get_u32(request->payload + 4U);
        const uint8_t mode = request->payload[8U];
        const bool shared_allowed = (access & RBSP_RESOURCE_ACCESS_SHARED_READ) != 0U;
        if (duration < RBSP_RESOURCE_LEASE_MINIMUM_MS || duration > RBSP_RESOURCE_LEASE_MAXIMUM_MS ||
            (mode != RBSP_RESOURCE_LEASE_EXCLUSIVE && !(mode == RBSP_RESOURCE_LEASE_SHARED && shared_allowed))) {
            *response_size = make_status_response(core, request,
                mode != RBSP_RESOURCE_LEASE_SHARED && mode != RBSP_RESOURCE_LEASE_EXCLUSIVE ? RBSP_STATUS_INVALID_PAYLOAD : RBSP_STATUS_ACCESS_DENIED,
                0U, NULL, 0U); return true;
        }
        if (lease->active) {
            *response_size = make_status_response(core, request, RBSP_STATUS_RESOURCE_BUSY, 0U, NULL, 0U); return true;
        }
        lease->active = true; lease->lease_id = core->next_static_resource_lease_id++;
        if (core->next_static_resource_lease_id == 0U) core->next_static_resource_lease_id = 1U;
        lease->owner_session_id = request->session_id; lease->granted_duration_ms = duration;
        lease->expires_at_ms = now_ms + duration; lease->access_mode = mode;
        uint8_t data[27U]; encode_static_lease_info(data, resource_id, lease, request->session_id, now_ms, true);
        *response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data)); return true;
    }
    const uint64_t lease_id = get_u64(request->payload + 4U);
    const uint32_t duration = get_u32(request->payload + 12U);
    const bool release = request->command == RBSP_COMMAND_RESOURCE_RELEASE;
    if (lease_id == 0U || (release ? duration != 0U : duration < RBSP_RESOURCE_LEASE_MINIMUM_MS || duration > RBSP_RESOURCE_LEASE_MAXIMUM_MS))
        *response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
    else if (!lease->active || lease->lease_id != lease_id || lease->owner_session_id != request->session_id)
        *response_size = make_status_response(core, request, RBSP_STATUS_ACCESS_DENIED, 0U, NULL, 0U);
    else if (release) { memset(lease, 0, sizeof(*lease)); *response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, NULL, 0U); }
    else { lease->granted_duration_ms = duration; lease->expires_at_ms = now_ms + duration;
        uint8_t data[27U]; encode_static_lease_info(data, resource_id, lease, request->session_id, now_ms, true);
        *response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data)); }
    return true;
}
#endif

static uint32_t saturating_add_u32(uint32_t left, uint32_t right) {
    return UINT32_MAX - left < right ? UINT32_MAX : left + right;
}

static rbsp_core_resource_counters_t* resource_counters(
    rbsp_core_t* core, const rbsp_static_resource_descriptor_t* descriptor) {
    const uint16_t instance = descriptor->instance;
    (void)core;
    (void)instance;
#if defined(CONFIG_REMOTEBSP_BUS)
    if (descriptor->type == RBSP_RESOURCE_TYPE_I2C_BUS ||
        descriptor->type == RBSP_RESOURCE_TYPE_I2C_DEVICE ||
        descriptor->type == RBSP_RESOURCE_TYPE_SPI_BUS ||
        descriptor->type == RBSP_RESOURCE_TYPE_SPI_DEVICE) {
        size_t resource_index = 0U;
        if (find_bus_resource(core, descriptor->resource_id,
                              &resource_index) != NULL &&
            resource_index < CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT) {
            return &core->bus_status[resource_index];
        }
    }
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
    if (descriptor->type == RBSP_RESOURCE_TYPE_UART &&
        instance < CONFIG_UART_RESOURCE_COUNT) {
        return &core->uart_status[instance];
    }
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    if (descriptor->type == RBSP_RESOURCE_TYPE_PWM &&
        instance < CONFIG_PWM_RESOURCE_COUNT) {
        return &core->pwm_status[instance];
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    if (descriptor->type == RBSP_RESOURCE_TYPE_TIMED_BITSTREAM &&
        instance < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT) {
        return &core->timed_bitstream_status[instance];
    }
#endif
#if defined(CONFIG_REMOTEBSP_ADC)
    if (descriptor->type == RBSP_RESOURCE_TYPE_ADC) {
        for (size_t index = 0U; index < core->hal.adc_resource_count; ++index) {
            if (core->hal.adc_resources[index].resource_id ==
                descriptor->resource_id && index < CONFIG_ADC_RESOURCE_COUNT) {
                return &core->adc_status[index];
            }
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMER)
    if (descriptor->type == RBSP_RESOURCE_TYPE_TIMER) {
        for (size_t index = 0U; index < core->hal.timer_resource_count; ++index) {
            if (core->hal.timer_resources[index].resource_id == descriptor->resource_id &&
                index < CONFIG_TIMER_RESOURCE_COUNT) return &core->timer_status[index];
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
    if (descriptor->type == RBSP_RESOURCE_TYPE_STORAGE) {
        for (size_t index = 0U; index < core->hal.storage_resource_count;
             ++index) {
            if (core->hal.storage_resources[index].resource_id ==
                    descriptor->resource_id &&
                index < CONFIG_STORAGE_RESOURCE_COUNT) {
                return &core->storage_status[index];
            }
        }
    }
#endif
    return NULL;
}

static void encode_resource_runtime_status(
    rbsp_core_t* core, const rbsp_static_resource_descriptor_t* descriptor,
    uint8_t output[RBSP_RESOURCE_STATUS_SIZE]) {
    rbsp_resource_runtime_status_t status = {0U};
    if (core->hal.resource_status != NULL) {
        (void)core->hal.resource_status(
            descriptor->type, descriptor->instance, &status);
    }
    rbsp_core_resource_counters_t* const counters =
        resource_counters(core, descriptor);
    if (counters != NULL) {
        status.rx_overruns = saturating_add_u32(
            status.rx_overruns, counters->rx_overruns);
        status.tx_overruns = saturating_add_u32(
            status.tx_overruns, counters->tx_overruns);
        status.backend_failed =
            status.backend_failed || counters->backend_failed;
    }
#if CONFIG_UART_RESOURCE_COUNT > 0
    if (descriptor->type == RBSP_RESOURCE_TYPE_UART) {
        for (size_t index = 0U; index < CONFIG_UART_RESOURCE_COUNT; ++index) {
            const rbsp_uart_object_t* const object = &core->uart_objects[index];
            if (object->used && object->port == descriptor->instance) {
                status.busy = true;
                status.rx_buffered = saturating_add_u32(
                    status.rx_buffered, object->pending_length);
                break;
            }
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    if (descriptor->type == RBSP_RESOURCE_TYPE_PWM) {
        for (size_t index = 0U; index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
            if (core->pwm_objects[index].used &&
                core->pwm_objects[index].channel == descriptor->instance) {
                status.busy = true;
                break;
            }
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    if (descriptor->type == RBSP_RESOURCE_TYPE_TIMED_BITSTREAM) {
        if (core->hal.timed_bitstream_busy != NULL &&
            core->hal.timed_bitstream_busy((uint8_t)descriptor->instance)) {
            status.busy = true;
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
    if (descriptor->type == RBSP_RESOURCE_TYPE_TIMER ||
        descriptor->type == RBSP_RESOURCE_TYPE_STORAGE) {
        rbsp_static_resource_lease_t* lease = find_static_resource_lease(
            core, descriptor->resource_id, NULL);
        if (lease != NULL && lease->active) status.busy = true;
    }
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    if (descriptor->type == RBSP_RESOURCE_TYPE_I2C_DEVICE ||
        descriptor->type == RBSP_RESOURCE_TYPE_SPI_DEVICE) {
        size_t resource_index = 0U;
        const rbsp_bus_resource_config_t* const resource =
            find_bus_resource(core, descriptor->resource_id,
                              &resource_index);
        if (resource != NULL &&
            resource_index < core->hal.bus_resource_count &&
            core->bus_leases[resource_index].active) {
            /*
             * 总线事务是同步原子操作，执行中的瞬态无法被另一个请求观测；
             * 设备持有独占租约才是主机可观测、可操作的 Busy 状态。
             */
            status.busy = true;
        }
    }
#endif
    uint32_t errors = 0U;
    if (status.rx_overruns != 0U) {
        errors |= RBSP_RESOURCE_ERROR_RX_OVERFLOW;
    }
    if (status.tx_overruns != 0U) {
        errors |= RBSP_RESOURCE_ERROR_TX_OVERFLOW;
    }
    if (status.backend_failed) {
        errors |= RBSP_RESOURCE_ERROR_BACKEND_FAILURE;
    }
    const uint8_t health = status.backend_failed
                               ? RBSP_RESOURCE_HEALTH_FAILED
                               : errors != 0U
                                     ? RBSP_RESOURCE_HEALTH_DEGRADED
                                     : status.busy
                                           ? RBSP_RESOURCE_HEALTH_BUSY
                                           : RBSP_RESOURCE_HEALTH_NORMAL;
    memset(output, 0, RBSP_RESOURCE_STATUS_SIZE);
    put_u32(output, descriptor->resource_id);
    output[4U] = health;
    put_u32(output + 5U, errors);
    put_u32(output + 9U, status.rx_buffered);
    put_u32(output + 13U, status.tx_buffered);
    put_u32(output + 17U, status.rx_overruns);
    put_u32(output + 21U, status.tx_overruns);
}

static bool basic_resource_contract(
    rbsp_core_t* core, const rbsp_request_t* request,
    uint16_t* response_size) {
    if (request->object_id != 0U || request->payload_length != 4U) {
        return false;
    }
    rbsp_static_resource_descriptor_t descriptor;
    if (!find_static_resource(
            core, get_u32(request->payload), &descriptor) ||
        descriptor.type == RBSP_RESOURCE_TYPE_STEPGEN_AXIS ||
        descriptor.type >= RBSP_RESOURCE_TYPE_I2C_BUS) {
        return false;
    }
    uint16_t access = 0U;
    if (descriptor.type == RBSP_RESOURCE_TYPE_UART) {
        access = RBSP_RESOURCE_ACCESS_READABLE |
                 RBSP_RESOURCE_ACCESS_WRITABLE;
    } else if (descriptor.type == RBSP_RESOURCE_TYPE_ADC) {
        access = RBSP_RESOURCE_ACCESS_READABLE |
                 RBSP_RESOURCE_ACCESS_SHARED_READ |
                 RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED;
    } else if (descriptor.type == RBSP_RESOURCE_TYPE_STORAGE) {
        access = RBSP_RESOURCE_ACCESS_READABLE |
                 RBSP_RESOURCE_ACCESS_WRITABLE |
                 RBSP_RESOURCE_ACCESS_SHARED_READ |
                 RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE |
                 RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED;
    } else {
        access = RBSP_RESOURCE_ACCESS_WRITABLE |
                 RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE;
    }
    uint8_t data[RBSP_RESOURCE_CONTRACT_SIZE] = {0U};
    put_u32(data, descriptor.resource_id);
    put_u16(data + 4U, 1U);
    put_u16(data + 6U, access);
    /* 当前实体后端没有独立操作队列合同；0 表示尚未声明。 */
    put_u32(data + 20U, 0U);
    *response_size = make_status_response(
        core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
    return true;
}

static bool process_request(rbsp_core_t* core,
                            const rbsp_request_t* request) {
#if defined(CONFIG_REMOTEBSP_MOTION)
    (void)expire_stepgen_leases(core, core->hal.milliseconds());
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    (void)expire_bus_leases(core, core->hal.milliseconds());
#endif
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
    expire_static_resource_leases(core, core->hal.milliseconds());
#endif
    rbsp_request_cache_entry_t* cached =
        find_cache(core, request);
    if (cached != NULL) {
        if (!same_cached_request(cached, request)) {
            return false;
        }
        return send_packet(core, cached->response,
                           cached->response_size,
                           allocate_transfer_id(core),
                           response_can_id(core));
    }

    uint16_t response_size = 0U;
    uint8_t* payload = core->tx_packet + RBSP_HEADER_SIZE;
    bool request_bootloader = false;
    rbsp_bootloader_mode_t bootloader_mode = RBSP_BOOTLOADER_CAN;

    switch (request->command) {
        case RBSP_COMMAND_DISCOVERY_REQUEST:
            if (request->payload_length != 2U ||
                request->payload[0] == 0U ||
                request->payload[0] > request->payload[1] ||
                RBSP_PROTOCOL_VERSION < request->payload[0] ||
                RBSP_PROTOCOL_VERSION > request->payload[1]) {
                return false;
            }
            memcpy(payload, core->info.uuid, 16U);
            put_u16(payload + 16U, core->info.firmware_major);
            put_u16(payload + 18U, core->info.firmware_minor);
            put_u16(payload + 20U, core->info.firmware_patch);
            put_u32(payload + 22U, core->info.board_type);
            payload[26] = RBSP_PROTOCOL_VERSION;
            response_size = encode_packet(
                core->tx_packet, RBSP_MESSAGE_RESPONSE,
                RBSP_COMMAND_DISCOVERY_RESPONSE,
                request->session_id, request->request_id, 0U, 0U,
                payload, 27U);
            break;

        case RBSP_COMMAND_NODE_ASSIGN:
            if (request->payload_length != 20U ||
                memcmp(request->payload, core->info.uuid, 16U) != 0 ||
                get_u32(request->payload + 16U) == 0U ||
                get_u32(request->payload + 16U) > 127U) {
                return false;
            }
            core->node_id = get_u32(request->payload + 16U);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, core->node_id, NULL, 0U);
            break;

#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
        case RBSP_COMMAND_DEVICE_PARAMETER_STATUS: {
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                uint8_t data[RBSP_DEVICE_PARAM_STATUS_SIZE];
                encode_device_param_status(
                    core, request->session_id, core->hal.milliseconds(), data);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_DEVICE_PARAMETER_LIST: {
            const size_t count = rbsp_device_param_definition_count();
            const size_t data_length = 4U + count * 8U;
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 0U ||
                       data_length + 1U >
                           CONFIG_REMOTE_MAX_PACKET_SIZE - RBSP_HEADER_SIZE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                uint8_t* data = payload + 1U;
                bool valid = true;
                put_u16(data, RBSP_DEVICE_PARAM_PROTOCOL_VERSION);
                put_u16(data + 2U, (uint16_t)count);
                for (size_t index = 0U; index < count; ++index) {
                    rbsp_device_param_definition definition;
                    uint8_t* item = data + 4U + index * 8U;
                    if (!rbsp_device_param_definition_at(
                            index, &definition)) {
                        valid = false;
                        break;
                    }
                    put_u16(item, definition.id);
                    item[2U] = definition.type;
                    item[3U] = definition.flags;
                    put_u16(item + 4U, definition.minimum_length);
                    put_u16(item + 6U, definition.maximum_length);
                }
                response_size = make_status_response(
                    core, request,
                    valid ? RBSP_STATUS_OK : RBSP_STATUS_RESOURCE_FAILED,
                    0U, valid ? data : NULL,
                    valid ? (uint16_t)data_length : 0U);
            }
            break;
        }

        case RBSP_COMMAND_DEVICE_PARAMETER_READ: {
            rbsp_device_param_record record;
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 2U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!rbsp_device_param_store_get(
                           &core->device_params,
                           get_u16(request->payload), &record)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t* data = payload + 1U;
                put_u16(data, record.id);
                data[2U] = record.type;
                data[3U] = record.flags;
                put_u32(data + 4U, core->device_params.generation);
                put_u16(data + 8U, record.length);
                put_u16(data + 10U, 0U);
                memcpy(data + 12U, record.value, record.length);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, data,
                    (uint16_t)(12U + record.length));
            }
            break;
        }

        case RBSP_COMMAND_DEVICE_PARAMETER_UNLOCK: {
            const uint32_t now_ms = core->hal.milliseconds();
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 8U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (request->session_id == 0U ||
                       get_u32(request->payload) !=
                           core->device_params.generation ||
                       get_u32(request->payload + 4U) !=
                           RBSP_DEVICE_PARAM_UNLOCK_CONFIRMATION) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else {
                core->device_param_unlock_session = request->session_id;
                core->device_param_unlock_token =
                    UINT32_C(0xD15EA5E5) ^ request->session_id ^
                    core->device_params.generation;
                if (core->device_param_unlock_token == 0U) {
                    core->device_param_unlock_token = 1U;
                }
                core->device_param_unlock_expires_ms =
                    now_ms + RBSP_DEVICE_PARAM_UNLOCK_TIME_MS;
                put_u32(payload + 1U, core->device_params.generation);
                put_u32(payload + 5U,
                        core->device_param_unlock_token);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    payload + 1U, 8U);
            }
            break;
        }

        case RBSP_COMMAND_DEVICE_PARAMETER_WRITE: {
            const uint32_t now_ms = core->hal.milliseconds();
            const uint16_t length = request->payload_length >= 12U
                ? get_u16(request->payload + 10U)
                : 0U;
            rbsp_device_param_definition definition;
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length < 12U ||
                       length > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE ||
                       request->payload_length != (uint16_t)(12U + length)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!device_param_unlock_active(
                           core, request->session_id, now_ms) ||
                       get_u32(request->payload + 4U) !=
                           core->device_param_unlock_token) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else if (!rbsp_device_param_find_definition(
                           get_u16(request->payload + 8U), &definition) ||
                       !rbsp_device_param_validate_value(
                           get_u16(request->payload + 8U),
                           length == 0U ? NULL : request->payload + 12U,
                           length)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!rbsp_device_param_store_set(
                           &core->device_params,
                           get_u32(request->payload),
                           get_u16(request->payload + 8U),
                           length == 0U ? NULL : request->payload + 12U,
                           length, core->tx_packet,
                           sizeof(core->tx_packet))) {
                response_size = make_status_response(
                    core, request,
                    core->device_params.last_error ==
                            RBSP_DEVICE_PARAM_STORE_ERROR_WRITE_ONCE
                        ? RBSP_STATUS_ACCESS_DENIED
                        : RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                uint8_t data[RBSP_DEVICE_PARAM_STATUS_SIZE];
                if ((definition.flags &
                     RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART) != 0U) {
                    core->device_param_restart_required = true;
                }
                encode_device_param_status(
                    core, request->session_id, now_ms, data);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_DEVICE_PARAMETER_LOCK:
            if (!core->device_params_ready) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                if (core->device_param_unlock_session ==
                    request->session_id) {
                    core->device_param_unlock_session = 0U;
                    core->device_param_unlock_token = 0U;
                    core->device_param_unlock_expires_ms = 0U;
                }
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
            }
            break;
#endif


        case RBSP_COMMAND_GET_INFO:
            if (request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
                break;
            }
            memcpy(payload + 1U, core->info.uuid, 16U);
            put_u16(payload + 17U, core->info.firmware_major);
            put_u16(payload + 19U, core->info.firmware_minor);
            put_u16(payload + 21U, core->info.firmware_patch);
            put_u32(payload + 23U, core->info.board_type);
            payload[27] = RBSP_PROTOCOL_VERSION;
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, request->object_id,
                payload + 1U, 27U);
            break;

        case RBSP_COMMAND_GET_CAPABILITY: {
            if (request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
                break;
            }
            uint64_t capabilities = 0U;
            if ((core->hal.gpio_configure != NULL ||
                 core->hal.gpio_configure_pull != NULL) &&
                core->hal.gpio_read != NULL &&
                core->hal.gpio_write != NULL) {
                capabilities |= 1ULL << 0U;
            }
#if CONFIG_UART_RESOURCE_COUNT > 0
            if (core->hal.uart_configure != NULL &&
                core->hal.uart_read != NULL &&
                core->hal.uart_write != NULL) {
                capabilities |= 1ULL << 1U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (bus_has_device_kind(core, RBSP_BUS_SPI_DEVICE)) {
                capabilities |= 1ULL << 2U;
            }
            if (bus_has_device_kind(core, RBSP_BUS_I2C_DEVICE)) {
                capabilities |= 1ULL << 3U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_ADC)
            if (adc_configuration_valid(&core->hal)) {
                capabilities |= 1ULL << 4U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_TIMER)
            if (timer_configuration_valid(&core->hal)) {
                capabilities |= 1ULL << 6U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
            if (storage_configuration_valid(&core->hal)) {
                capabilities |= 1ULL << 7U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
            if (core->hal.pwm_configure != NULL &&
                core->hal.pwm_write != NULL &&
                core->hal.pwm_stop != NULL) {
                capabilities |= 1ULL << 5U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
            if (core->hal.timed_bitstream_configure != NULL &&
                core->hal.timed_bitstream_write != NULL &&
                core->hal.timed_bitstream_abort != NULL) {
                capabilities |= 1ULL << 10U;
            }
#endif
            if (core->hal.enter_bootloader != NULL) {
                capabilities |= 1ULL << 8U;
            }
#if defined(CONFIG_REMOTEBSP_MOTION)
            if (motion_available(core)) {
                capabilities |= 1ULL << 9U;
            }
#endif
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
            if (core->device_params_ready) {
                capabilities |= 1ULL << 12U;
            }
#endif
            put_u64(payload + 1U, capabilities);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, request->object_id,
                payload + 1U, 8U);
            break;
        }

        case RBSP_COMMAND_PING:
            if ((uint32_t)request->payload_length + 1U >
                CONFIG_REMOTE_MAX_PACKET_SIZE - RBSP_HEADER_SIZE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, request->object_id,
                    request->payload, request->payload_length);
            }
            break;

        case RBSP_COMMAND_FIRMWARE_IDENTITY:
            if (request->object_id != 0U || request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
                break;
            }
            put_u16(payload + 1U, RBSP_FIRMWARE_IDENTITY_SCHEMA_VERSION);
            put_u16(payload + 3U,
                    core->info.firmware_identity_available_fields);
            put_u32(payload + 5U, core->info.board_type);
            memcpy(payload + 9U, core->info.uuid, 16U);
            memcpy(payload + 25U, core->info.project_sha256, 32U);
            memcpy(payload + 57U, core->info.config_sha256, 32U);
            memcpy(payload + 89U, core->info.firmware_input_sha256, 32U);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, payload + 1U, 120U);
            break;

        case RBSP_COMMAND_HEALTH_SNAPSHOT: {
            rbsp_mcu_health_sample_t sample;
            memset(&sample, 0, sizeof(sample));
            if (request->object_id != 0U || request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
                break;
            }
            if (core->hal.health_sample == NULL ||
                !core->hal.health_sample(&sample) ||
                sample.producer_generation == 0U ||
                (sample.available_fields & ~UINT32_C(0x0F)) != 0U ||
                (((sample.available_fields &
                   RBSP_MCU_HEALTH_CPU_LOAD_AVAILABLE) != 0U) &&
                 sample.cpu_load_permille > 1000U) ||
                (((sample.available_fields &
                   RBSP_MCU_HEALTH_ISR_LOAD_AVAILABLE) != 0U) &&
                 sample.isr_load_permille > 1000U) ||
                (core->health_producer_generation != 0U &&
                 core->health_producer_generation !=
                     sample.producer_generation) ||
                core->health_sample_sequence == UINT64_MAX) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            core->health_producer_generation = sample.producer_generation;
            const uint64_t faults = resource_fault_count(core);
            const uint16_t metric_count = 11U;
            const uint64_t uptime_ms =
                gpio_monotonic_us(core, core->hal.milliseconds()) /
                    UINT64_C(1000) -
                core->health_started_ms;
            uint8_t* data = payload + 1U;
            memset(data, 0, RBSP_HEALTH_HEADER_SIZE +
                            metric_count * RBSP_HEALTH_METRIC_SIZE);
            put_u16(data, 1U);
            data[2U] = RBSP_HEALTH_SOURCE_MCU;
            data[3U] = faults != 0U ||
                       (((sample.available_fields &
                          RBSP_MCU_HEALTH_STACK_FREE_AVAILABLE) != 0U) &&
                        sample.minimum_stack_free_bytes == 0U)
                           ? RBSP_OVERALL_HEALTH_FAULT
                           : RBSP_OVERALL_HEALTH_UNKNOWN;
            put_u64(data + 4U, ++core->health_sample_sequence);
            put_u64(data + 12U, uptime_ms);
            put_u32(data + 20U, core->node_id);
            put_u64(data + 24U, sample.producer_generation);
            put_u16(data + 32U, metric_count);
            uint8_t* metric = data + RBSP_HEALTH_HEADER_SIZE;
#define RBSP_PUT_HEALTH_METRIC(id, availability, unit, value)             \
    do {                                                                  \
        put_health_metric(metric, (id), (availability), (unit), (value)); \
        metric += RBSP_HEALTH_METRIC_SIZE;                                \
    } while (0)
            RBSP_PUT_HEALTH_METRIC(
                1U, (sample.available_fields &
                     RBSP_MCU_HEALTH_CPU_LOAD_AVAILABLE) != 0U
                        ? RBSP_METRIC_AVAILABLE : RBSP_METRIC_UNAVAILABLE,
                RBSP_METRIC_UNIT_PERMILLE, sample.cpu_load_permille);
            RBSP_PUT_HEALTH_METRIC(
                2U, (sample.available_fields &
                     RBSP_MCU_HEALTH_ISR_LOAD_AVAILABLE) != 0U
                        ? RBSP_METRIC_AVAILABLE : RBSP_METRIC_UNAVAILABLE,
                RBSP_METRIC_UNIT_PERMILLE, sample.isr_load_permille);
            RBSP_PUT_HEALTH_METRIC(
                3U, (sample.available_fields &
                     RBSP_MCU_HEALTH_STACK_FREE_AVAILABLE) != 0U
                        ? RBSP_METRIC_AVAILABLE : RBSP_METRIC_UNAVAILABLE,
                RBSP_METRIC_UNIT_BYTES, sample.minimum_stack_free_bytes);
            RBSP_PUT_HEALTH_METRIC(4U, RBSP_METRIC_UNAVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT, 0U);
            RBSP_PUT_HEALTH_METRIC(5U, RBSP_METRIC_UNAVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT, 0U);
#if defined(CONFIG_REMOTEBSP_MOTION)
            RBSP_PUT_HEALTH_METRIC(6U, RBSP_METRIC_AVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT,
                                   core->motion.size);
            RBSP_PUT_HEALTH_METRIC(7U, RBSP_METRIC_AVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT,
                                   CONFIG_MOTION_QUEUE_DEPTH);
#else
            RBSP_PUT_HEALTH_METRIC(6U, RBSP_METRIC_UNAVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT, 0U);
            RBSP_PUT_HEALTH_METRIC(7U, RBSP_METRIC_UNAVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT, 0U);
#endif
            RBSP_PUT_HEALTH_METRIC(20U, RBSP_METRIC_AVAILABLE,
                                   RBSP_METRIC_UNIT_MILLISECONDS,
                                   uptime_ms);
            RBSP_PUT_HEALTH_METRIC(21U, RBSP_METRIC_AVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT,
                                   active_lease_count(core));
            RBSP_PUT_HEALTH_METRIC(22U, RBSP_METRIC_AVAILABLE,
                                   RBSP_METRIC_UNIT_COUNT, faults);
            RBSP_PUT_HEALTH_METRIC(
                23U, (sample.available_fields &
                      RBSP_MCU_HEALTH_SAMPLE_OVERRUN_AVAILABLE) != 0U
                         ? RBSP_METRIC_AVAILABLE : RBSP_METRIC_UNAVAILABLE,
                RBSP_METRIC_UNIT_COUNT, sample.sample_overrun_total);
#undef RBSP_PUT_HEALTH_METRIC
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, data,
                (uint16_t)(RBSP_HEALTH_HEADER_SIZE +
                           metric_count * RBSP_HEALTH_METRIC_SIZE));
            break;
        }

#if defined(CONFIG_REMOTEBSP_MOTION)
        case RBSP_COMMAND_TIME_SYNC: {
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (core->motion_group.boot_epoch == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 4U ||
                       request->payload[0] != 1U ||
                       request->payload[1] != 0U ||
                       get_u16(request->payload + 2U) != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                uint8_t data[36U];
                memset(data, 0, sizeof(data));
                data[0] = 1U;
                data[1] = 64U;
                put_u64(data + 4U, core->motion_group.boot_epoch);
                put_u64(data + 12U, UINT64_C(1000000000));
                put_u64(data + 20U, core->hal.nanoseconds());
                put_u64(data + 28U, core->hal.nanoseconds());
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }
#endif

        case RBSP_COMMAND_RESOURCE_ENUM: {
            if (request->object_id != 0U || request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            rbsp_static_resource_iterator_t iterator = {0U, 0U};
            rbsp_static_resource_descriptor_t descriptor;
            uint16_t count = 0U;
            uint16_t data_length = 2U;
            bool exhausted = false;
            while (next_static_resource(core, &iterator, &descriptor)) {
                if ((uint32_t)RBSP_HEADER_SIZE + 1U + data_length +
                        RBSP_RESOURCE_DESCRIPTOR_SIZE >
                    CONFIG_REMOTE_MAX_PACKET_SIZE) {
                    exhausted = true;
                    break;
                }
                encode_resource_descriptor(
                    payload + 1U + data_length, &descriptor);
                data_length += RBSP_RESOURCE_DESCRIPTOR_SIZE;
                ++count;
            }
            if (exhausted) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_EXHAUSTED,
                    0U, NULL, 0U);
            } else {
                put_u16(payload + 1U, count);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    payload + 1U, data_length);
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_DESCRIBE: {
            rbsp_static_resource_descriptor_t descriptor;
            if (request->object_id != 0U || request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!find_static_resource(
                           core, get_u32(request->payload), &descriptor)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t data[RBSP_RESOURCE_DESCRIPTOR_SIZE];
                encode_resource_descriptor(data, &descriptor);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_STATUS: {
            rbsp_static_resource_descriptor_t descriptor;
            if (request->object_id != 0U || request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!find_static_resource(
                           core, get_u32(request->payload), &descriptor)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t data[RBSP_RESOURCE_STATUS_SIZE];
                encode_resource_runtime_status(core, &descriptor, data);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_RESET: {
            rbsp_static_resource_descriptor_t descriptor;
            uint8_t status = RBSP_STATUS_UNSUPPORTED_CAPABILITY;
            if (request->object_id != 0U || request->payload_length != 4U) {
                status = RBSP_STATUS_INVALID_PAYLOAD;
            } else if (!find_static_resource(
                           core, get_u32(request->payload), &descriptor)) {
                status = RBSP_STATUS_OBJECT_NOT_FOUND;
            } else {
                bool owned_by_peer = false;
                (void)owned_by_peer;
#if defined(CONFIG_REMOTEBSP_BUS)
                size_t bus_resource_index = 0U;
                const rbsp_bus_resource_config_t* const bus_resource =
                    find_bus_resource(core, descriptor.resource_id,
                                      &bus_resource_index);
                if (bus_resource != NULL &&
                    bus_resource_index < CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT &&
                    core->hal.bus_reset != NULL) {
                    const rbsp_bus_lease_t* const lease =
                        &core->bus_leases[bus_resource_index];
                    owned_by_peer = lease->active &&
                                    lease->owner_session_id !=
                                        request->session_id;
                    if (!lease->active || owned_by_peer ||
                        lease->owner_session_id != request->session_id) {
                        status = RBSP_STATUS_ACCESS_DENIED;
                    } else if (!core->hal.bus_reset(bus_resource)) {
                        core->bus_status[bus_resource_index].backend_failed =
                            true;
                        status = RBSP_STATUS_RESOURCE_FAILED;
                    } else {
                        memset(&core->bus_leases[bus_resource_index], 0,
                               sizeof(core->bus_leases[bus_resource_index]));
                        memset(&core->bus_status[bus_resource_index], 0,
                               sizeof(core->bus_status[bus_resource_index]));
                        status = RBSP_STATUS_OK;
                    }
                } else
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
                if (descriptor.type == RBSP_RESOURCE_TYPE_UART &&
                    descriptor.instance < CONFIG_UART_RESOURCE_COUNT &&
                    core->hal.uart_reset != NULL) {
                    for (size_t index = 0U;
                         index < CONFIG_UART_RESOURCE_COUNT; ++index) {
                        const rbsp_uart_object_t* const object =
                            &core->uart_objects[index];
                        if (object->used &&
                            object->port == descriptor.instance &&
                            object->owner_session_id != request->session_id) {
                            owned_by_peer = true;
                            break;
                        }
                    }
                    if (owned_by_peer) {
                        status = RBSP_STATUS_ACCESS_DENIED;
                    } else if (!core->hal.uart_reset(
                                   (uint8_t)descriptor.instance)) {
                        core->uart_status[descriptor.instance].backend_failed =
                            true;
                        status = RBSP_STATUS_RESOURCE_FAILED;
                    } else {
                        for (size_t index = 0U;
                             index < CONFIG_UART_RESOURCE_COUNT; ++index) {
                            rbsp_uart_object_t* const object =
                                &core->uart_objects[index];
                            if (object->used &&
                                object->port == descriptor.instance) {
                                object->used = false;
                            }
                        }
                        memset(&core->uart_status[descriptor.instance], 0,
                               sizeof(core->uart_status[descriptor.instance]));
                        status = RBSP_STATUS_OK;
                    }
                } else
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
                if (descriptor.type == RBSP_RESOURCE_TYPE_PWM &&
                    descriptor.instance < CONFIG_PWM_RESOURCE_COUNT &&
                    core->hal.pwm_stop != NULL) {
                    for (size_t index = 0U;
                         index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
                        const rbsp_pwm_object_t* const object =
                            &core->pwm_objects[index];
                        if (object->used &&
                            object->channel == descriptor.instance &&
                            object->owner_session_id != request->session_id) {
                            owned_by_peer = true;
                            break;
                        }
                    }
                    if (owned_by_peer) {
                        status = RBSP_STATUS_ACCESS_DENIED;
                    } else if (!core->hal.pwm_stop(
                                   (uint8_t)descriptor.instance)) {
                        core->pwm_status[descriptor.instance].backend_failed =
                            true;
                        status = RBSP_STATUS_RESOURCE_FAILED;
                    } else {
                        for (size_t index = 0U;
                             index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
                            rbsp_pwm_object_t* const object =
                                &core->pwm_objects[index];
                            if (object->used &&
                                object->channel == descriptor.instance) {
                                object->used = false;
                            }
                        }
                        memset(&core->pwm_status[descriptor.instance], 0,
                               sizeof(core->pwm_status[descriptor.instance]));
                        status = RBSP_STATUS_OK;
                    }
                } else
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
                if (descriptor.type == RBSP_RESOURCE_TYPE_TIMED_BITSTREAM &&
                    descriptor.instance <
                        CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT &&
                    core->hal.timed_bitstream_abort != NULL) {
                    for (size_t index = 0U;
                         index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT;
                         ++index) {
                        const rbsp_timed_bitstream_object_t* const object =
                            &core->timed_bitstream_objects[index];
                        if (object->used &&
                            object->channel == descriptor.instance &&
                            object->owner_session_id != request->session_id) {
                            owned_by_peer = true;
                            break;
                        }
                    }
                    if (owned_by_peer) {
                        status = RBSP_STATUS_ACCESS_DENIED;
                    } else if (!core->hal.timed_bitstream_abort(
                                   (uint8_t)descriptor.instance)) {
                        core->timed_bitstream_status[descriptor.instance]
                            .backend_failed = true;
                        status = RBSP_STATUS_RESOURCE_FAILED;
                    } else {
                        for (size_t index = 0U;
                             index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT;
                             ++index) {
                            rbsp_timed_bitstream_object_t* const object =
                                &core->timed_bitstream_objects[index];
                            if (object->used &&
                                object->channel == descriptor.instance) {
                                object->used = false;
                            }
                        }
                        memset(
                            &core->timed_bitstream_status[descriptor.instance],
                            0,
                            sizeof(core->timed_bitstream_status[
                                descriptor.instance]));
                        status = RBSP_STATUS_OK;
                    }
                } else
#endif
                {
                    status = RBSP_STATUS_UNSUPPORTED_CAPABILITY;
                }
            }
            response_size = make_status_response(
                core, request, status, 0U, NULL, 0U);
            break;
        }

#if defined(CONFIG_REMOTEBSP_MOTION)
        case RBSP_COMMAND_RESOURCE_CONTRACT: {
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
            if (handle_static_resource_lease_command(core, request, &response_size)) break;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (handle_bus_resource_command(
                    core, request, &response_size)) {
                break;
            }
#endif
            if (basic_resource_contract(core, request, &response_size)) {
                break;
            }
            uint8_t axis = 0U;
            const uint32_t resource_id =
                request->payload_length == 4U
                    ? get_u32(request->payload)
                    : 0U;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!motion_axis_from_resource(core, resource_id, &axis)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t data[32U];
                memset(data, 0, sizeof(data));
                put_u32(data, resource_id);
                put_u16(data + 4U, 1U);
                put_u16(data + 6U,
                        RBSP_RESOURCE_ACCESS_WRITABLE |
                            RBSP_RESOURCE_ACCESS_EXCLUSIVE_WRITE |
                            RBSP_RESOURCE_ACCESS_LEASE_SUPPORTED |
                            RBSP_RESOURCE_ACCESS_LEASE_REQUIRED);
                put_u32(data + 8U, 1000U);
                /* 尚未在实体板量化最坏服务延迟，按协议以 0 表示未声明。 */
                put_u32(data + 12U, 0U);
                put_u32(data + 16U,
                        core->motion.maximum_step_rate_hz[axis]);
                put_u32(data + 20U, CONFIG_MOTION_QUEUE_DEPTH);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_ACQUIRE: {
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
            if (handle_static_resource_lease_command(core, request, &response_size)) break;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (handle_bus_resource_command(
                    core, request, &response_size)) {
                break;
            }
#endif
            uint8_t axis = 0U;
            const uint32_t resource_id =
                request->payload_length == 9U
                    ? get_u32(request->payload)
                    : 0U;
            const uint32_t duration_ms =
                request->payload_length == 9U
                    ? get_u32(request->payload + 4U)
                    : 0U;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->session_id == 0U ||
                       request->payload_length != 9U ||
                       duration_ms < RBSP_RESOURCE_LEASE_MINIMUM_MS ||
                       duration_ms > RBSP_RESOURCE_LEASE_MAXIMUM_MS ||
                       request->payload[8U] < 1U ||
                       request->payload[8U] >
                           RBSP_RESOURCE_LEASE_EXCLUSIVE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!motion_axis_from_resource(core, resource_id, &axis)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else if (request->payload[8U] !=
                       RBSP_RESOURCE_LEASE_EXCLUSIVE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else if (core->stepgen_leases[axis].active) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    0U, NULL, 0U);
            } else {
                rbsp_stepgen_lease_t* lease = &core->stepgen_leases[axis];
                lease->active = true;
                lease->lease_id = core->next_stepgen_lease_id++;
                if (core->next_stepgen_lease_id == 0U) {
                    core->next_stepgen_lease_id = 1U;
                }
                lease->owner_session_id = request->session_id;
                lease->granted_duration_ms = duration_ms;
                lease->expires_at_ms =
                    core->hal.milliseconds() + duration_ms;
                uint8_t data[27U];
                encode_stepgen_lease_info(
                    data, resource_id, lease, request->session_id,
                    core->hal.milliseconds(), true);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_RENEW:
        case RBSP_COMMAND_RESOURCE_RELEASE: {
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
            if (handle_static_resource_lease_command(core, request, &response_size)) break;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (handle_bus_resource_command(
                    core, request, &response_size)) {
                break;
            }
#endif
            uint8_t axis = 0U;
            const uint32_t resource_id =
                request->payload_length == 16U
                    ? get_u32(request->payload)
                    : 0U;
            const uint64_t lease_id =
                request->payload_length == 16U
                    ? get_u64(request->payload + 4U)
                    : 0U;
            const uint32_t duration_ms =
                request->payload_length == 16U
                    ? get_u32(request->payload + 12U)
                    : 0U;
            const bool release = request->command ==
                                 RBSP_COMMAND_RESOURCE_RELEASE;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->session_id == 0U ||
                       request->payload_length != 16U || lease_id == 0U ||
                       (release ? duration_ms != 0U
                                : duration_ms < RBSP_RESOURCE_LEASE_MINIMUM_MS ||
                                      duration_ms >
                                          RBSP_RESOURCE_LEASE_MAXIMUM_MS)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!motion_axis_from_resource(core, resource_id, &axis)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                rbsp_stepgen_lease_t* lease = &core->stepgen_leases[axis];
                if (!lease->active || lease->lease_id != lease_id ||
                    lease->owner_session_id != request->session_id) {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_ACCESS_DENIED,
                        0U, NULL, 0U);
                } else if (release) {
                    const uint32_t owner_session_id =
                        lease->owner_session_id;
                    memset(lease, 0, sizeof(*lease));
                    (void)stop_for_stepgen_lease_loss(
                        core, axis, owner_session_id);
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
                } else {
                    lease->granted_duration_ms = duration_ms;
                    lease->expires_at_ms =
                        core->hal.milliseconds() + duration_ms;
                    uint8_t data[27U];
                    encode_stepgen_lease_info(
                        data, resource_id, lease, request->session_id,
                        core->hal.milliseconds(), true);
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK, 0U,
                        data, sizeof(data));
                }
            }
            break;
        }

        case RBSP_COMMAND_RESOURCE_LEASE_STATUS: {
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
            if (handle_static_resource_lease_command(core, request, &response_size)) break;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (handle_bus_resource_command(
                    core, request, &response_size)) {
                break;
            }
#endif
            uint8_t axis = 0U;
            const uint32_t resource_id =
                request->payload_length == 4U
                    ? get_u32(request->payload)
                    : 0U;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                       request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (!motion_axis_from_resource(core, resource_id, &axis)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else {
                uint8_t data[27U];
                encode_stepgen_lease_info(
                    data, resource_id, &core->stepgen_leases[axis],
                    request->session_id, core->hal.milliseconds(), false);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
            }
            break;
        }

#endif
#if defined(CONFIG_REMOTEBSP_ADC)
        case RBSP_COMMAND_ADC_CONTRACT: {
            const rbsp_adc_resource_config_t* resource = NULL;
            if (request->object_id == 0U && request->payload_length == 4U) {
                resource = find_adc_resource(
                    core, get_u32(request->payload), NULL);
            }
            if (request->object_id != 0U || request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
            } else if (resource == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
            } else {
                uint8_t data[20U] = {0U};
                put_u16(data, 1U);
                put_u16(data + 2U, resource->resolution_bits);
                put_u32(data + 4U, resource->resource_id);
                put_u32(data + 8U, resource->maximum_sample_rate_hz);
                put_u32(data + 12U, resource->reference_mv);
                put_u16(data + 16U, resource->maximum_batch_samples);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_ADC_SAMPLE: {
            const uint32_t resource_id = request->payload_length >= 4U
                                             ? get_u32(request->payload) : 0U;
            size_t resource_index = 0U;
            const rbsp_adc_resource_config_t* resource =
                find_adc_resource(core, resource_id, &resource_index);
            if (request->object_id != 0U || request->payload_length != 16U ||
                get_u16(request->payload + 14U) != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            if (resource == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
                break;
            }
            const uint32_t timeout_us = get_u32(request->payload + 4U);
            const uint32_t interval_us = get_u32(request->payload + 8U);
            const uint16_t count = get_u16(request->payload + 12U);
            const uint64_t duration = count > 0U
                ? (uint64_t)interval_us * (uint64_t)(count - 1U) : 0U;
            const uint32_t minimum_interval = resource->maximum_sample_rate_hz
                ? (1000000U + resource->maximum_sample_rate_hz - 1U) /
                      resource->maximum_sample_rate_hz : UINT32_MAX;
            if (timeout_us == 0U || timeout_us > 1000000U || count == 0U ||
                count > 32U || count > resource->maximum_batch_samples ||
                (count > 1U && (interval_us == 0U ||
                                interval_us < minimum_interval)) ||
                duration > timeout_us) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            uint16_t samples[32U];
            uint32_t elapsed_us = 0U;
            if (!core->hal.adc_sample(resource, timeout_us, interval_us,
                                      samples, count, &elapsed_us) ||
                elapsed_us > timeout_us) {
                core->adc_status[resource_index].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED, 0U, NULL, 0U);
                break;
            }
            uint8_t data[80U] = {0U};
            put_u32(data, resource_id);
            put_u32(data + 4U, ++core->adc_sequence[resource_index]);
            put_u32(data + 8U, elapsed_us);
            put_u16(data + 12U, count);
            for (uint16_t index = 0U; index < count; ++index) {
                put_u16(data + 16U + index * 2U, samples[index]);
            }
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, data,
                (uint16_t)(16U + count * 2U));
            break;
        }
#endif

#if defined(CONFIG_REMOTEBSP_TIMER)
        case RBSP_COMMAND_TIMER_CONTRACT: {
            const rbsp_timer_resource_config_t* resource = NULL;
            if (request->object_id == 0U && request->payload_length == 4U)
                resource = find_timer_resource(core, get_u32(request->payload), NULL);
            if (request->object_id != 0U || request->payload_length != 4U)
                response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
            else if (resource == NULL)
                response_size = make_status_response(core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
            else {
                uint8_t data[16U];
                put_u16(data, 1U); put_u16(data + 2U, resource->capabilities);
                put_u32(data + 4U, resource->resource_id);
                put_u32(data + 8U, resource->tick_hz);
                put_u32(data + 12U, resource->maximum_operation_us);
                response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            }
            break;
        }
        case RBSP_COMMAND_TIMER_EXECUTE: {
            const uint32_t resource_id = request->payload_length >= 4U ? get_u32(request->payload) : 0U;
            size_t resource_index = 0U;
            const rbsp_timer_resource_config_t* resource = find_timer_resource(core, resource_id, &resource_index);
            if (request->object_id != 0U || request->payload_length != 16U || get_u16(request->payload + 6U) != 0U) {
                response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U); break;
            }
            if (resource == NULL) {
                response_size = make_status_response(core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U); break;
            }
            const uint16_t operation = get_u16(request->payload + 4U);
            const uint32_t parameter_us = get_u32(request->payload + 8U);
            const uint32_t timeout_us = get_u32(request->payload + 12U);
            const uint16_t operation_bit = operation >= 1U && operation <= 3U ? (uint16_t)(1U << (operation - 1U)) : 0U;
            if (operation_bit == 0U || (resource->capabilities & operation_bit) == 0U ||
                parameter_us == 0U || timeout_us == 0U || parameter_us > timeout_us ||
                timeout_us > resource->maximum_operation_us) {
                response_size = make_status_response(core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U); break;
            }
            if (!static_resource_lease_allows(core, resource_id,
                    request->session_id, RBSP_RESOURCE_LEASE_EXCLUSIVE)) {
                response_size = make_status_response(core, request, RBSP_STATUS_ACCESS_DENIED, 0U, NULL, 0U); break;
            }
            uint64_t value = 0U; uint32_t elapsed_us = 0U;
            if (!core->hal.timer_execute(resource, operation, parameter_us, timeout_us, &value, &elapsed_us) || elapsed_us > timeout_us) {
                core->timer_status[resource_index].backend_failed = true;
                response_size = make_status_response(core, request, RBSP_STATUS_RESOURCE_FAILED, 0U, NULL, 0U); break;
            }
            uint8_t data[24U] = {0U};
            put_u32(data, resource_id); put_u16(data + 4U, operation);
            put_u32(data + 8U, ++core->timer_sequence[resource_index]);
            put_u64(data + 12U, value); put_u32(data + 20U, elapsed_us);
            response_size = make_status_response(core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            break;
        }
#endif

#if defined(CONFIG_REMOTEBSP_STORAGE)
        case RBSP_COMMAND_STORAGE_CONTRACT: {
            const rbsp_storage_resource_config_t* resource = NULL;
            if (request->object_id == 0U && request->payload_length == 4U) {
                resource = find_storage_resource(
                    core, get_u32(request->payload), NULL);
            }
            if (request->object_id != 0U || request->payload_length != 4U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
            } else if (resource == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
            } else {
                uint8_t data[24U] = {0U};
                put_u16(data, 1U);
                put_u16(data + 2U, resource->flags);
                put_u32(data + 4U, resource->resource_id);
                put_u32(data + 8U, resource->capacity_bytes);
                put_u32(data + 12U, resource->erase_block_bytes);
                put_u32(data + 16U, resource->write_alignment_bytes);
                put_u32(data + 20U, resource->maximum_transfer_bytes);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            }
            break;
        }

        case RBSP_COMMAND_STORAGE_READ:
        case RBSP_COMMAND_STORAGE_ERASE: {
            const uint32_t resource_id = request->payload_length >= 4U
                                             ? get_u32(request->payload) : 0U;
            size_t resource_index = 0U;
            const rbsp_storage_resource_config_t* resource =
                find_storage_resource(core, resource_id, &resource_index);
            if (request->object_id != 0U || request->payload_length != 16U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            if (resource == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
                break;
            }
            const uint32_t offset = get_u32(request->payload + 4U);
            const uint32_t length = get_u32(request->payload + 8U);
            const uint32_t timeout_us = get_u32(request->payload + 12U);
            const bool erase = request->command == RBSP_COMMAND_STORAGE_ERASE;
            if (!static_resource_lease_allows(
                    core, resource_id, request->session_id,
                    erase ? RBSP_RESOURCE_LEASE_EXCLUSIVE
                          : RBSP_RESOURCE_LEASE_SHARED)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED, 0U, NULL, 0U);
                break;
            }
            if (length == 0U || length > resource->maximum_transfer_bytes ||
                timeout_us == 0U || timeout_us > 1000000U ||
                offset > resource->capacity_bytes ||
                length > resource->capacity_bytes - offset ||
                (erase && (offset % resource->erase_block_bytes != 0U ||
                           length % resource->erase_block_bytes != 0U)) ||
                (!erase && (uint32_t)RBSP_HEADER_SIZE + 13U + length >
                               CONFIG_REMOTE_MAX_PACKET_SIZE)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            bool ok = false;
            if (erase) {
                ok = core->hal.storage_erase(
                    resource, offset, length, timeout_us);
            } else {
                uint8_t* data = payload + 13U;
                ok = core->hal.storage_read(
                    resource, offset, data, length, timeout_us);
                if (ok) {
                    put_u32(payload + 1U, resource_id);
                    put_u32(payload + 5U, offset);
                    put_u32(payload + 9U, length);
                }
            }
            if (!ok) {
                core->storage_status[resource_index].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED, 0U, NULL, 0U);
            } else if (erase) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    payload + 1U, (uint16_t)(12U + length));
            }
            break;
        }

        case RBSP_COMMAND_STORAGE_PROGRAM: {
            const uint32_t resource_id = request->payload_length >= 4U
                                             ? get_u32(request->payload) : 0U;
            size_t resource_index = 0U;
            const rbsp_storage_resource_config_t* resource =
                find_storage_resource(core, resource_id, &resource_index);
            if (request->object_id != 0U || request->payload_length < 17U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            if (resource == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND, 0U, NULL, 0U);
                break;
            }
            if (!static_resource_lease_allows(core, resource_id,
                    request->session_id, RBSP_RESOURCE_LEASE_EXCLUSIVE)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED, 0U, NULL, 0U);
                break;
            }
            const uint32_t offset = get_u32(request->payload + 4U);
            const uint32_t timeout_us = get_u32(request->payload + 8U);
            const uint32_t length = get_u32(request->payload + 12U);
            if (length == 0U || length > resource->maximum_transfer_bytes ||
                request->payload_length != 16U + length ||
                timeout_us == 0U || timeout_us > 1000000U ||
                offset > resource->capacity_bytes ||
                length > resource->capacity_bytes - offset ||
                offset % resource->write_alignment_bytes != 0U ||
                length % resource->write_alignment_bytes != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD, 0U, NULL, 0U);
                break;
            }
            if (!core->hal.storage_program(
                    resource, offset, request->payload + 16U,
                    length, timeout_us)) {
                core->storage_status[resource_index].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED, 0U, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
            }
            break;
        }
#endif

#if (defined(CONFIG_REMOTEBSP_BUS) || defined(CONFIG_REMOTEBSP_TIMER) || \
     defined(CONFIG_REMOTEBSP_STORAGE)) && !defined(CONFIG_REMOTEBSP_MOTION)
        case RBSP_COMMAND_RESOURCE_CONTRACT:
        case RBSP_COMMAND_RESOURCE_ACQUIRE:
        case RBSP_COMMAND_RESOURCE_RENEW:
        case RBSP_COMMAND_RESOURCE_RELEASE:
        case RBSP_COMMAND_RESOURCE_LEASE_STATUS:
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
            if (handle_static_resource_lease_command(core, request, &response_size)) break;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
            if (!handle_bus_resource_command(core, request, &response_size)) {
#else
            {
#endif
                if (request->command == RBSP_COMMAND_RESOURCE_CONTRACT &&
                    basic_resource_contract(
                        core, request, &response_size)) {
                    break;
                }
                uint16_t expected_length = 4U;
                if (request->command == RBSP_COMMAND_RESOURCE_ACQUIRE) {
                    expected_length = 9U;
                } else if (request->command == RBSP_COMMAND_RESOURCE_RENEW ||
                           request->command == RBSP_COMMAND_RESOURCE_RELEASE) {
                    expected_length = 16U;
                }
                response_size = make_status_response(
                    core, request,
                    request->object_id != 0U ||
                            request->payload_length != expected_length
                        ? RBSP_STATUS_INVALID_PAYLOAD
                        : RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            }
            break;
#endif

#if !defined(CONFIG_REMOTEBSP_BUS) && !defined(CONFIG_REMOTEBSP_MOTION) && \
    !defined(CONFIG_REMOTEBSP_TIMER) && !defined(CONFIG_REMOTEBSP_STORAGE)
        case RBSP_COMMAND_RESOURCE_CONTRACT:
            if (!basic_resource_contract(core, request, &response_size)) {
                response_size = make_status_response(
                    core, request,
                    request->object_id != 0U || request->payload_length != 4U
                        ? RBSP_STATUS_INVALID_PAYLOAD
                        : RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            }
            break;
#endif

        case RBSP_COMMAND_BOOTLOADER_ENTER:
        case RBSP_COMMAND_BOOTLOADER_ENTER_USB:
            if (request->object_id != 0U ||
                request->payload_length !=
                    sizeof(bootloader_confirmation) ||
                memcmp(request->payload, bootloader_confirmation,
                       sizeof(bootloader_confirmation)) != 0) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else if (core->hal.enter_bootloader == NULL) {
                response_size = make_status_response(
                    core, request,
                    RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    0U, NULL, 0U);
                request_bootloader = true;
                if (request->command ==
                    RBSP_COMMAND_BOOTLOADER_ENTER_USB) {
                    bootloader_mode = RBSP_BOOTLOADER_USB;
                }
            }
            break;

        case RBSP_COMMAND_GPIO_CREATE: {
            if ((core->hal.gpio_configure == NULL &&
                 core->hal.gpio_configure_pull == NULL) ||
                core->hal.gpio_read == NULL ||
                core->hal.gpio_write == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (request->object_id != 0U ||
                request->payload_length != 4U ||
                request->payload[2] > RBSP_GPIO_OUTPUT ||
                request->payload[3] > 1U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            const uint16_t pin = get_u16(request->payload);
            if (core->hal.gpio_resource_allowed != NULL &&
                !core->hal.gpio_resource_allowed(
                    pin, (rbsp_gpio_direction_t)request->payload[2])) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
                break;
            }
            rbsp_gpio_object_t* free_object = NULL;
            bool busy = false;
            for (size_t index = 0;
                 index < CONFIG_GPIO_RESOURCE_COUNT; ++index) {
                if (!core->gpio_objects[index].used &&
                    free_object == NULL) {
                    free_object = &core->gpio_objects[index];
                } else if (core->gpio_objects[index].used &&
                           core->gpio_objects[index].pin == pin) {
                    busy = true;
                }
            }
            if (busy) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    0U, NULL, 0U);
            } else if (free_object == NULL ||
                       core->next_object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_EXHAUSTED,
                    0U, NULL, 0U);
            } else if (!configure_gpio(
                           core, pin,
                           (rbsp_gpio_direction_t)request->payload[2],
                           RBSP_GPIO_FLOATING,
                           request->payload[3] != 0U)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
                free_object->owner_session_id = request->session_id;
                free_object->pin = pin;
                free_object->direction =
                    (rbsp_gpio_direction_t)request->payload[2];
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    free_object->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_GPIO_READ: {
            rbsp_gpio_object_t* object =
                find_gpio_object(core, request->object_id);
            if (request->payload_length != 0U ||
                request->object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else {
                bool value = false;
                if (!core->hal.gpio_read(object->pin, &value)) {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_FAILED,
                        request->object_id, NULL, 0U);
                } else {
                    payload[1] = value ? 1U : 0U;
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK,
                        request->object_id, payload + 1U, 1U);
                }
            }
            break;
        }

        case RBSP_COMMAND_GPIO_WRITE: {
            rbsp_gpio_object_t* object =
                find_gpio_object(core, request->object_id);
            if (request->payload_length != 1U ||
                request->payload[0] > 1U ||
                request->object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (object->direction != RBSP_GPIO_OUTPUT) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.gpio_write(
                           object->pin, request->payload[0] != 0U)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_GPIO_CLOSE: {
            rbsp_gpio_object_t* object =
                find_gpio_object(core, request->object_id);
            if (request->object_id == 0U ||
                request->payload_length != 1U ||
                request->payload[0] != 1U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                /* 不存在即已关闭：响应丢失后的重复请求确定成功。 */
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (object->direction == RBSP_GPIO_OUTPUT &&
                       !core->hal.gpio_write(object->pin, false)) {
                /* 写低失败时保留对象，绝不能释放后让新所有者接管。 */
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            }
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
            else if (object->input_events_enabled &&
                       core->hal.gpio_input_event_configure != NULL &&
                       !core->hal.gpio_input_event_configure(
                           object->pin, false)) {
                /* EXTI仍活跃时保留对象，禁止新会话接管同一静态端点。 */
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            }
#endif
            else {
                memset(object, 0, sizeof(*object));
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_GPIO_INPUT_SUBSCRIBE: {
            rbsp_gpio_object_t* object =
                find_gpio_object(core, request->object_id);
            const uint8_t edge_mask = request->payload_length == 8U
                                          ? request->payload[1U]
                                          : 0U;
            const uint16_t queue_capacity = request->payload_length == 8U
                                                ? get_u16(request->payload + 2U)
                                                : 0U;
            if (request->object_id == 0U ||
                request->payload_length != 8U ||
                request->payload[0U] != RBSP_GPIO_INPUT_EVENT_VERSION ||
                edge_mask == 0U ||
                (edge_mask & ~(RBSP_GPIO_EDGE_RISING |
                               RBSP_GPIO_EDGE_FALLING)) != 0U ||
                queue_capacity == 0U ||
                queue_capacity > CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id ||
                       object->direction != RBSP_GPIO_INPUT) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else {
                bool value = false;
                if (!core->hal.gpio_read(object->pin, &value)
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
                    ||
                    (core->hal.gpio_input_event_configure != NULL &&
                     !core->hal.gpio_input_event_configure(
                         object->pin, true))
#endif
                    ) {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_FAILED,
                        request->object_id, NULL, 0U);
                } else {
                    object->input_events_enabled = true;
                    object->input_edge_mask = edge_mask;
                    object->input_queue_capacity = queue_capacity;
                    object->input_debounce_us =
                        get_u32(request->payload + 4U);
                    object->stable_value = value;
                    object->candidate_value = value;
                    object->candidate_active = false;
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
                    object->input_irq_hint = false;
#endif
                    object->candidate_since_us = 0U;
                    object->input_event_sequence = 0U;
                    object->input_dropped_events = 0U;
                    object->input_event_begin = 0U;
                    object->input_event_count = 0U;
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK,
                        request->object_id, NULL, 0U);
                }
            }
            break;
        }

        case RBSP_COMMAND_GPIO_INPUT_EVENT_STATUS: {
            rbsp_gpio_object_t* object =
                find_gpio_object(core, request->object_id);
            if (request->object_id == 0U ||
                request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id ||
                       !object->input_events_enabled) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else {
                uint8_t data[26U] = {0U};
                uint16_t data_size = 13U;
                data[0U] = RBSP_GPIO_INPUT_EVENT_VERSION;
                put_u16(data + 1U, object->input_event_count);
                put_u16(data + 3U, object->input_queue_capacity);
                put_u32(data + 5U, object->input_dropped_events);
                put_u32(data + 9U, object->input_event_sequence);
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
                uint32_t mailbox_dropped = 0U;
                if (core->hal.gpio_input_event_diagnostics != NULL &&
                    core->hal.gpio_input_event_diagnostics(
                        object->pin, &mailbox_dropped)) {
                    data[0U] = RBSP_GPIO_INPUT_EVENT_STATUS_VERSION;
                    data[13U] = 1U; /* EXTI诊断字段可用。 */
                    put_u32(data + 14U, mailbox_dropped);
                    put_u32(data + 18U, object->input_hint_matched);
                    put_u32(data + 22U, core->gpio_input_hint_ignored);
                    data_size = sizeof(data);
                }
#endif
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, data, data_size);
            }
            break;
        }

#if CONFIG_UART_RESOURCE_COUNT > 0
        case RBSP_COMMAND_UART_CREATE: {
            if (core->hal.uart_configure == NULL ||
                core->hal.uart_read == NULL ||
                core->hal.uart_write == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (request->object_id != 0U ||
                (request->payload_length != 8U &&
                 request->payload_length != 9U) ||
                get_u32(request->payload + 1U) == 0U ||
                request->payload[5] < 5U ||
                request->payload[5] > 8U ||
                (request->payload[6] != 1U &&
                 request->payload[6] != 2U) ||
                request->payload[7] > 2U ||
                (request->payload_length == 9U &&
                 request->payload[8] >
                     RBSP_UART_RECEIVE_STREAMING)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            rbsp_uart_object_t* free_object = NULL;
            bool busy = false;
            for (size_t index = 0;
                 index < CONFIG_UART_RESOURCE_COUNT; ++index) {
                if (!core->uart_objects[index].used &&
                    free_object == NULL) {
                    free_object = &core->uart_objects[index];
                } else if (core->uart_objects[index].used &&
                           core->uart_objects[index].port ==
                               request->payload[0]) {
                    busy = true;
                }
            }
            if (!uart_port_available(core, request->payload[0])) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
            } else if (!uart_runtime_baud_allowed(
                           core, request->payload[0],
                           get_u32(request->payload + 1U))) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
            } else if (busy) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    0U, NULL, 0U);
            } else if (free_object == NULL ||
                       core->next_object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_EXHAUSTED,
                    0U, NULL, 0U);
            } else if (!core->hal.uart_configure(
                           request->payload[0],
                           get_u32(request->payload + 1U),
                           request->payload[5], request->payload[6],
                           request->payload[7])) {
                core->uart_status[request->payload[0]].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
                free_object->owner_session_id = request->session_id;
                free_object->port = request->payload[0];
                free_object->streaming =
                    request->payload_length == 9U &&
                    request->payload[8] ==
                        RBSP_UART_RECEIVE_STREAMING;
                free_object->pending_length = 0U;
                free_object->event_sequence = 0U;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    free_object->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_UART_READ: {
            rbsp_uart_object_t* object =
                find_uart_object(core, request->object_id);
            const uint16_t requested =
                request->payload_length == 2U
                    ? get_u16(request->payload)
                    : 0U;
            /*
             * UART_READ 会从接收环形缓冲区消费数据。响应必须能够完整放入
             * 去重缓存，否则响应丢失后的重试会再次消费下一批数据。
             */
            const uint16_t maximum =
                CONFIG_REMOTE_CACHE_RESPONSE_SIZE - RBSP_HEADER_SIZE - 1U;
            if (request->object_id == 0U ||
                request->payload_length != 2U ||
                requested == 0U || requested > maximum) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (object->streaming) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else {
                const size_t count = core->hal.uart_read(
                    object->port, payload + 1U, requested);
                if (count > requested) {
                    core->uart_status[object->port].backend_failed = true;
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_FAILED,
                        request->object_id, NULL, 0U);
                } else {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK,
                        request->object_id, payload + 1U,
                        (uint16_t)count);
                }
            }
            break;
        }

        case RBSP_COMMAND_UART_WRITE: {
            rbsp_uart_object_t* object =
                find_uart_object(core, request->object_id);
            if (request->object_id == 0U ||
                request->payload_length == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.uart_write(
                           object->port, request->payload,
                           request->payload_length)) {
                core->uart_status[object->port].tx_overruns =
                    saturating_add_u32(
                        core->uart_status[object->port].tx_overruns, 1U);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }
#endif

#if defined(CONFIG_REMOTEBSP_BUS)
        case RBSP_COMMAND_I2C_CONTRACT:
        case RBSP_COMMAND_I2C_TRANSFER:
        case RBSP_COMMAND_SPI_CONTRACT:
        case RBSP_COMMAND_SPI_TRANSFER:
            (void)handle_bus_command(core, request, &response_size);
            break;
#endif

#if defined(CONFIG_REMOTEBSP_PWM)
        case RBSP_COMMAND_PWM_CREATE: {
            if (core->hal.pwm_configure == NULL ||
                core->hal.pwm_write == NULL || core->hal.pwm_stop == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            const uint8_t channel = request->payload_length == 8U
                                        ? request->payload[0]
                                        : UINT8_MAX;
            const uint32_t frequency = request->payload_length == 8U
                                           ? get_u32(request->payload + 1U)
                                           : 0U;
            const uint16_t duty = request->payload_length == 8U
                                      ? get_u16(request->payload + 5U)
                                      : UINT16_MAX;
            if (request->object_id != 0U || request->payload_length != 8U ||
                channel >= CONFIG_PWM_RESOURCE_COUNT || frequency == 0U ||
                frequency > CONFIG_PWM_MAX_FREQUENCY_HZ || duty > 10000U ||
                request->payload[7] > 1U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            rbsp_pwm_object_t* free_object = NULL;
            bool busy = false;
            for (size_t index = 0; index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
                if (!core->pwm_objects[index].used && free_object == NULL) {
                    free_object = &core->pwm_objects[index];
                } else if (core->pwm_objects[index].used &&
                           core->pwm_objects[index].channel == channel) {
                    busy = true;
                }
            }
            if (busy) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY, 0U, NULL, 0U);
            } else if (free_object == NULL || core->next_object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_EXHAUSTED,
                    0U, NULL, 0U);
            } else if (!core->hal.pwm_configure(
                           channel, frequency, duty,
                           request->payload[7] != 0U)) {
                core->pwm_status[channel].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
                free_object->owner_session_id = request->session_id;
                free_object->channel = channel;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    free_object->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_PWM_WRITE: {
            rbsp_pwm_object_t* object =
                find_pwm_object(core, request->object_id);
            const uint16_t duty = request->payload_length == 2U
                                      ? get_u16(request->payload)
                                      : UINT16_MAX;
            if (request->object_id == 0U || request->payload_length != 2U ||
                duty > 10000U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.pwm_write(object->channel, duty)) {
                core->pwm_status[object->channel].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_PWM_STOP: {
            rbsp_pwm_object_t* object =
                find_pwm_object(core, request->object_id);
            if (request->object_id == 0U || request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.pwm_stop(object->channel)) {
                core->pwm_status[object->channel].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                /* PWM_STOP 是该对象的终止操作；成功后立即释放对象槽。 */
                object->used = false;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
        case RBSP_COMMAND_TIMED_BITSTREAM_CREATE: {
            if (core->hal.timed_bitstream_configure == NULL ||
                core->hal.timed_bitstream_write == NULL ||
                core->hal.timed_bitstream_abort == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            const uint8_t channel = request->payload_length == 17U
                                        ? request->payload[0]
                                        : UINT8_MAX;
            const uint32_t period = request->payload_length == 17U
                                        ? get_u32(request->payload + 1U)
                                        : 0U;
            const uint32_t zero_high = request->payload_length == 17U
                                           ? get_u32(request->payload + 5U)
                                           : 0U;
            const uint32_t one_high = request->payload_length == 17U
                                          ? get_u32(request->payload + 9U)
                                          : 0U;
            const uint32_t reset_us = request->payload_length == 17U
                                          ? get_u32(request->payload + 13U)
                                          : 0U;
            if (request->object_id != 0U || request->payload_length != 17U ||
                channel >= CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT ||
                period == 0U || zero_high == 0U || one_high == 0U ||
                zero_high >= period || one_high >= period || reset_us == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            rbsp_timed_bitstream_object_t* free_object = NULL;
            bool busy = false;
            for (size_t index = 0;
                 index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT; ++index) {
                if (!core->timed_bitstream_objects[index].used &&
                    free_object == NULL) {
                    free_object = &core->timed_bitstream_objects[index];
                } else if (core->timed_bitstream_objects[index].used &&
                           core->timed_bitstream_objects[index].channel ==
                               channel) {
                    busy = true;
                }
            }
            if (busy) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY, 0U, NULL, 0U);
            } else if (free_object == NULL || core->next_object_id == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_EXHAUSTED,
                    0U, NULL, 0U);
            } else if (!core->hal.timed_bitstream_configure(
                           channel, period, zero_high, one_high, reset_us)) {
                core->timed_bitstream_status[channel].backend_failed = true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
                free_object->owner_session_id = request->session_id;
                free_object->channel = channel;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    free_object->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_TIMED_BITSTREAM_WRITE: {
            rbsp_timed_bitstream_object_t* object =
                find_timed_bitstream_object(core, request->object_id);
            const uint16_t bit_count = request->payload_length >= 2U
                                           ? get_u16(request->payload)
                                           : 0U;
            const uint16_t data_size = (uint16_t)((bit_count + 7U) / 8U);
            if (request->object_id == 0U || bit_count == 0U ||
                bit_count > CONFIG_TIMED_BITSTREAM_MAX_BITS ||
                request->payload_length != (uint16_t)(2U + data_size)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (core->hal.timed_bitstream_busy != NULL &&
                       core->hal.timed_bitstream_busy(object->channel)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.timed_bitstream_write(
                           object->channel, request->payload + 2U,
                           bit_count)) {
                core->timed_bitstream_status[object->channel].backend_failed =
                    true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }

        case RBSP_COMMAND_TIMED_BITSTREAM_ABORT: {
            rbsp_timed_bitstream_object_t* object =
                find_timed_bitstream_object(core, request->object_id);
            if (request->object_id == 0U || request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    request->object_id, NULL, 0U);
            } else if (object == NULL) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    request->object_id, NULL, 0U);
            } else if (object->owner_session_id != request->session_id) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.timed_bitstream_abort(object->channel)) {
                core->timed_bitstream_status[object->channel].backend_failed =
                    true;
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    request->object_id, NULL, 0U);
            } else {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK,
                    request->object_id, NULL, 0U);
            }
            break;
        }
#endif

#if defined(CONFIG_REMOTEBSP_MOTION)
        case RBSP_COMMAND_MOTION_ENQUEUE: {
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            const uint32_t group_critical_state =
                core->hal.motion_enter_critical();
            rbsp_motion_group_observe_motion(
                &core->motion_group, &core->motion);
            observe_motion_owner(core);
            const bool group_blocks = rbsp_motion_group_blocks_enqueue(
                &core->motion_group);
            core->hal.motion_exit_critical(group_critical_state);
            if (group_blocks) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    0U, NULL, 0U);
                break;
            }
            const uint8_t axis_count =
                request->payload_length >= 24U
                    ? request->payload[21]
                    : 0U;
            if (request->object_id != 0U ||
                request->payload_length !=
                    (uint16_t)(24U + (uint16_t)axis_count * 8U) ||
                axis_count == 0U ||
                axis_count > core->motion.axis_count ||
                request->payload[20] > 1U ||
                get_u16(request->payload + 22U) != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            rbsp_motion_segment_t segment;
            memset(&segment, 0, sizeof(segment));
            segment.sequence = get_u32(request->payload);
            segment.start_time_ns = get_u64(request->payload + 4U);
            segment.duration_ns = get_u64(request->payload + 12U);
            segment.final_segment = request->payload[20] != 0U;
            segment.axis_count = core->motion.axis_count;
            bool seen[CONFIG_MOTION_MAX_AXES];
            memset(seen, 0, sizeof(seen));
            bool valid = true;
            uint64_t resource_mask = 0U;
            for (uint8_t index = 0U; index < axis_count; ++index) {
                const uint8_t* entry =
                    request->payload + 24U + (uint16_t)index * 8U;
                const uint32_t resource_id = get_u32(entry);
                uint8_t axis = UINT8_MAX;
                for (uint8_t candidate = 0U;
                     candidate < core->motion.axis_count; ++candidate) {
                    if (motion_resource_id(core, candidate) ==
                        resource_id) {
                        axis = candidate;
                        break;
                    }
                }
                if (axis == UINT8_MAX || seen[axis]) {
                    valid = false;
                    break;
                }
                seen[axis] = true;
                resource_mask |= UINT64_C(1) << axis;
                segment.steps[axis] =
                    (int32_t)get_u32(entry + 4U);
            }
            if (!valid) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
                break;
            }
            if (!stepgen_mask_leased(
                    core, resource_mask, request->session_id)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
                break;
            }
            const uint32_t owner_critical_state =
                core->hal.motion_enter_critical();
            observe_motion_owner(core);
            const bool owner_conflict =
                core->motion_owner_session_id != 0U &&
                core->motion_owner_session_id != request->session_id;
            core->hal.motion_exit_critical(owner_critical_state);
            if (owner_conflict) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
                break;
            }
            rbsp_motion_segment_t accepted;
            rbsp_motion_enqueue_result_t result;
            bool start_scheduler = false;
            if (!rbsp_motion_validate_segment(
                    &core->motion, &segment)) {
                ++core->motion.rejected_segments;
                result = RBSP_MOTION_ENQUEUE_INVALID;
            } else {
                const uint32_t critical_state =
                    core->hal.motion_enter_critical();
                observe_motion_owner(core);
                start_scheduler = core->motion.size == 0U;
                result = rbsp_motion_commit_segment(
                    &core->motion, &segment,
                    core->hal.nanoseconds(), &accepted);
                if (result == RBSP_MOTION_ENQUEUE_OK) {
                    core->motion_owner_session_id = request->session_id;
                    core->motion_resource_mask |= resource_mask;
                }
                core->hal.motion_exit_critical(critical_state);
            }
            if (result != RBSP_MOTION_ENQUEUE_OK) {
                uint8_t status = RBSP_STATUS_INVALID_PAYLOAD;
                if (result == RBSP_MOTION_ENQUEUE_FULL) {
                    status = RBSP_STATUS_RESOURCE_EXHAUSTED;
                } else if (result ==
                           RBSP_MOTION_ENQUEUE_FAULTED) {
                    status = RBSP_STATUS_RESOURCE_FAILED;
                }
                response_size = make_status_response(
                    core, request, status, 0U, NULL, 0U);
                break;
            }
            if (start_scheduler &&
                !rbsp_core_motion_service(core)) {
                const uint32_t abort_critical_state =
                    core->hal.motion_enter_critical();
                rbsp_motion_abort(
                    &core->motion, RBSP_MOTION_FAULT_TIMING);
                core->motion_owner_session_id = 0U;
                core->motion_resource_mask = 0U;
                core->hal.motion_exit_critical(abort_critical_state);
                (void)rbsp_core_motion_service(core);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
                break;
            }
            put_u32(payload + 1U, accepted.sequence);
            put_u64(payload + 5U, accepted.start_time_ns);
            put_u64(payload + 13U, accepted.duration_ns);
            payload[21] = accepted.final_segment ? 1U : 0U;
            payload[22] = 0U;
            put_u16(payload + 23U, 0U);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U,
                payload + 1U, 24U);
            break;
        }

        case RBSP_COMMAND_MOTION_STATUS: {
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            const uint16_t data_length =
                (uint16_t)(92U + core->motion.axis_count * 24U);
            if (request->object_id != 0U ||
                request->payload_length != 0U ||
                (uint32_t)data_length + 1U >
                    CONFIG_REMOTE_MAX_PACKET_SIZE -
                        RBSP_HEADER_SIZE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            uint8_t* data = payload + 1U;
            memset(data, 0, data_length);
            rbsp_motion_status_snapshot_t snapshot;
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            const bool snapshot_ok = rbsp_motion_snapshot(
                &core->motion, &snapshot);
            core->hal.motion_exit_critical(critical_state);
            if (!snapshot_ok) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
                break;
            }
            data[0] = (uint8_t)snapshot.state;
            data[1] = (uint8_t)snapshot.fault;
            data[2] = snapshot.axis_count;
            put_u64(data + 4U, core->hal.nanoseconds());
            put_u16(data + 12U, snapshot.queue_depth);
            put_u16(data + 14U, CONFIG_MOTION_QUEUE_DEPTH);
            put_u32(data + 16U,
                    snapshot.last_accepted_sequence);
            put_u32(data + 20U,
                    snapshot.last_completed_sequence);
            put_u64(data + 24U,
                    snapshot.accepted_segments);
            put_u64(data + 32U,
                    snapshot.rejected_segments);
            put_u64(data + 40U,
                    snapshot.completed_segments);
            put_u64(data + 48U, snapshot.emitted_edges);
            uint64_t emitted_steps = 0U;
            for (uint8_t axis = 0U;
                 axis < snapshot.axis_count; ++axis) {
                if (UINT64_MAX - emitted_steps <
                    snapshot.emitted_steps[axis]) {
                    emitted_steps = UINT64_MAX;
                } else {
                    emitted_steps +=
                        snapshot.emitted_steps[axis];
                }
            }
            put_u64(data + 56U, emitted_steps);
            put_u64(data + 64U, snapshot.safety_stops);
            put_u64(data + 72U, snapshot.limit_stops);
            put_u64(data + 80U, snapshot.queue_underruns);
            put_u16(data + 88U,
                    snapshot.maximum_queue_depth);
            put_u16(data + 90U,
                    (uint16_t)(snapshot.queue_low_watermark |
                               (snapshot.queue_low ? 0x8000U : 0U)));
            for (uint8_t axis = 0U;
                 axis < snapshot.axis_count; ++axis) {
                uint8_t* entry =
                    data + 92U + (uint16_t)axis * 24U;
                put_u32(entry, motion_resource_id(core, axis));
                entry[4] =
                    (snapshot.enabled[axis] ? 0x01U : 0U) |
                    (snapshot.direction_positive[axis]
                         ? 0x02U
                         : 0U) |
                    (snapshot.step_high[axis] ? 0x04U : 0U);
                put_u64(entry + 8U,
                        (uint64_t)snapshot.position_steps[axis]);
                put_u64(entry + 16U,
                        snapshot.emitted_steps[axis]);
            }
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U,
                data, data_length);
            break;
        }

        case RBSP_COMMAND_MOTION_CONTRACT: {
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            const uint16_t data_length =
                (uint16_t)(24U + core->motion.axis_count * 20U);
            if (request->object_id != 0U ||
                request->payload_length != 0U ||
                (uint32_t)data_length + 1U >
                    CONFIG_REMOTE_MAX_PACKET_SIZE - RBSP_HEADER_SIZE) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            uint8_t* data = payload + 1U;
            memset(data, 0, data_length);
            put_u16(data, 1U);
            put_u16(data + 4U, core->motion.axis_count);
            put_u16(data + 6U, CONFIG_MOTION_QUEUE_DEPTH);
            put_u64(data + 8U,
                    (uint64_t)CONFIG_MOTION_MIN_LEAD_TIME_US * 1000U);
            put_u32(data + 16U, CONFIG_MOTION_MAX_TOTAL_STEP_RATE_HZ);
            for (uint8_t axis = 0U;
                 axis < core->motion.axis_count; ++axis) {
                uint8_t* entry =
                    data + 24U + (uint16_t)axis * 20U;
                put_u32(entry, motion_resource_id(core, axis));
                put_u32(entry + 4U,
                        core->motion.maximum_step_rate_hz[axis]);
                put_u32(entry + 8U,
                        CONFIG_MOTION_STEP_PULSE_WIDTH_US * 1000U);
                put_u32(entry + 12U,
                        CONFIG_MOTION_MIN_STEP_LOW_US * 1000U);
                put_u32(entry + 16U,
                        CONFIG_MOTION_DIRECTION_SETUP_US * 1000U);
            }
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, data, data_length);
            break;
        }

        case RBSP_COMMAND_MOTION_GROUP_PREPARE: {
            rbsp_motion_group_identity_t identity;
            rbsp_motion_segment_t segment;
            uint64_t resource_mask = 0U;
            const uint16_t segment_length =
                request->payload_length >=
                        RBSP_MOTION_GROUP_IDENTITY_SIZE + 4U
                    ? get_u16(request->payload +
                              RBSP_MOTION_GROUP_IDENTITY_SIZE)
                    : 0U;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (core->motion_group.boot_epoch == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (request->object_id != 0U ||
                request->payload_length <
                    RBSP_MOTION_GROUP_IDENTITY_SIZE + 4U ||
                get_u16(request->payload +
                        RBSP_MOTION_GROUP_IDENTITY_SIZE + 2U) != 0U ||
                (uint32_t)request->payload_length !=
                    (uint32_t)RBSP_MOTION_GROUP_IDENTITY_SIZE + 4U +
                        segment_length ||
                !decode_motion_group_identity(request->payload, &identity)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            if (!decode_motion_group_segment(
                    core,
                    request->payload + RBSP_MOTION_GROUP_IDENTITY_SIZE + 4U,
                    segment_length, identity.node_start_tick, &segment,
                    &resource_mask)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
                break;
            }
            if (!stepgen_mask_leased(
                    core, resource_mask, request->session_id)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    0U, NULL, 0U);
                break;
            }
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            const rbsp_motion_group_ready_code_t code =
                rbsp_motion_group_prepare(
                    &core->motion_group, &core->motion, &identity, &segment,
                    resource_mask, request->session_id,
                    core->hal.nanoseconds());
            core->hal.motion_exit_critical(critical_state);
            uint8_t data[RBSP_MOTION_GROUP_RESULT_SIZE];
            encode_motion_group_result(data, &identity, (uint8_t)code);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            break;
        }

        case RBSP_COMMAND_MOTION_GROUP_COMMIT: {
            rbsp_motion_group_identity_t identity;
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (core->motion_group.boot_epoch == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (request->object_id != 0U ||
                request->payload_length != RBSP_MOTION_GROUP_IDENTITY_SIZE ||
                !decode_motion_group_identity(request->payload, &identity)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            if (rbsp_motion_group_owned_by(
                    &core->motion_group, request->session_id) &&
                !stepgen_mask_leased(
                    core, core->motion_group.resource_mask,
                    request->session_id)) {
                const uint32_t abort_critical_state =
                    core->hal.motion_enter_critical();
                (void)rbsp_motion_group_emergency_abort(
                    &core->motion_group);
                core->hal.motion_exit_critical(abort_critical_state);
                uint8_t data[RBSP_MOTION_GROUP_RESULT_SIZE];
                encode_motion_group_result(
                    data, &identity,
                    (uint8_t)RBSP_MOTION_GROUP_COMMIT_REJECTED);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U,
                    data, sizeof(data));
                break;
            }
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            const bool was_prepared = core->motion_group.state ==
                                      RBSP_MOTION_GROUP_PREPARED;
            rbsp_motion_group_commit_code_t code = rbsp_motion_group_commit(
                &core->motion_group, &core->motion, &identity,
                request->session_id, core->hal.nanoseconds(), NULL);
            if (was_prepared &&
                code == RBSP_MOTION_GROUP_COMMIT_ARMED) {
                core->motion_owner_session_id = request->session_id;
                core->motion_resource_mask =
                    core->motion_group.resource_mask;
            }
            core->hal.motion_exit_critical(critical_state);
            if (code == RBSP_MOTION_GROUP_COMMIT_ARMED &&
                !rbsp_core_motion_service(core)) {
                const uint32_t abort_critical_state =
                    core->hal.motion_enter_critical();
                (void)rbsp_motion_group_emergency_abort(
                    &core->motion_group);
                if (core->motion.size != 0U ||
                    core->motion.fault == RBSP_MOTION_FAULT_NONE) {
                    rbsp_motion_abort(
                        &core->motion, RBSP_MOTION_FAULT_TIMING);
                }
                core->motion_owner_session_id = 0U;
                core->motion_resource_mask = 0U;
                core->hal.motion_exit_critical(abort_critical_state);
                /* 确保 EN/STEP 即使在首次调度失败时也立即落入安全状态。 */
                (void)rbsp_core_motion_service(core);
                code = RBSP_MOTION_GROUP_COMMIT_REJECTED;
            }
            uint8_t data[RBSP_MOTION_GROUP_RESULT_SIZE];
            encode_motion_group_result(data, &identity, (uint8_t)code);
            response_size = make_status_response(
                core, request, RBSP_STATUS_OK, 0U, data, sizeof(data));
            break;
        }

        case RBSP_COMMAND_MOTION_GROUP_ABORT: {
            rbsp_motion_group_identity_t identity;
            const uint8_t reason =
                request->payload_length == RBSP_MOTION_GROUP_RESULT_SIZE
                    ? request->payload[RBSP_MOTION_GROUP_IDENTITY_SIZE]
                    : 0U;
            bool reserved_valid = true;
            if (request->payload_length == RBSP_MOTION_GROUP_RESULT_SIZE) {
                for (uint8_t index = 81U;
                     index < RBSP_MOTION_GROUP_RESULT_SIZE; ++index) {
                    reserved_valid &= request->payload[index] == 0U;
                }
            }
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (core->motion_group.boot_epoch == 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
                break;
            }
            if (request->object_id != 0U ||
                request->payload_length != RBSP_MOTION_GROUP_RESULT_SIZE ||
                reason == 0U || reason > 7U || !reserved_valid ||
                !decode_motion_group_identity(request->payload, &identity)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
                break;
            }
            const bool armed =
                core->motion_group.state == RBSP_MOTION_GROUP_ARMED;
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            const bool accepted = rbsp_motion_group_abort(
                &core->motion_group, &identity, request->session_id);
            if (accepted && armed) {
                rbsp_motion_abort(&core->motion,
                                  RBSP_MOTION_FAULT_ABORTED);
                core->motion_owner_session_id = 0U;
                core->motion_resource_mask = 0U;
            }
            core->hal.motion_exit_critical(critical_state);
            if (accepted && armed) {
                (void)rbsp_core_motion_service(core);
            }
            response_size = make_status_response(
                core, request,
                accepted ? RBSP_STATUS_OK : RBSP_STATUS_ACCESS_DENIED,
                0U, NULL, 0U);
            break;
        }

        case RBSP_COMMAND_MOTION_ABORT:
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                const uint32_t critical_state =
                    core->hal.motion_enter_critical();
                (void)rbsp_motion_group_emergency_abort(
                    &core->motion_group);
                rbsp_motion_abort(
                    &core->motion, RBSP_MOTION_FAULT_ABORTED);
                core->motion_owner_session_id = 0U;
                core->motion_resource_mask = 0U;
                core->hal.motion_exit_critical(critical_state);
                (void)rbsp_core_motion_service(core);
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
            }
            break;

        case RBSP_COMMAND_MOTION_CLEAR_FAULT:
            if (!motion_available(core)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_UNSUPPORTED_CAPABILITY,
                    0U, NULL, 0U);
            } else if (request->object_id != 0U ||
                request->payload_length != 0U) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_INVALID_PAYLOAD,
                    0U, NULL, 0U);
            } else {
                const uint32_t critical_state =
                    core->hal.motion_enter_critical();
                const bool cleared = rbsp_motion_clear_fault(
                    &core->motion,
                    core->hal.nanoseconds != NULL
                        ? core->hal.nanoseconds()
                        : 0U);
                core->hal.motion_exit_critical(critical_state);
                if (!cleared) {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_RESOURCE_BUSY,
                        0U, NULL, 0U);
                } else {
                    response_size = make_status_response(
                        core, request, RBSP_STATUS_OK, 0U, NULL, 0U);
                }
            }
            break;
#endif

        default:
            response_size = make_status_response(
                core, request, RBSP_STATUS_UNKNOWN_COMMAND,
                request->object_id, NULL, 0U);
            break;
    }

    insert_cache(core, request, core->tx_packet, response_size);
    const bool sent = send_packet(
        core, core->tx_packet, response_size,
        allocate_transfer_id(core), response_can_id(core));
    if (sent && request_bootloader) {
        core->bootloader_request_pending = true;
        core->bootloader_request_ms = core->hal.milliseconds();
        core->bootloader_request_mode = bootloader_mode;
    }
    return sent;
}

static rbsp_reassembly_slot_t* find_slot(
    rbsp_core_t* core, uint32_t stream_id, uint16_t transfer_id) {
    for (size_t index = 0;
         index < CONFIG_REMOTE_REASSEMBLY_SLOTS; ++index) {
        rbsp_reassembly_slot_t* slot = &core->reassembly[index];
        if (slot->active && slot->stream_id == stream_id &&
            slot->transfer_id == transfer_id) {
            return slot;
        }
    }
    return NULL;
}

static rbsp_reassembly_slot_t* allocate_slot(rbsp_core_t* core) {
    for (size_t index = 0;
         index < CONFIG_REMOTE_REASSEMBLY_SLOTS; ++index) {
        if (!core->reassembly[index].active) {
            memset(&core->reassembly[index], 0,
                   sizeof(core->reassembly[index]));
            core->reassembly[index].active = true;
            return &core->reassembly[index];
        }
    }
    return NULL;
}

bool rbsp_core_init(rbsp_core_t* core, const rbsp_hal_t* hal,
                    rbsp_link_mode_t mode,
                    const rbsp_node_info_t* info) {
    if (core == NULL || hal == NULL || info == NULL ||
        (hal->link_send == NULL && hal->can_send == NULL) ||
        hal->milliseconds == NULL ||
        (mode != RBSP_CAN_CLASSICAL && mode != RBSP_CAN_FD &&
         mode != RBSP_USB)) {
        return false;
    }
    memset(core, 0, sizeof(*core));
    core->hal = *hal;
    core->link_mode = mode;
    core->info = *info;
    core->info.firmware_identity_available_fields = 0U;
    memset(core->info.project_sha256, 0,
           sizeof(core->info.project_sha256));
    memset(core->info.config_sha256, 0,
           sizeof(core->info.config_sha256));
    memset(core->info.firmware_input_sha256, 0,
           sizeof(core->info.firmware_input_sha256));
#ifdef RBSP_STUDIO_STATIC_RESOURCE_TABLE
    if (decode_sha256_literal(RBSP_STUDIO_RESOURCE_PROJECT_SHA256,
                              core->info.project_sha256)) {
        core->info.firmware_identity_available_fields |=
            RBSP_FIRMWARE_IDENTITY_PROJECT_SHA256;
    }
    if (decode_sha256_literal(RBSP_CONFIG_SHA256,
                              core->info.config_sha256)) {
        core->info.firmware_identity_available_fields |=
            RBSP_FIRMWARE_IDENTITY_CONFIG_SHA256;
    }
    if (decode_sha256_literal(RBSP_FIRMWARE_INPUT_SHA256,
                              core->info.firmware_input_sha256)) {
        core->info.firmware_identity_available_fields |=
            RBSP_FIRMWARE_IDENTITY_INPUT_SHA256;
    }
#endif
    core->next_object_id = 1U;
#if defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
    core->next_static_resource_lease_id = 1U;
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    if (!bus_configuration_valid(hal)) {
        return false;
    }
    core->next_bus_lease_id = 1U;
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    if (hal->nanoseconds == NULL ||
        hal->motion_set_enable == NULL ||
        hal->motion_set_direction == NULL ||
        hal->motion_set_step == NULL ||
        hal->motion_schedule_compare == NULL ||
        hal->motion_cancel_compare == NULL ||
        hal->motion_enter_critical == NULL ||
        hal->motion_exit_critical == NULL ||
        !rbsp_motion_init(&core->motion, hal->motion_axis_count)) {
        return false;
    }
    const uint64_t boot_epoch = hal->motion_boot_epoch != NULL
                                    ? hal->motion_boot_epoch()
                                    : 0U;
    if (!rbsp_motion_group_init(&core->motion_group, boot_epoch)) {
        return false;
    }
    core->next_stepgen_lease_id = 1U;
    core->default_motion_axis_count = hal->motion_axis_count;
#endif
    core->next_transfer_id = 1U;
    core->last_heartbeat_ms = hal->milliseconds();
    core->gpio_clock_last_ms = core->last_heartbeat_ms;
    core->gpio_clock_epoch_ms = 0U;
    core->health_started_ms =
        gpio_monotonic_us(core, core->last_heartbeat_ms) / UINT64_C(1000);
    return true;
}

#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
bool rbsp_core_device_params_init(
    rbsp_core_t* core, const rbsp_device_param_backend* backend) {
    rbsp_device_param_record uuid;
    if (core == NULL || backend == NULL ||
        !rbsp_device_param_store_init(&core->device_params, backend) ||
        !rbsp_device_param_store_boot(&core->device_params) ||
        !rbsp_device_param_store_set_commit_budget(
            &core->device_params,
            CONFIG_REMOTEBSP_DEVICE_PARAM_COMMIT_BUDGET)) {
        return false;
    }
    core->device_params_ready = true;
    core->device_param_restart_required = false;
    core->device_param_unlock_session = 0U;
    core->device_param_unlock_token = 0U;
    core->device_param_unlock_expires_ms = 0U;
    if (rbsp_device_param_store_get(
            &core->device_params, RBSP_DEVICE_PARAM_DEVICE_UUID, &uuid) &&
        uuid.length == sizeof(core->info.uuid)) {
        memcpy(core->info.uuid, uuid.value, sizeof(core->info.uuid));
    }
    return true;
}
#endif

static void service_gpio_input_events(rbsp_core_t* core, uint32_t now_ms) {
#if CONFIG_GPIO_RESOURCE_COUNT > 0
    if (core->node_id == 0U || core->hal.gpio_read == NULL) return;
    const uint64_t now_us = gpio_monotonic_us(core, now_ms);
    for (size_t index = 0U; index < CONFIG_GPIO_RESOURCE_COUNT; ++index) {
        rbsp_gpio_object_t* object = &core->gpio_objects[index];
        if (!object->used || !object->input_events_enabled) continue;
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
        object->input_irq_hint = false;
#endif
        bool raw_value = object->stable_value;
        if (core->hal.gpio_read(object->pin, &raw_value)) {
            if (raw_value == object->stable_value) {
                object->candidate_active = false;
            } else if (!object->candidate_active ||
                       raw_value != object->candidate_value) {
                object->candidate_active = true;
                object->candidate_value = raw_value;
                object->candidate_since_us = now_us;
            } else if (now_us - object->candidate_since_us >=
                       object->input_debounce_us) {
                const uint8_t edge = raw_value ? RBSP_GPIO_EDGE_RISING
                                               : RBSP_GPIO_EDGE_FALLING;
                object->stable_value = raw_value;
                object->candidate_active = false;
                ++object->input_event_sequence;
                if ((object->input_edge_mask & edge) != 0U) {
                    if (object->input_event_count >=
                        object->input_queue_capacity) {
                        if (object->input_dropped_events != UINT32_MAX)
                            ++object->input_dropped_events;
                    } else {
                        const uint16_t position = (uint16_t)(
                            (object->input_event_begin +
                             object->input_event_count) %
                            CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY);
                        rbsp_gpio_input_event_t* event =
                            &object->input_events[position];
                        event->sequence = object->input_event_sequence;
                        event->timestamp_us = now_us;
                        event->value = raw_value;
                        event->edge = edge;
                        ++object->input_event_count;
                    }
                }
            }
        }
        if (object->input_event_count != 0U) {
            const rbsp_gpio_input_event_t* event =
                &object->input_events[object->input_event_begin];
            uint8_t data[19U] = {0U};
            data[0U] = RBSP_GPIO_INPUT_EVENT_VERSION;
            put_u32(data + 1U, event->sequence);
            put_u64(data + 5U, event->timestamp_us);
            data[13U] = event->value ? 1U : 0U;
            data[14U] = event->edge;
            put_u32(data + 15U, object->input_dropped_events);
            const uint16_t size = encode_packet(
                core->tx_packet, RBSP_MESSAGE_EVENT,
                RBSP_COMMAND_GPIO_INPUT_EVENT, 0U, event->sequence,
                object->object_id, 0U, data, sizeof(data));
            if (send_packet(core, core->tx_packet, size,
                            allocate_transfer_id(core),
                            RBSP_ROUTE_EVENT_BASE + core->node_id)) {
                object->input_event_begin = (uint16_t)(
                    (object->input_event_begin + 1U) %
                    CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY);
                --object->input_event_count;
            }
        }
    }
#else
    (void)core;
    (void)now_ms;
#endif
}

#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
bool rbsp_core_gpio_input_hint(rbsp_core_t* core, uint16_t pin) {
#if CONFIG_GPIO_RESOURCE_COUNT > 0
    if (core == NULL) return false;
    for (size_t index = 0U; index < CONFIG_GPIO_RESOURCE_COUNT; ++index) {
        rbsp_gpio_object_t* object = &core->gpio_objects[index];
        if (object->used && object->input_events_enabled &&
            object->pin == pin) {
            object->input_irq_hint = true;
            if (object->input_hint_matched != UINT32_MAX)
                ++object->input_hint_matched;
            return true;
        }
    }
#else
    (void)core;
    (void)pin;
#endif
    if (core != NULL && core->gpio_input_hint_ignored != UINT32_MAX)
        ++core->gpio_input_hint_ignored;
    return false;
}
#endif


void rbsp_core_poll(rbsp_core_t* core) {
    if (core == NULL) {
        return;
    }
    const uint32_t now = core->hal.milliseconds();
#if defined(CONFIG_REMOTEBSP_MOTION)
    (void)expire_stepgen_leases(core, now);
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    (void)expire_bus_leases(core, now);
#endif
    if (core->bootloader_request_pending &&
        (uint32_t)(now - core->bootloader_request_ms) >=
            RBSP_BOOTLOADER_RESET_DELAY_MS) {
        core->bootloader_request_pending = false;
        core->hal.enter_bootloader(core->bootloader_request_mode);
        return;
    }
    for (size_t index = 0;
         index < CONFIG_REMOTE_REASSEMBLY_SLOTS; ++index) {
        rbsp_reassembly_slot_t* slot = &core->reassembly[index];
        if (slot->active &&
            (uint32_t)(now - slot->last_update_ms) >=
                CONFIG_REMOTE_REASSEMBLY_TIMEOUT_MS) {
            slot->active = false;
        }
    }

    service_gpio_input_events(core, now);

#if CONFIG_UART_RESOURCE_COUNT > 0
    if (core->node_id != 0U && core->hal.uart_read != NULL) {
        for (size_t index = 0;
             index < CONFIG_UART_RESOURCE_COUNT; ++index) {
            rbsp_uart_object_t* object = &core->uart_objects[index];
            if (!object->used || !object->streaming) {
                continue;
            }
            if (object->pending_length < CONFIG_UART_EVENT_CHUNK_SIZE) {
                const size_t capacity =
                    CONFIG_UART_EVENT_CHUNK_SIZE -
                    object->pending_length;
                const size_t count = core->hal.uart_read(
                    object->port,
                    object->pending + object->pending_length,
                    capacity);
                if (count != 0U && count <= capacity) {
                    if (object->pending_length == 0U) {
                        object->first_byte_ms = now;
                    }
                    object->pending_length =
                        (uint16_t)(object->pending_length + count);
                }
            }
            const bool full =
                object->pending_length == CONFIG_UART_EVENT_CHUNK_SIZE;
            const bool expired =
                object->pending_length != 0U &&
                (uint32_t)(now - object->first_byte_ms) >=
                    CONFIG_UART_EVENT_FLUSH_MS;
            if (!full && !expired) {
                continue;
            }
            const uint32_t sequence = object->event_sequence + 1U;
            const uint16_t size = encode_packet(
                core->tx_packet, RBSP_MESSAGE_EVENT,
                RBSP_COMMAND_UART_RX_EVENT, 0U, sequence,
                object->object_id, 0U, object->pending,
                object->pending_length);
            if (send_packet(core, core->tx_packet, size,
                            allocate_transfer_id(core),
                            RBSP_ROUTE_EVENT_BASE + core->node_id)) {
                object->event_sequence = sequence;
                object->pending_length = 0U;
            }
        }
    }
#endif

    if (core->node_id != 0U &&
        (uint32_t)(now - core->last_heartbeat_ms) >=
            CONFIG_HEARTBEAT_INTERVAL_MS) {
        uint8_t* payload = core->tx_packet + RBSP_HEADER_SIZE;
        memcpy(payload, core->info.uuid, 16U);
        payload[16] = RBSP_PROTOCOL_VERSION;
        const uint16_t size = encode_packet(
            core->tx_packet, RBSP_MESSAGE_EVENT,
            RBSP_COMMAND_HEARTBEAT, 0U, 0U, core->node_id, 0U,
            payload, 17U);
        (void)send_packet(core, core->tx_packet, size,
                          allocate_transfer_id(core),
                          RBSP_ROUTE_EVENT_BASE + core->node_id);
        core->last_heartbeat_ms = now;
    }
}

#if defined(CONFIG_REMOTEBSP_MOTION)
bool rbsp_core_motion_service(rbsp_core_t* core) {
    if (core == NULL || !motion_available(core)) {
        return false;
    }
    const rbsp_motion_io_t io = {
        core->hal.motion_set_enable,
        core->hal.motion_set_direction,
        core->hal.motion_set_step,
        core->hal.motion_limit_active};
    uint64_t deadline_ns = RBSP_MOTION_NO_DEADLINE;
    const bool result = rbsp_motion_service(
        &core->motion, &io, core->hal.nanoseconds(),
        &deadline_ns);
    rbsp_motion_group_observe_motion(
        &core->motion_group, &core->motion);
    observe_motion_owner(core);
    if (!result) {
        if (core->motion_group.state == RBSP_MOTION_GROUP_ARMED) {
            (void)rbsp_motion_group_emergency_abort(&core->motion_group);
        }
        core->motion_owner_session_id = 0U;
        core->motion_resource_mask = 0U;
        core->hal.motion_cancel_compare();
        return false;
    }
    if (deadline_ns == RBSP_MOTION_NO_DEADLINE) {
        core->hal.motion_cancel_compare();
        return true;
    }
    if (!core->hal.motion_schedule_compare(deadline_ns)) {
        rbsp_motion_abort(
            &core->motion, RBSP_MOTION_FAULT_TIMING);
        if (core->motion_group.state == RBSP_MOTION_GROUP_ARMED) {
            (void)rbsp_motion_group_emergency_abort(&core->motion_group);
        }
        core->motion_owner_session_id = 0U;
        core->motion_resource_mask = 0U;
        (void)rbsp_motion_service(
            &core->motion, &io, core->hal.nanoseconds(), NULL);
        core->hal.motion_cancel_compare();
        return false;
    }
    return true;
}

bool rbsp_core_motion_tick(rbsp_core_t* core) {
    return rbsp_core_motion_service(core);
}
#endif

#if CONFIG_GPIO_RESOURCE_COUNT > 0 || CONFIG_UART_RESOURCE_COUNT > 0 || \
    defined(CONFIG_REMOTEBSP_PWM) || \
    defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM) || \
    defined(CONFIG_REMOTEBSP_MOTION) || defined(CONFIG_REMOTEBSP_BUS) || \
    defined(CONFIG_REMOTEBSP_TIMER) || defined(CONFIG_REMOTEBSP_STORAGE)
size_t rbsp_core_release_session(rbsp_core_t* core, uint32_t session_id) {
    if (core == NULL || session_id == 0U) {
        return 0U;
    }
    size_t released = 0U;
#if CONFIG_GPIO_RESOURCE_COUNT > 0
    for (size_t index = 0U; index < CONFIG_GPIO_RESOURCE_COUNT; ++index) {
        rbsp_gpio_object_t* object = &core->gpio_objects[index];
        if (!object->used || object->owner_session_id != session_id) {
            continue;
        }
        if (object->direction == RBSP_GPIO_OUTPUT &&
            (core->hal.gpio_write == NULL ||
             !core->hal.gpio_write(object->pin, false))) {
            /* 无法确认安全低电平时保留所有权和对象，供后续清理重试。 */
            continue;
        }
#ifdef CONFIG_REMOTEBSP_GPIO_EXTI
        if (object->input_events_enabled &&
            core->hal.gpio_input_event_configure != NULL &&
            !core->hal.gpio_input_event_configure(object->pin, false)) {
            continue;
        }
#endif
        memset(object, 0, sizeof(*object));
        ++released;
    }
#endif
#if CONFIG_UART_RESOURCE_COUNT > 0
    for (size_t index = 0U; index < CONFIG_UART_RESOURCE_COUNT; ++index) {
        rbsp_uart_object_t* object = &core->uart_objects[index];
        if (!object->used || object->owner_session_id != session_id) {
            continue;
        }
        if (core->hal.uart_reset == NULL ||
            !core->hal.uart_reset(object->port)) {
            core->uart_status[object->port].backend_failed = true;
            /* 底层仍可能持有缓冲或中断，保留对象供后续清理重试。 */
            continue;
        }
        memset(&core->uart_status[object->port], 0,
               sizeof(core->uart_status[object->port]));
        memset(object, 0, sizeof(*object));
        ++released;
    }
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
    for (size_t index = 0U; index < CONFIG_PWM_RESOURCE_COUNT; ++index) {
        rbsp_pwm_object_t* object = &core->pwm_objects[index];
        if (!object->used || object->owner_session_id != session_id) {
            continue;
        }
        if (core->hal.pwm_stop == NULL ||
            !core->hal.pwm_stop(object->channel)) {
            continue;
        }
        memset(object, 0, sizeof(*object));
        ++released;
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    for (size_t index = 0U;
         index < CONFIG_TIMED_BITSTREAM_RESOURCE_COUNT; ++index) {
        rbsp_timed_bitstream_object_t* object =
            &core->timed_bitstream_objects[index];
        if (!object->used || object->owner_session_id != session_id) {
            continue;
        }
        if (core->hal.timed_bitstream_abort == NULL ||
            !core->hal.timed_bitstream_abort(object->channel)) {
            continue;
        }
        memset(object, 0, sizeof(*object));
        ++released;
    }
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
    if (motion_available(core)) {
    const uint32_t critical_state = core->hal.motion_enter_critical();
    if ((core->motion_group.state == RBSP_MOTION_GROUP_PREPARED ||
         core->motion_group.state == RBSP_MOTION_GROUP_ARMED) &&
        rbsp_motion_group_owned_by(&core->motion_group, session_id)) {
        (void)rbsp_motion_group_emergency_abort(&core->motion_group);
    }
    core->hal.motion_exit_critical(critical_state);
    for (uint8_t axis = 0U; axis < core->motion.axis_count; ++axis) {
        rbsp_stepgen_lease_t* lease = &core->stepgen_leases[axis];
        if (!lease->active || lease->owner_session_id != session_id) {
            continue;
        }
        memset(lease, 0, sizeof(*lease));
        (void)stop_for_stepgen_lease_loss(core, axis, session_id);
        ++released;
    }
    if (core->motion_owner_session_id == session_id) {
        const uint32_t stop_critical_state =
            core->hal.motion_enter_critical();
        if (core->motion.size != 0U ||
            core->motion.state == RBSP_MOTION_ARMED ||
            core->motion.state == RBSP_MOTION_RUNNING) {
            rbsp_motion_abort(&core->motion, RBSP_MOTION_FAULT_ABORTED);
        }
        core->motion_owner_session_id = 0U;
        core->motion_resource_mask = 0U;
        core->hal.motion_exit_critical(stop_critical_state);
        (void)rbsp_core_motion_service(core);
    }
    }
#endif
#if defined(CONFIG_REMOTEBSP_BUS)
    for (size_t index = 0U; index < core->hal.bus_resource_count; ++index) {
        rbsp_bus_lease_t* lease = &core->bus_leases[index];
        if (lease->active && lease->owner_session_id == session_id) {
            memset(lease, 0, sizeof(*lease));
            ++released;
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_TIMER)
    for (size_t index = 0U; index < core->hal.timer_resource_count; ++index) {
        rbsp_static_resource_lease_t* lease = &core->timer_leases[index];
        if (lease->active && lease->owner_session_id == session_id) {
            memset(lease, 0, sizeof(*lease)); ++released;
        }
    }
#endif
#if defined(CONFIG_REMOTEBSP_STORAGE)
    for (size_t index = 0U; index < core->hal.storage_resource_count; ++index) {
        rbsp_static_resource_lease_t* lease = &core->storage_leases[index];
        if (lease->active && lease->owner_session_id == session_id) {
            memset(lease, 0, sizeof(*lease)); ++released;
        }
    }
#endif
    for (size_t index = 0U;
         index < CONFIG_REMOTE_REQUEST_CACHE_ENTRIES; ++index) {
        if (core->cache[index].valid &&
            core->cache[index].session_id == session_id) {
            core->cache[index].valid = false;
        }
    }
    return released;
}
#endif

void rbsp_core_accept_link(rbsp_core_t* core,
                           const rbsp_link_frame_t* frame) {
    if (core == NULL || frame == NULL) {
        return;
    }
    const uint8_t mtu = link_mtu(core->link_mode);
    const uint8_t capacity =
        (uint8_t)(mtu - RBSP_FRAGMENT_HEADER_SIZE);
    const uint32_t assigned_request_id =
        RBSP_ROUTE_REQUEST_BASE + core->node_id;
    if (frame->length <= RBSP_FRAGMENT_HEADER_SIZE ||
        frame->length > mtu ||
        (frame->route != RBSP_ROUTE_DISCOVERY &&
         (core->node_id == 0U ||
          frame->route != assigned_request_id))) {
        return;
    }

    const uint16_t transfer_id = get_u16(frame->data);
    const uint16_t sequence = get_u16(frame->data + 2U);
    const uint8_t flags =
        frame->data[4] & RBSP_FRAGMENT_FLAG_MASK;
    const uint8_t payload_length =
        frame->data[4] >> RBSP_FRAGMENT_LENGTH_SHIFT;
    if (payload_length == 0U || payload_length > capacity ||
        (uint16_t)RBSP_FRAGMENT_HEADER_SIZE + payload_length >
            frame->length ||
        (((flags & RBSP_FRAGMENT_FIRST) != 0U) !=
         (sequence == 0U))) {
        return;
    }
    for (uint8_t index =
             (uint8_t)(RBSP_FRAGMENT_HEADER_SIZE + payload_length);
         index < frame->length; ++index) {
        if (frame->data[index] != 0U) {
            return;
        }
    }
    if ((flags & RBSP_FRAGMENT_LAST) == 0U &&
        payload_length != capacity) {
        return;
    }

    rbsp_reassembly_slot_t* slot =
        find_slot(core, frame->route, transfer_id);
    if (slot == NULL) {
        if ((flags & RBSP_FRAGMENT_FIRST) == 0U) {
            return;
        }
        slot = allocate_slot(core);
        if (slot == NULL) {
            return;
        }
        slot->stream_id = frame->route;
        slot->transfer_id = transfer_id;
    }

    if (sequence < slot->next_sequence) {
        const uint32_t offset = (uint32_t)sequence * capacity;
        if (offset + payload_length <= slot->size &&
            memcmp(slot->packet + offset,
                   frame->data + RBSP_FRAGMENT_HEADER_SIZE,
                   payload_length) == 0) {
            slot->last_update_ms = core->hal.milliseconds();
        }
        return;
    }
    if (sequence != slot->next_sequence ||
        (uint32_t)slot->size + payload_length >
            CONFIG_REMOTE_MAX_PACKET_SIZE) {
        slot->active = false;
        return;
    }

    memcpy(slot->packet + slot->size,
           frame->data + RBSP_FRAGMENT_HEADER_SIZE,
           payload_length);
    slot->size = (uint16_t)(slot->size + payload_length);
    slot->next_sequence++;
    slot->last_update_ms = core->hal.milliseconds();

    if ((flags & RBSP_FRAGMENT_LAST) != 0U) {
        rbsp_request_t request;
        if (decode_request(slot->packet, slot->size, &request)) {
            (void)process_request(core, &request);
        }
        slot->active = false;
    }
}

void rbsp_core_accept_can(rbsp_core_t* core,
                          const rbsp_can_frame_t* frame) {
    rbsp_core_accept_link(core, frame);
}
