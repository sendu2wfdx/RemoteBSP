#include "remotebsp_embedded/core.h"
#include "remotebsp_embedded/byte_ring.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static rbsp_can_frame_t sent_frames[1024];
static size_t sent_count;
static uint32_t now_ms;
static bool gpio_values[256];
static unsigned gpio_write_count;
static unsigned uart_read_count;

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

static uint16_t make_request(uint8_t* packet, uint16_t command,
                             uint32_t request_id, uint32_t object_id,
                             const uint8_t* payload,
                             uint16_t payload_length) {
    const uint16_t size = (uint16_t)(24U + payload_length);
    memset(packet, 0, size);
    packet[0] = 1U;
    packet[1] = 1U;
    put_u16(packet + 2U, command);
    put_u32(packet + 4U, 0x12345678U);
    put_u32(packet + 8U, request_id);
    put_u32(packet + 12U, object_id);
    put_u16(packet + 16U, payload_length);
    if (payload_length != 0U) {
        memcpy(packet + 24U, payload, payload_length);
    }
    uint32_t crc = crc32_update(0xFFFFFFFFU, packet, 20U);
    crc = crc32_update(crc, packet + 24U, payload_length);
    put_u32(packet + 20U, ~crc);
    return size;
}

static bool fake_can_send(const rbsp_can_frame_t* frame) {
    assert(sent_count < sizeof(sent_frames) / sizeof(sent_frames[0]));
    sent_frames[sent_count++] = *frame;
    return true;
}

static uint32_t fake_milliseconds(void) {
    return now_ms;
}

static bool fake_gpio_configure(uint16_t pin,
                                rbsp_gpio_direction_t direction,
                                bool initial_value) {
    (void)direction;
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0])) {
        return false;
    }
    gpio_values[pin] = initial_value;
    return true;
}

static bool fake_gpio_write(uint16_t pin, bool value) {
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0])) {
        return false;
    }
    gpio_values[pin] = value;
    ++gpio_write_count;
    return true;
}

static bool fake_gpio_read(uint16_t pin, bool* value) {
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0])) {
        return false;
    }
    *value = gpio_values[pin];
    return true;
}

static size_t fake_uart_read(uint8_t port, uint8_t* data,
                             size_t capacity) {
    (void)port;
    memset(data, 0x5AU, capacity);
    ++uart_read_count;
    return capacity;
}

static bool fake_uart_configure(uint8_t port, uint32_t baud_rate,
                                uint8_t data_bits, uint8_t stop_bits,
                                uint8_t parity) {
    return port < 2U && baud_rate != 0U && data_bits == 8U &&
           stop_bits == 1U && parity == 0U;
}

static bool fake_uart_write(uint8_t port, const uint8_t* data,
                            size_t length) {
    return port < 2U && data != NULL && length != 0U;
}

static void feed_packet(rbsp_core_t* core, uint32_t can_id,
                        uint16_t transfer_id, const uint8_t* packet,
                        uint16_t packet_size) {
    const uint8_t mtu =
        core->can_mode == RBSP_CAN_FD ? 64U : 8U;
    const uint8_t capacity = (uint8_t)(mtu - 5U);
    uint16_t offset = 0U;
    uint16_t sequence = 0U;
    while (offset < packet_size) {
        const uint16_t remaining = (uint16_t)(packet_size - offset);
        const uint8_t length =
            remaining > capacity ? capacity : (uint8_t)remaining;
        rbsp_can_frame_t frame = {0};
        frame.identifier = can_id;
        put_u16(frame.data, transfer_id);
        put_u16(frame.data + 2U, sequence);
        frame.data[4] = (uint8_t)(length << 2U);
        if (sequence == 0U) {
            frame.data[4] |= 1U;
        }
        if ((uint16_t)(offset + length) == packet_size) {
            frame.data[4] |= 2U;
        }
        memcpy(frame.data + 5U, packet + offset, length);
        frame.length = (uint8_t)(5U + length);
        rbsp_core_accept_can(core, &frame);
        offset = (uint16_t)(offset + length);
        ++sequence;
    }
}

