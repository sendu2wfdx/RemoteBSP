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
    RBSP_COMMAND_GPIO_CREATE = 0x0100,
    RBSP_COMMAND_GPIO_READ = 0x0101,
    RBSP_COMMAND_GPIO_WRITE = 0x0102,
    RBSP_COMMAND_UART_CREATE = 0x0200,
    RBSP_COMMAND_UART_READ = 0x0201,
    RBSP_COMMAND_UART_WRITE = 0x0202,
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
};

static const uint8_t bootloader_confirmation[8] = {
    'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};

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

static bool send_packet(rbsp_core_t* core, const uint8_t* packet,
                        uint16_t packet_size, uint16_t transfer_id,
                        uint32_t can_id) {
    const uint8_t mtu =
        core->can_mode == RBSP_CAN_FD ? 64U : 8U;
    const uint8_t capacity =
        (uint8_t)(mtu - RBSP_FRAGMENT_HEADER_SIZE);
    uint16_t sequence = 0U;
    uint16_t offset = 0U;

    while (offset < packet_size) {
        const uint16_t remaining = (uint16_t)(packet_size - offset);
        const uint8_t length =
            remaining > capacity ? capacity : (uint8_t)remaining;
        rbsp_can_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.identifier = can_id;
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
        if (core->can_mode == RBSP_CAN_FD) {
            frame.length = canonical_fd_length(frame.length);
        }
        if (!core->hal.can_send(&frame)) {
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
        return RBSP_CAN_ID_PROVISIONAL_BASE +
               CONFIG_NODE_PROVISIONAL_ID;
    }
    return RBSP_CAN_ID_RESPONSE_BASE + core->node_id;
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

#if CONFIG_UART_RESOURCE_COUNT > 0
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
            if (core->hal.gpio_configure != NULL &&
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
            if (core->hal.enter_bootloader != NULL) {
                capabilities |= 1ULL << 8U;
            }
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
            if (core->hal.gpio_configure == NULL ||
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
            } else if (!core->hal.gpio_configure(
                           pin,
                           (rbsp_gpio_direction_t)request->payload[2],
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
                request->payload_length != 8U ||
                get_u32(request->payload + 1U) == 0U ||
                request->payload[5] < 5U ||
                request->payload[5] > 8U ||
                (request->payload[6] != 1U &&
                 request->payload[6] != 2U) ||
                request->payload[7] > 2U) {
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
            if (request->payload[0] >= CONFIG_UART_RESOURCE_COUNT) {
                response_size = make_status_response(
                    core, request, RBSP_STATUS_OBJECT_NOT_FOUND,
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
                    rbsp_can_mode_t mode,
                    const rbsp_node_info_t* info) {
    if (core == NULL || hal == NULL || info == NULL ||
        hal->can_send == NULL || hal->milliseconds == NULL ||
        (mode != RBSP_CAN_CLASSICAL && mode != RBSP_CAN_FD)) {
        return false;
    }
    memset(core, 0, sizeof(*core));
    core->hal = *hal;
    core->can_mode = mode;
    core->info = *info;
    core->next_object_id = 1U;
    core->next_transfer_id = 1U;
    core->last_heartbeat_ms = hal->milliseconds();
    return true;
}

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
                          RBSP_CAN_ID_EVENT_BASE + core->node_id);
        core->last_heartbeat_ms = now;
    }
}

void rbsp_core_accept_can(rbsp_core_t* core,
                          const rbsp_can_frame_t* frame) {
    if (core == NULL || frame == NULL) {
        return;
    }
    const uint8_t mtu =
        core->can_mode == RBSP_CAN_FD ? 64U : 8U;
    const uint8_t capacity =
        (uint8_t)(mtu - RBSP_FRAGMENT_HEADER_SIZE);
    const uint32_t assigned_request_id =
        RBSP_CAN_ID_REQUEST_BASE + core->node_id;
    if (frame->length <= RBSP_FRAGMENT_HEADER_SIZE ||
        frame->length > mtu ||
        (frame->identifier != RBSP_CAN_ID_DISCOVERY &&
         (core->node_id == 0U ||
          frame->identifier != assigned_request_id))) {
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
        find_slot(core, frame->identifier, transfer_id);
    if (slot == NULL) {
        if ((flags & RBSP_FRAGMENT_FIRST) == 0U) {
            return;
        }
        slot = allocate_slot(core);
        if (slot == NULL) {
            return;
        }
        slot->stream_id = frame->identifier;
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
