#include "remotebsp_embedded/core.h"

#include <string.h>

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
    RBSP_COMMAND_TIME_SYNC = 0x0020,
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
    RBSP_COMMAND_UART_CREATE = 0x0200,
    RBSP_COMMAND_UART_READ = 0x0201,
    RBSP_COMMAND_UART_WRITE = 0x0202,
    RBSP_COMMAND_UART_RX_EVENT = 0x0280,
    RBSP_COMMAND_PWM_CREATE = 0x0600,
    RBSP_COMMAND_PWM_WRITE = 0x0601,
    RBSP_COMMAND_PWM_STOP = 0x0602,
    RBSP_COMMAND_TIMED_BITSTREAM_CREATE = 0x0700,
    RBSP_COMMAND_TIMED_BITSTREAM_WRITE = 0x0701,
    RBSP_COMMAND_TIMED_BITSTREAM_ABORT = 0x0702,
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
    RBSP_FRAGMENT_FIRST = 0x01,
    RBSP_FRAGMENT_LAST = 0x02,
    RBSP_FRAGMENT_FLAG_MASK = 0x03,
    RBSP_FRAGMENT_LENGTH_SHIFT = 2,
    RBSP_BOOTLOADER_RESET_DELAY_MS = 100,
#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
    RBSP_DEVICE_PARAM_PROTOCOL_VERSION = 1,
    RBSP_DEVICE_PARAM_STATUS_SIZE = 16,
    RBSP_DEVICE_PARAM_UNLOCK_CONFIRMATION = 0x50564252,
    RBSP_DEVICE_PARAM_UNLOCK_TIME_MS = 60000,
    RBSP_DEVICE_PARAM_STATUS_UNLOCKED = 1U << 0,
    RBSP_DEVICE_PARAM_STATUS_RESTART_REQUIRED = 1U << 1,
#endif
};

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
}
#endif

#if defined(CONFIG_REMOTEBSP_MOTION)
static uint64_t get_u64(const uint8_t* input) {
    uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= (uint64_t)input[index] << (index * 8U);
    }
    return value;
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
    uint64_t node_start_tick, rbsp_motion_segment_t* segment) {
    if (length < 24U) {
        return false;
    }
    const uint8_t axis_count = input[21U];
    if (axis_count == 0U || axis_count > core->motion.axis_count ||
        length != (uint16_t)(24U + (uint16_t)axis_count * 8U) ||
        input[20U] > 1U || get_u16(input + 22U) != 0U) {
        return false;
    }
    memset(segment, 0, sizeof(*segment));
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
static bool process_request(rbsp_core_t* core,
                            const rbsp_request_t* request) {
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
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
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
            } else if (object->streaming) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_ACCESS_DENIED,
                    request->object_id, NULL, 0U);
            } else {
                const size_t count = core->hal.uart_read(
                    object->port, payload + 1U, requested);
                if (count > requested) {
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
            } else if (!core->hal.uart_write(
                           object->port, request->payload,
                           request->payload_length)) {
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
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
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
            } else if (!core->hal.pwm_write(object->channel, duty)) {
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
            } else if (!core->hal.pwm_stop(object->channel)) {
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
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_FAILED,
                    0U, NULL, 0U);
            } else {
                free_object->used = true;
                free_object->object_id = core->next_object_id++;
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
            } else if (core->hal.timed_bitstream_busy != NULL &&
                       core->hal.timed_bitstream_busy(object->channel)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_RESOURCE_BUSY,
                    request->object_id, NULL, 0U);
            } else if (!core->hal.timed_bitstream_write(
                           object->channel, request->payload + 2U,
                           bit_count)) {
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
            } else if (!core->hal.timed_bitstream_abort(object->channel)) {
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
                segment.steps[axis] =
                    (int32_t)get_u32(entry + 4U);
            }
            if (!valid) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
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
                start_scheduler = core->motion.size == 0U;
                result = rbsp_motion_commit_segment(
                    &core->motion, &segment,
                    core->hal.nanoseconds(), &accepted);
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
                    segment_length, identity.node_start_tick, &segment)) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
                    0U, NULL, 0U);
                break;
            }
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            const rbsp_motion_group_ready_code_t code =
                rbsp_motion_group_prepare(
                    &core->motion_group, &core->motion, &identity, &segment,
                    request->session_id, core->hal.nanoseconds());
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
            const uint32_t critical_state =
                core->hal.motion_enter_critical();
            rbsp_motion_group_commit_code_t code = rbsp_motion_group_commit(
                &core->motion_group, &core->motion, &identity,
                request->session_id, core->hal.nanoseconds(), NULL);
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
    core->next_object_id = 1U;
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
    core->default_motion_axis_count = hal->motion_axis_count;
#endif
    core->next_transfer_id = 1U;
    core->last_heartbeat_ms = hal->milliseconds();
    return true;
}

#if defined(CONFIG_REMOTEBSP_DEVICE_PARAMS)
bool rbsp_core_device_params_init(
    rbsp_core_t* core, const rbsp_device_param_backend* backend) {
    rbsp_device_param_record uuid;
    if (core == NULL || backend == NULL ||
        !rbsp_device_param_store_init(&core->device_params, backend) ||
        !rbsp_device_param_store_boot(&core->device_params)) {
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


void rbsp_core_poll(rbsp_core_t* core) {
    if (core == NULL) {
        return;
    }
    const uint32_t now = core->hal.milliseconds();
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
    if (!result || deadline_ns == RBSP_MOTION_NO_DEADLINE) {
        core->hal.motion_cancel_compare();
        return result;
    }
    if (!core->hal.motion_schedule_compare(deadline_ns)) {
        rbsp_motion_abort(
            &core->motion, RBSP_MOTION_FAULT_TIMING);
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