static uint16_t reassemble_sent(uint8_t* packet, uint32_t expected_id) {
    uint16_t size = 0U;
    for (size_t index = 0; index < sent_count; ++index) {
        const rbsp_can_frame_t* frame = &sent_frames[index];
        assert(frame->identifier == expected_id);
        const uint8_t length = frame->data[4] >> 2U;
        memcpy(packet + size, frame->data + 5U, length);
        size = (uint16_t)(size + length);
    }
    assert(size >= 24U);
    assert((uint16_t)(24U +
           (uint16_t)(packet[16] | ((uint16_t)packet[17] << 8U))) ==
           size);
    const uint32_t received_crc = get_u32(packet + 20U);
    uint32_t crc = crc32_update(0xFFFFFFFFU, packet, 20U);
    crc = crc32_update(crc, packet + 24U, size - 24U);
    assert(received_crc == ~crc);
    return size;
}

static void clear_sent(void) {
    sent_count = 0U;
    memset(sent_frames, 0, sizeof(sent_frames));
}

static void test_byte_ring(void) {
    uint8_t storage[8];
    rbsp_byte_ring_t ring;
    assert(rbsp_byte_ring_init(&ring, storage, sizeof(storage)));
    assert(rbsp_byte_ring_size(&ring) == 0U);
    assert(rbsp_byte_ring_free(&ring) == 7U);

    const uint8_t first[] = {1U, 2U, 3U, 4U, 5U};
    assert(rbsp_byte_ring_write_exact(
        &ring, first, sizeof(first)));
    assert(rbsp_byte_ring_size(&ring) == sizeof(first));

    uint8_t output[8] = {0U};
    assert(rbsp_byte_ring_read(&ring, output, 3U) == 3U);
    assert(memcmp(output, first, 3U) == 0);

    const uint8_t wrapped[] = {6U, 7U, 8U, 9U, 10U};
    assert(rbsp_byte_ring_write_exact(
        &ring, wrapped, sizeof(wrapped)));
    assert(rbsp_byte_ring_size(&ring) == 7U);

    /*
     * 空间不足时必须整段拒绝，不能留下半条 UART_WRITE 数据。
     */
    const uint8_t rejected[] = {11U, 12U};
    assert(!rbsp_byte_ring_write_exact(
        &ring, rejected, sizeof(rejected)));
    assert(rbsp_byte_ring_size(&ring) == 7U);

    assert(rbsp_byte_ring_read(&ring, output, sizeof(output)) == 7U);
    const uint8_t expected[] = {4U, 5U, 6U, 7U, 8U, 9U, 10U};
    assert(memcmp(output, expected, sizeof(expected)) == 0);
    assert(rbsp_byte_ring_size(&ring) == 0U);
}

int main(void) {
    test_byte_ring();

    rbsp_core_t core;
    const rbsp_hal_t hal = {
        fake_can_send, fake_milliseconds,
        fake_gpio_configure, fake_gpio_write, fake_gpio_read,
        fake_uart_configure, fake_uart_read, fake_uart_write};
    rbsp_node_info_t info = {
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
         0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
        0, 1, 0, 0x0103CB};
    assert(rbsp_core_init(&core, &hal, RBSP_CAN_CLASSICAL, &info));

    uint8_t request[1024];
    uint8_t response[1024];
    const uint8_t discovery[] = {1U, 1U};
    uint16_t request_size =
        make_request(request, 0x0001U, 1U, 0U,
                     discovery, sizeof(discovery));
    feed_packet(&core, 0x700U, 1U, request, request_size);
    assert(reassemble_sent(response, 0x481U) == 51U);
    assert(response[2] == 0x02U && response[3] == 0x00U);
    assert(memcmp(response + 24U, info.uuid, 16U) == 0);

    clear_sent();
    uint8_t assignment[20];
    memcpy(assignment, info.uuid, 16U);
    put_u32(assignment + 16U, 25U);
    request_size = make_request(request, 0x0003U, 2U, 0U,
                                assignment, sizeof(assignment));
    feed_packet(&core, 0x700U, 2U, request, request_size);
    assert(core.node_id == 25U);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24] == 0U);

    clear_sent();
    const uint8_t create[] = {7U, 0U, 1U, 0U};
    request_size = make_request(request, 0x0100U, 3U, 0U,
                                create, sizeof(create));
    feed_packet(&core, 0x619U, 3U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t gpio_object = get_u32(response + 12U);
    assert(gpio_object != 0U);

    clear_sent();
    const uint8_t high[] = {1U};
    request_size = make_request(request, 0x0102U, 4U, gpio_object,
                                high, sizeof(high));
    feed_packet(&core, 0x619U, 4U, request, request_size);
    assert(gpio_write_count == 1U && gpio_values[7]);
    clear_sent();
    feed_packet(&core, 0x619U, 5U, request, request_size);
    assert(gpio_write_count == 1U);
    reassemble_sent(response, 0x599U);

    /*
     * 1024 字节测试配置下，999 字节 PING 产生恰好 1024 字节的响应。
     * 它超过 256 字节去重缓存，但 PING 无副作用，可以安全地重新计算。
     */
    clear_sent();
    uint8_t large_ping[999];
    memset(large_ping, 0xA5, sizeof(large_ping));
    request_size = make_request(request, 0x0012U, 5U, 0U,
                                large_ping, sizeof(large_ping));
    feed_packet(&core, 0x619U, 6U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 1024U);
    assert(response[24] == 0U);
    assert(memcmp(response + 25U, large_ping, sizeof(large_ping)) == 0);
    for (size_t index = 0;
         index < CONFIG_REMOTE_REQUEST_CACHE_ENTRIES; ++index) {
        assert(!core.cache[index].valid ||
               core.cache[index].request_id != 5U);
    }

    clear_sent();
    feed_packet(&core, 0x619U, 7U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 1024U);

    clear_sent();
    uint8_t uart_config[8] = {0U};
    put_u32(uart_config + 1U, 115200U);
    uart_config[5] = 8U;
    uart_config[6] = 1U;
    request_size = make_request(request, 0x0200U, 6U, 0U,
                                uart_config, sizeof(uart_config));
    feed_packet(&core, 0x619U, 8U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t uart_object = get_u32(response + 12U);
    assert(response[24] == 0U && uart_object != 0U);

    /*
     * UART_READ 会消费接收数据，因此最大响应必须完整装入去重缓存。
     * 256 字节缓存扣除 24 字节头和状态字节后，单次最多读取 231 字节。
     */
    clear_sent();
    uint8_t uart_read_length[2];
    put_u16(uart_read_length, 231U);
    request_size = make_request(request, 0x0201U, 7U, uart_object,
                                uart_read_length,
                                sizeof(uart_read_length));
    feed_packet(&core, 0x619U, 9U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 256U);
    assert(response[24] == 0U && uart_read_count == 1U);

    clear_sent();
    feed_packet(&core, 0x619U, 10U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 256U);
    assert(uart_read_count == 1U);

    clear_sent();
    put_u16(uart_read_length, 232U);
    request_size = make_request(request, 0x0201U, 8U, uart_object,
                                uart_read_length,
                                sizeof(uart_read_length));
    feed_packet(&core, 0x619U, 11U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24] == 2U && uart_read_count == 1U);

    clear_sent();
    now_ms = 500U;
    rbsp_core_poll(&core);
    reassemble_sent(response, 0x519U);
    assert(response[1] == 3U);
    assert(response[2] == 4U);

    puts("嵌入式远程核心测试通过");
    return 0;
}
