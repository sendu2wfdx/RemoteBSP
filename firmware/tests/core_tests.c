#include "remotebsp_embedded/core.h"
#include "remotebsp_embedded/byte_ring.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 标准 assert 在 Release/NDEBUG 下会消失；固件安全断言必须始终执行。 */
#undef assert
#define assert(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: 测试断言失败: %s\n",              \
                          __FILE__, __LINE__, #condition);                    \
            abort();                                                          \
        }                                                                     \
    } while (0)

static rbsp_can_frame_t sent_frames[1024];
static size_t sent_count;
static uint32_t now_ms;
static bool gpio_values[256];
static rbsp_gpio_direction_t gpio_directions[256];
static rbsp_gpio_pull_t gpio_pulls[256];
static unsigned gpio_configure_count;
static unsigned gpio_write_count;
static bool gpio_write_must_fail;
static unsigned uart_read_count;
static unsigned uart_write_count;
static unsigned uart_reset_count;
static bool uart_reset_must_fail;
#if defined(CONFIG_REMOTEBSP_BUS)
enum {
    TEST_I2C_BUS_ID = 0x0B000001U,
    TEST_I2C_DEVICE_ID = 0x0C000001U,
};

static const rbsp_bus_resource_config_t test_bus_resources[] = {
    {TEST_I2C_BUS_ID, 0U, 400000U, 100U, 100000U, 1000U,
     128U, 1U, 0U, 1U, RBSP_BUS_I2C_BUS,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START, 0U, 0U},
    {TEST_I2C_DEVICE_ID, TEST_I2C_BUS_ID, 400000U, 100U, 100000U, 1000U,
     128U, 1U, 0x48U, 1U, RBSP_BUS_I2C_DEVICE,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START, 0U, 0U},
};
#endif
static uint16_t pwm_duty[2];
static bool pwm_stopped[2];
static uint16_t timed_bit_count;
static uint8_t timed_bit_data[96];
static bool timed_bit_busy;
static unsigned timed_bitstream_write_count;
static unsigned timed_bitstream_abort_count;
static unsigned pwm_stop_count;
static bool pwm_stop_must_fail;
static bool timed_bitstream_abort_must_fail;
static unsigned bootloader_enter_count;
static rbsp_bootloader_mode_t last_bootloader_mode;
#if defined(CONFIG_REMOTEBSP_MOTION)
static bool motion_schedule_allowed = true;
#endif
#if defined(CONFIG_REMOTEBSP_SOFT_HALF_DUPLEX_UART)
static rbsp_runtime_tmc_uart_config_t applied_tmc_uart[
    CONFIG_SOFT_HALF_DUPLEX_UART_PORT_COUNT];
static uint8_t applied_tmc_uart_count;
static unsigned tmc_uart_apply_count;
#endif

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
    for (unsigned index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)(value >> (index * 8U));
    }
}


static uint32_t get_u32(const uint8_t* input) {
    return (uint32_t)input[0] |
           ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) |
           ((uint32_t)input[3] << 24U);
}

static uint16_t get_u16(const uint8_t* input) {
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8U);
}

static uint64_t get_u64(const uint8_t* input) {
    uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= (uint64_t)input[index] << (index * 8U);
    }
    return value;
}

#if defined(CONFIG_REMOTEBSP_MOTION)
static void make_motion_group_identity(uint8_t output[80U],
                                       uint64_t boot_epoch,
                                       uint64_t node_start_tick) {
    memset(output, 0, 80U);
    put_u16(output, 1U);
    put_u64(output + 4U, 9001U);
    put_u32(output + 12U, 77U);
    put_u32(output + 16U, 1U);
    put_u64(output + 24U, boot_epoch);
    put_u64(output + 32U, 9U);
    put_u64(output + 40U, node_start_tick);
    output[48U] = 1U;
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

static uint16_t make_request_for_session(
    uint8_t* packet, uint16_t command, uint32_t session_id,
    uint32_t request_id, uint32_t object_id, const uint8_t* payload,
    uint16_t payload_length) {
    const uint16_t size = (uint16_t)(24U + payload_length);
    memset(packet, 0, size);
    packet[0] = 1U;
    packet[1] = 1U;
    put_u16(packet + 2U, command);
    put_u32(packet + 4U, session_id);
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

static uint16_t make_request(uint8_t* packet, uint16_t command,
                             uint32_t request_id, uint32_t object_id,
                             const uint8_t* payload,
                             uint16_t payload_length) {
    return make_request_for_session(
        packet, command, 0x12345678U, request_id, object_id,
        payload, payload_length);
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
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0])) {
        return false;
    }
    gpio_values[pin] = initial_value;
    gpio_directions[pin] = direction;
    gpio_pulls[pin] = RBSP_GPIO_FLOATING;
    ++gpio_configure_count;
    return true;
}

static bool fake_gpio_configure_pull(uint16_t pin,
                                     rbsp_gpio_direction_t direction,
                                     rbsp_gpio_pull_t pull,
                                     bool initial_value) {
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0]) ||
        pull > RBSP_GPIO_PULL_DOWN) {
        return false;
    }
    gpio_values[pin] = initial_value;
    gpio_directions[pin] = direction;
    gpio_pulls[pin] = pull;
    ++gpio_configure_count;
    return true;
}

static bool fake_gpio_write(uint16_t pin, bool value) {
    if (pin >= sizeof(gpio_values) / sizeof(gpio_values[0])) {
        return false;
    }
    if (gpio_write_must_fail) {
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

#if defined(CONFIG_REMOTEBSP_MOTION)
static uint64_t fake_nanoseconds(void) {
    return (uint64_t)now_ms * 1000000ULL;
}

static uint64_t fake_motion_boot_epoch(void) {
    return 31U;
}

static bool fake_motion_set_enable(uint8_t axis, bool enabled) {
    (void)enabled;
    return axis < CONFIG_MOTION_MAX_AXES;
}

static bool fake_motion_set_direction(uint8_t axis, bool positive) {
    (void)positive;
    return axis < CONFIG_MOTION_MAX_AXES;
}

static bool fake_motion_set_step(uint8_t axis, bool high) {
    (void)high;
    return axis < CONFIG_MOTION_MAX_AXES;
}

static bool fake_motion_limit_active(uint8_t axis, bool* active) {
    if (axis >= CONFIG_MOTION_MAX_AXES || active == NULL) {
        return false;
    }
    *active = false;
    return true;
}

static bool fake_motion_schedule_compare(uint64_t deadline_ns) {
    return motion_schedule_allowed &&
           deadline_ns != RBSP_MOTION_NO_DEADLINE;
}

static void fake_motion_cancel_compare(void) {
}

static uint32_t fake_motion_enter_critical(void) {
    return 0U;
}

static void fake_motion_exit_critical(uint32_t state) {
    (void)state;
}

#endif


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
    if (port >= 2U || data == NULL || length == 0U) {
        return false;
    }
    ++uart_write_count;
    return true;
}

static bool fake_uart_reset(uint8_t port) {
    ++uart_reset_count;
    return port < 2U && !uart_reset_must_fail;
}

#if defined(CONFIG_REMOTEBSP_BUS)
static rbsp_bus_transaction_status_t fake_i2c_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, const uint8_t* write_data, uint16_t write_length,
    uint8_t* read_data, uint16_t read_length,
    uint16_t* transmitted, uint16_t* received) {
    (void)flags;
    (void)write_data;
    assert(device->resource_id == TEST_I2C_DEVICE_ID);
    assert(timeout_us >= 100U);
    *transmitted = write_length;
    *received = read_length;
    memset(read_data, 0xA5, read_length);
    return RBSP_BUS_TRANSACTION_OK;
}
#endif


static bool fake_pwm_configure(uint8_t channel, uint32_t frequency_hz,
                               uint16_t duty, bool active_low) {
    (void)active_low;
    if (channel >= 2U || frequency_hz == 0U || duty > 10000U) {
        return false;
    }
    pwm_duty[channel] = duty;
    pwm_stopped[channel] = false;
    return true;
}

static bool fake_pwm_write(uint8_t channel, uint16_t duty) {
    if (channel >= 2U || duty > 10000U) {
        return false;
    }
    pwm_duty[channel] = duty;
    pwm_stopped[channel] = false;
    return true;
}

static bool fake_pwm_stop(uint8_t channel) {
    if (channel >= 2U || pwm_stop_must_fail) {
        return false;
    }
    ++pwm_stop_count;
    pwm_duty[channel] = 0U;
    pwm_stopped[channel] = true;
    return true;
}

static bool fake_resource_status(
    uint8_t resource_type, uint16_t instance,
    rbsp_resource_runtime_status_t* status) {
    if (status == NULL || resource_type != 2U || instance != 0U) {
        return false;
    }
    status->rx_buffered = 7U;
    status->tx_buffered = 3U;
    status->rx_overruns = 2U;
    return true;
}


static bool fake_timed_bitstream_configure(
    uint8_t channel, uint32_t bit_period_ns, uint32_t zero_high_ns,
    uint32_t one_high_ns, uint32_t reset_time_us) {
    return channel == 0U && bit_period_ns != 0U && zero_high_ns != 0U &&
           one_high_ns != 0U && zero_high_ns < bit_period_ns &&
           one_high_ns < bit_period_ns && reset_time_us != 0U;
}

static bool fake_timed_bitstream_write(uint8_t channel,
                                       const uint8_t* data,
                                       uint16_t bit_count) {
    const size_t size = (bit_count + 7U) / 8U;
    if (channel != 0U || data == NULL || bit_count == 0U ||
        size > sizeof(timed_bit_data)) {
        return false;
    }
    memcpy(timed_bit_data, data, size);
    timed_bit_count = bit_count;
    timed_bit_busy = true;
    ++timed_bitstream_write_count;
    return true;
}

static bool fake_timed_bitstream_busy(uint8_t channel) {
    return channel == 0U && timed_bit_busy;
}

static bool fake_timed_bitstream_abort(uint8_t channel) {
    if (channel != 0U || timed_bitstream_abort_must_fail) {
        return false;
    }
    ++timed_bitstream_abort_count;
    timed_bit_busy = false;
    return true;
}


static void fake_enter_bootloader(rbsp_bootloader_mode_t mode) {
    last_bootloader_mode = mode;
    ++bootloader_enter_count;
}

static void feed_packet(rbsp_core_t* core, uint32_t can_id,
                        uint16_t transfer_id, const uint8_t* packet,
                        uint16_t packet_size) {
    const uint8_t mtu =
        core->link_mode == RBSP_CAN_FD ? 64U : 8U;
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
        if (frame->identifier != expected_id) {
            fprintf(stderr,
                    "CAN 响应标识符不匹配：帧 %zu，实际 0x%03lX，期望 0x%03lX\n",
                    index, (unsigned long)frame->identifier,
                    (unsigned long)expected_id);
        }
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

static uint8_t exchange_status(rbsp_core_t* core, uint16_t command,
                               uint32_t session_id, uint32_t request_id,
                               uint32_t object_id, const uint8_t* payload,
                               uint16_t payload_length,
                               uint8_t response[1024]) {
    uint8_t request[1024];
    const uint16_t request_size = make_request_for_session(
        request, command, session_id, request_id, object_id,
        payload, payload_length);
    clear_sent();
    feed_packet(core, 0x619U, (uint16_t)request_id,
                request, request_size);
    (void)reassemble_sent(response, 0x599U);
    return response[24U];
}

static void test_resource_reset(const rbsp_hal_t* hal,
                                const rbsp_node_info_t* info) {
    enum {
        OWNER_SESSION = 0x11223344U,
        PEER_SESSION = 0x55667788U,
    };
    rbsp_core_t core;
    uint8_t response[1024];
    uint8_t resource_id[4U];
    assert(rbsp_core_init(&core, hal, RBSP_CAN_CLASSICAL, info));
    core.node_id = 25U;

#if CONFIG_UART_RESOURCE_COUNT > 0
    uint8_t uart_config[8U] = {0U};
    put_u32(uart_config + 1U, 115200U);
    uart_config[5U] = 8U;
    uart_config[6U] = 1U;
    assert(exchange_status(&core, 0x0200U, OWNER_SESSION, 400U, 0U,
                           uart_config, sizeof(uart_config), response) == 0U);
    const uint32_t uart_object_id = get_u32(response + 12U);
    put_u32(resource_id, 0x02000000U);
    const unsigned resets_before_uart = uart_reset_count;
    assert(exchange_status(&core, 0x0033U, PEER_SESSION, 401U, 0U,
                           resource_id, sizeof(resource_id), response) == 4U);
    assert(uart_reset_count == resets_before_uart &&
           core.uart_objects[0U].used);

    core.uart_status[0U].rx_overruns = 3U;
    core.uart_status[0U].tx_overruns = 2U;
    uart_reset_must_fail = true;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 402U, 0U,
                           resource_id, sizeof(resource_id), response) == 7U);
    assert(core.uart_objects[0U].used &&
           core.uart_objects[0U].object_id == uart_object_id &&
           core.uart_status[0U].backend_failed);

    uart_reset_must_fail = false;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 403U, 0U,
                           resource_id, sizeof(resource_id), response) == 0U);
    assert(!core.uart_objects[0U].used &&
           core.uart_status[0U].rx_overruns == 0U &&
           core.uart_status[0U].tx_overruns == 0U &&
           !core.uart_status[0U].backend_failed);
    /* 空闲资源重复复位必须保持幂等。 */
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 404U, 0U,
                           resource_id, sizeof(resource_id), response) == 0U);
#endif

#if defined(CONFIG_REMOTEBSP_PWM)
    uint8_t pwm_config[8U] = {0U};
    put_u32(pwm_config + 1U, 1000U);
    put_u16(pwm_config + 5U, 5000U);
    assert(exchange_status(&core, 0x0600U, OWNER_SESSION, 410U, 0U,
                           pwm_config, sizeof(pwm_config), response) == 0U);
    const uint32_t pwm_object_id = get_u32(response + 12U);
    put_u32(resource_id, 0x06000000U);
    const unsigned stops_before_pwm = pwm_stop_count;
    assert(exchange_status(&core, 0x0033U, PEER_SESSION, 411U, 0U,
                           resource_id, sizeof(resource_id), response) == 4U);
    assert(pwm_stop_count == stops_before_pwm && core.pwm_objects[0U].used);

    pwm_stop_must_fail = true;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 412U, 0U,
                           resource_id, sizeof(resource_id), response) == 7U);
    assert(core.pwm_objects[0U].used &&
           core.pwm_objects[0U].object_id == pwm_object_id &&
           core.pwm_status[0U].backend_failed);
    pwm_stop_must_fail = false;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 413U, 0U,
                           resource_id, sizeof(resource_id), response) == 0U);
    assert(!core.pwm_objects[0U].used && pwm_stopped[0U] &&
           !core.pwm_status[0U].backend_failed);
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
    uint8_t timed_config[17U] = {0U};
    put_u32(timed_config + 1U, 1250U);
    put_u32(timed_config + 5U, 350U);
    put_u32(timed_config + 9U, 700U);
    put_u32(timed_config + 13U, 80U);
    assert(exchange_status(&core, 0x0700U, OWNER_SESSION, 420U, 0U,
                           timed_config, sizeof(timed_config), response) == 0U);
    const uint32_t timed_object_id = get_u32(response + 12U);
    put_u32(resource_id, 0x0A000000U);
    const unsigned aborts_before_timed = timed_bitstream_abort_count;
    assert(exchange_status(&core, 0x0033U, PEER_SESSION, 421U, 0U,
                           resource_id, sizeof(resource_id), response) == 4U);
    assert(timed_bitstream_abort_count == aborts_before_timed &&
           core.timed_bitstream_objects[0U].used);

    timed_bitstream_abort_must_fail = true;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 422U, 0U,
                           resource_id, sizeof(resource_id), response) == 7U);
    assert(core.timed_bitstream_objects[0U].used &&
           core.timed_bitstream_objects[0U].object_id == timed_object_id &&
           core.timed_bitstream_status[0U].backend_failed);
    timed_bitstream_abort_must_fail = false;
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 423U, 0U,
                           resource_id, sizeof(resource_id), response) == 0U);
    assert(!core.timed_bitstream_objects[0U].used &&
           !core.timed_bitstream_status[0U].backend_failed);
#endif

#if defined(CONFIG_REMOTEBSP_BUS)
    put_u32(resource_id, TEST_I2C_DEVICE_ID);
    assert(exchange_status(&core, 0x0033U, OWNER_SESSION, 430U, 0U,
                           resource_id, sizeof(resource_id), response) == 6U);
#endif
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
        .can_send = fake_can_send,
        .milliseconds = fake_milliseconds,
        .gpio_configure = fake_gpio_configure,
        .gpio_configure_pull = fake_gpio_configure_pull,
        .gpio_write = fake_gpio_write,
        .gpio_read = fake_gpio_read,
        .uart_configure = fake_uart_configure,
        .uart_read = fake_uart_read,
        .uart_write = fake_uart_write,
        .uart_reset = fake_uart_reset,
        .resource_status = fake_resource_status,
#if defined(CONFIG_REMOTEBSP_BUS)
        .bus_resources = test_bus_resources,
        .bus_resource_count = 2U,
        .i2c_transfer = fake_i2c_transfer,
        .spi_transfer = NULL,
#endif
#if defined(CONFIG_REMOTEBSP_PWM)
        .pwm_configure = fake_pwm_configure,
        .pwm_write = fake_pwm_write,
        .pwm_stop = fake_pwm_stop,
#endif
#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
        .timed_bitstream_configure = fake_timed_bitstream_configure,
        .timed_bitstream_write = fake_timed_bitstream_write,
        .timed_bitstream_busy = fake_timed_bitstream_busy,
        .timed_bitstream_abort = fake_timed_bitstream_abort,
#endif
#if defined(CONFIG_REMOTEBSP_MOTION)
        .nanoseconds = fake_nanoseconds,
        .motion_boot_epoch = fake_motion_boot_epoch,
        .motion_axis_count = 2U,
        .motion_set_enable = fake_motion_set_enable,
        .motion_set_direction = fake_motion_set_direction,
        .motion_set_step = fake_motion_set_step,
        .motion_limit_active = fake_motion_limit_active,
        .motion_schedule_compare = fake_motion_schedule_compare,
        .motion_cancel_compare = fake_motion_cancel_compare,
        .motion_enter_critical = fake_motion_enter_critical,
        .motion_exit_critical = fake_motion_exit_critical,
#endif
        .enter_bootloader = fake_enter_bootloader,
    };
    rbsp_node_info_t info = {
        {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
         0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F},
        0, 1, 0, 0x0103CB};
    assert(rbsp_core_init(&core, &hal, RBSP_CAN_CLASSICAL, &info));
    test_resource_reset(&hal, &info);
    uint8_t request[1024];
    uint8_t response[1024];
#if defined(CONFIG_REMOTEBSP_MOTION)
    rbsp_core_t no_epoch_core;
    rbsp_hal_t no_epoch_hal = hal;
    no_epoch_hal.motion_boot_epoch = NULL;
    assert(rbsp_core_init(
        &no_epoch_core, &no_epoch_hal, RBSP_CAN_CLASSICAL, &info));
    assert(no_epoch_core.motion_group.boot_epoch == 0U);
    no_epoch_core.node_id = 26U;
    const uint8_t unsupported_time_sync[] = {1U, 0U, 0U, 0U};
    uint16_t no_epoch_request_size = make_request(
        request, 0x0020U, 99U, 0U, unsupported_time_sync,
        sizeof(unsupported_time_sync));
    clear_sent();
    feed_packet(&no_epoch_core, 0x61AU, 99U, request,
                no_epoch_request_size);
    assert(reassemble_sent(response, 0x59AU) == 25U);
    assert(response[24U] == 6U);
    uint8_t no_epoch_lease[9U];
    put_u32(no_epoch_lease, 0x09000000U);
    put_u32(no_epoch_lease + 4U, 1000U);
    no_epoch_lease[8U] = 2U;
    no_epoch_request_size = make_request(
        request, 0x0035U, 98U, 0U, no_epoch_lease,
        sizeof(no_epoch_lease));
    clear_sent();
    feed_packet(&no_epoch_core, 0x61AU, 98U, request,
                no_epoch_request_size);
    assert(reassemble_sent(response, 0x59AU) == 52U);
    assert(response[24U] == 0U);
    uint8_t no_epoch_segment[32U] = {0U};
    put_u32(no_epoch_segment, 1U);
    put_u64(no_epoch_segment + 4U, 10000000U);
    put_u64(no_epoch_segment + 12U, 2000000U);
    no_epoch_segment[20U] = 1U;
    no_epoch_segment[21U] = 1U;
    put_u32(no_epoch_segment + 24U, 0x09000000U);
    put_u32(no_epoch_segment + 28U, 1U);
    no_epoch_request_size = make_request(
        request, 0x0900U, 97U, 0U, no_epoch_segment,
        sizeof(no_epoch_segment));
    clear_sent();
    feed_packet(&no_epoch_core, 0x61AU, 97U, request,
                no_epoch_request_size);
    assert(reassemble_sent(response, 0x59AU) == 49U);
    assert(response[24U] == 0U && no_epoch_core.motion.size == 1U);
    clear_sent();
#endif
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

    /*
     * 静态资源目录来自编译期能力，不得把 GPIO 对象池容量误当成固定端点。
     * config_all 依次公开 2 UART、2 PWM、2 STEPGEN、1 定时位流和 2 BUS 项。
     */
    clear_sent();
    request_size = make_request(request, 0x0030U, 201U, 0U, NULL, 0U);
    feed_packet(&core, 0x619U, 201U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 180U);
    assert(response[24U] == 0U && get_u16(response + 25U) == 9U);
    const uint8_t* descriptor = response + 27U;
    assert(get_u32(descriptor) == 0x02000000U && descriptor[4U] == 2U);
    assert(get_u16(descriptor + 5U) == 0U &&
           get_u32(descriptor + 9U) == CONFIG_UART_RX_BUFFER_SIZE &&
           get_u32(descriptor + 13U) == CONFIG_UART_TX_BUFFER_SIZE);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x02000001U && descriptor[4U] == 2U);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x06000000U && descriptor[4U] == 6U);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x06000001U && descriptor[4U] == 6U);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x09000000U && descriptor[4U] == 9U);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x09000001U && descriptor[4U] == 9U);
    descriptor += 17U;
    assert(get_u32(descriptor) == 0x0A000000U && descriptor[4U] == 10U &&
           get_u32(descriptor + 13U) == 96U);
    descriptor += 17U;
    assert(get_u32(descriptor) == TEST_I2C_BUS_ID && descriptor[4U] == 11U);
    descriptor += 17U;
    assert(get_u32(descriptor) == TEST_I2C_DEVICE_ID &&
           descriptor[4U] == 12U && get_u32(descriptor + 9U) == 128U &&
           get_u32(descriptor + 13U) == 128U);

    uint8_t static_resource_id[4U];
    put_u32(static_resource_id, 0x06000000U);
    clear_sent();
    request_size = make_request(request, 0x0031U, 202U, 0U,
                                static_resource_id,
                                sizeof(static_resource_id));
    feed_packet(&core, 0x619U, 202U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 42U);
    assert(response[24U] == 0U && get_u32(response + 25U) == 0x06000000U &&
           response[29U] == 6U);

    clear_sent();
    request_size = make_request(request, 0x0032U, 203U, 0U,
                                static_resource_id,
                                sizeof(static_resource_id));
    feed_packet(&core, 0x619U, 203U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 50U);
    assert(response[24U] == 0U && get_u32(response + 25U) == 0x06000000U &&
           response[29U] == 0U);

    clear_sent();
    request_size = make_request(request, 0x0034U, 204U, 0U,
                                static_resource_id,
                                sizeof(static_resource_id));
    feed_packet(&core, 0x619U, 204U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 57U);
    assert(response[24U] == 0U && get_u32(response + 25U) == 0x06000000U &&
           get_u16(response + 29U) == 1U &&
           get_u16(response + 31U) == 0x000AU);

    const unsigned stops_before_idle_reset = pwm_stop_count;
    clear_sent();
    request_size = make_request(request, 0x0033U, 205U, 0U,
                                static_resource_id,
                                sizeof(static_resource_id));
    feed_packet(&core, 0x619U, 205U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U &&
           pwm_stop_count == stops_before_idle_reset + 1U);

    put_u32(static_resource_id, 0xDEADBEEFU);
    clear_sent();
    request_size = make_request(request, 0x0031U, 206U, 0U,
                                static_resource_id,
                                sizeof(static_resource_id));
    feed_packet(&core, 0x619U, 206U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 3U);

    clear_sent();
    request_size = make_request(request, 0x0030U, 207U, 1U, NULL, 0U);
    feed_packet(&core, 0x619U, 207U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 2U);

#if defined(CONFIG_REMOTEBSP_MOTION)
    /* STEPGEN 资源公开租约合同，并且只接受有界的独占租约。 */
    uint8_t resource_id_payload[4U];
    put_u32(resource_id_payload, 0x09000000U);
    clear_sent();
    request_size = make_request(request, 0x0034U, 90U, 0U,
                                resource_id_payload,
                                sizeof(resource_id_payload));
    feed_packet(&core, 0x619U, 90U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 57U);
    assert(response[24U] == 0U && get_u32(response + 25U) == 0x09000000U);
    assert(response[29U] == 1U && response[30U] == 0U);
    assert(response[31U] == 0x3AU && response[32U] == 0U);

#if defined(CONFIG_REMOTEBSP_BUS)
    /*
     * BUS 与 MOTION 同时启用时，通用资源命令必须按 ResourceType
     * 高字节稳定路由，不能把 I2C 设备误当作 STEPGEN。
     */
    put_u32(resource_id_payload, TEST_I2C_DEVICE_ID);
    clear_sent();
    request_size = make_request(request, 0x0034U, 96U, 0U,
                                resource_id_payload,
                                sizeof(resource_id_payload));
    feed_packet(&core, 0x619U, 96U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 57U);
    assert(response[24U] == 0U &&
           get_u32(response + 25U) == TEST_I2C_DEVICE_ID);
    assert(response[29U] == 1U && response[30U] == 0U);
    assert(response[31U] == 0x3BU && response[32U] == 0U);
    assert(get_u32(response + 41U) == 1000U);
    assert(get_u32(response + 45U) == 1U);

    uint8_t bus_lease_request[9U];
    put_u32(bus_lease_request, TEST_I2C_DEVICE_ID);
    put_u32(bus_lease_request + 4U, 1000U);
    bus_lease_request[8U] = 2U;
    clear_sent();
    request_size = make_request(request, 0x0035U, 98U, 0U,
                                bus_lease_request,
                                sizeof(bus_lease_request));
    feed_packet(&core, 0x619U, 98U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 52U);
    assert(response[24U] == 0U && core.bus_leases[1U].active);
    assert(!core.stepgen_leases[0U].active);

    put_u32(resource_id_payload, 0x09000000U);
    clear_sent();
    request_size = make_request(request, 0x0034U, 97U, 0U,
                                resource_id_payload,
                                sizeof(resource_id_payload));
    feed_packet(&core, 0x619U, 97U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 57U);
    assert(response[24U] == 0U && get_u32(response + 25U) == 0x09000000U);
    assert(response[31U] == 0x3AU && response[32U] == 0U);
#endif

    uint8_t lease_request[9U];
    put_u32(lease_request, 0x09000000U);
    put_u32(lease_request + 4U, 1000U);
    lease_request[8U] = 2U;
    clear_sent();
    request_size = make_request(request, 0x0035U, 91U, 0U,
                                lease_request, sizeof(lease_request));
    feed_packet(&core, 0x619U, 91U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 52U);
    const uint64_t stepgen_lease_id = get_u64(response + 29U);
    assert(response[24U] == 0U && stepgen_lease_id != 0U);
    assert(get_u32(response + 37U) == 0x12345678U);
    assert(core.stepgen_leases[0].active);
#if defined(CONFIG_REMOTEBSP_BUS)
    assert(core.bus_leases[1U].active);
#endif

    /* 相同请求命中去重缓存，不分配第二个租约；其它会话不能争用。 */
    clear_sent();
    feed_packet(&core, 0x619U, 92U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 52U);
    assert(get_u64(response + 29U) == stepgen_lease_id);
    assert(core.next_stepgen_lease_id == 2U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0035U, 0x87654321U, 92U, 0U,
        lease_request, sizeof(lease_request));
    feed_packet(&core, 0x619U, 93U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 8U);

    uint8_t lease_token[16U];
    put_u32(lease_token, 0x09000000U);
    put_u64(lease_token + 4U, stepgen_lease_id);
    put_u32(lease_token + 12U, 2000U);
    clear_sent();
    request_size = make_request(request, 0x0036U, 93U, 0U,
                                lease_token, sizeof(lease_token));
    feed_packet(&core, 0x619U, 94U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 52U);
    assert(response[24U] == 0U && get_u64(response + 29U) == stepgen_lease_id);
    assert(get_u32(response + 45U) == 2000U);

    clear_sent();
    request_size = make_request_for_session(
        request, 0x0036U, 0x87654321U, 95U, 0U,
        lease_token, sizeof(lease_token));
    feed_packet(&core, 0x619U, 96U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 4U);

    /* 状态查询不泄露能力令牌，但会报告当前所有者和剩余时间。 */
    clear_sent();
    request_size = make_request(request, 0x0038U, 94U, 0U,
                                resource_id_payload,
                                sizeof(resource_id_payload));
    feed_packet(&core, 0x619U, 95U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 52U);
    assert(response[24U] == 0U && get_u64(response + 29U) == 0U);
    assert(get_u32(response + 37U) == 0x12345678U);

    /* TIME_SYNC 暴露本次启动代次和与运动执行器一致的 1 GHz 节点时钟。 */
    clear_sent();
    const uint8_t time_sync[] = {1U, 0U, 0U, 0U};
    request_size = make_request(request, 0x0020U, 100U, 0U,
                                time_sync, sizeof(time_sync));
    feed_packet(&core, 0x619U, 100U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 61U);
    assert(response[24U] == 0U && response[25U] == 1U &&
           response[26U] == 64U);
    const uint64_t boot_epoch = get_u64(response + 29U);
    assert(boot_epoch != 0U);
    assert(get_u64(response + 37U) == 1000000000ULL);

    /* PREPARE 只冻结本地时刻和段，不修改队列，也不会输出 STEP。 */
    clear_sent();
    uint8_t group_prepare[116U];
    memset(group_prepare, 0, sizeof(group_prepare));
    make_motion_group_identity(group_prepare, boot_epoch, 10000000U);
    put_u16(group_prepare + 80U, 32U);
    put_u32(group_prepare + 84U, 1U);
    put_u64(group_prepare + 88U, 123456789U);
    put_u64(group_prepare + 96U, 2000000U);
    group_prepare[104U] = 1U;
    group_prepare[105U] = 1U;
    put_u32(group_prepare + 108U, 0x09000000U);
    put_u32(group_prepare + 112U, 4U);
    request_size = make_request(request, 0x0910U, 101U, 0U,
                                group_prepare, sizeof(group_prepare));
    feed_packet(&core, 0x619U, 101U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[24U] == 0U && response[105U] == 0U);
    assert(core.motion_group.state == RBSP_MOTION_GROUP_PREPARED);
    assert(core.motion.size == 0U && core.motion.emitted_edges == 0U);

    /* 已 PREPARE 时，普通入队不能越过冻结事务插入运动段。 */
    clear_sent();
    request_size = make_request(request, 0x0900U, 105U, 0U,
                                group_prepare + 84U, 32U);
    feed_packet(&core, 0x619U, 105U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 8U && core.motion.size == 0U);

    /* COMMIT 才把冻结段交给执行器；重复 COMMIT 不会重复入队。 */
    clear_sent();
    request_size = make_request(request, 0x0911U, 102U, 0U,
                                group_prepare, 80U);
    feed_packet(&core, 0x619U, 102U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[24U] == 0U && response[105U] == 0U);
    assert(core.motion_group.state == RBSP_MOTION_GROUP_ARMED);
    assert(core.motion.size == 1U && core.motion.accepted_segments == 1U);
    clear_sent();
    request_size = make_request(request, 0x0911U, 103U, 0U,
                                group_prepare, 80U);
    feed_packet(&core, 0x619U, 103U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[105U] == 0U && core.motion.size == 1U &&
           core.motion.accepted_segments == 1U);

    /* 已武装事务的 ABORT 复用普通运动安全停机并锁存 Aborted。 */
    clear_sent();
    uint8_t group_abort[88U];
    memset(group_abort, 0, sizeof(group_abort));
    memcpy(group_abort, group_prepare, 80U);
    group_abort[80U] = 1U;
    request_size = make_request(request, 0x0912U, 104U, 0U,
                                group_abort, sizeof(group_abort));
    feed_packet(&core, 0x619U, 104U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U);
    assert(core.motion_group.state == RBSP_MOTION_GROUP_ABORTED);
    assert(core.motion.size == 0U &&
           core.motion.fault == RBSP_MOTION_FAULT_ABORTED);
    assert(core.motion.safety_stops == 1U);

    /* 首次 compare 调度失败必须清空刚提交的段并立即锁存故障。 */
    clear_sent();
    request_size = make_request(request, 0x0903U, 106U, 0U,
                                NULL, 0U);
    feed_packet(&core, 0x619U, 106U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U);
    put_u64(group_prepare + 4U, 9002U);
    put_u32(group_prepare + 16U, 2U);
    put_u64(group_prepare + 40U, 20000000U);
    put_u32(group_prepare + 84U, 2U);
    request_size = make_request(request, 0x0910U, 107U, 0U,
                                group_prepare, sizeof(group_prepare));
    clear_sent();
    feed_packet(&core, 0x619U, 107U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[105U] == 0U &&
           core.motion_group.state == RBSP_MOTION_GROUP_PREPARED);
    motion_schedule_allowed = false;
    request_size = make_request(request, 0x0911U, 108U, 0U,
                                group_prepare, 80U);
    clear_sent();
    feed_packet(&core, 0x619U, 108U, request, request_size);
    motion_schedule_allowed = true;
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[24U] == 0U && response[105U] == 1U);
    assert(core.motion_group.state == RBSP_MOTION_GROUP_ABORTED);
    assert(core.motion.size == 0U &&
           core.motion.fault != RBSP_MOTION_FAULT_NONE);

    /* 普通 MotionAbort 同样必须使尚未提交的事务不可再 COMMIT。 */
    request_size = make_request(request, 0x0903U, 109U, 0U,
                                NULL, 0U);
    clear_sent();
    feed_packet(&core, 0x619U, 109U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U);
    put_u64(group_prepare + 4U, 9003U);
    put_u32(group_prepare + 16U, 3U);
    put_u64(group_prepare + 40U, 30000000U);
    put_u32(group_prepare + 84U, 3U);
    request_size = make_request(request, 0x0910U, 110U, 0U,
                                group_prepare, sizeof(group_prepare));
    clear_sent();
    feed_packet(&core, 0x619U, 110U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 113U);
    assert(response[105U] == 0U &&
           core.motion_group.state == RBSP_MOTION_GROUP_PREPARED);
    request_size = make_request(request, 0x0902U, 111U, 0U,
                                NULL, 0U);
    clear_sent();
    feed_packet(&core, 0x619U, 111U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U &&
           core.motion_group.state == RBSP_MOTION_GROUP_ABORTED &&
           core.motion.fault == RBSP_MOTION_FAULT_ABORTED);

    /* 独立 Core 覆盖租约缺失、过期、COMMIT 重检和会话释放。 */
    rbsp_core_t lease_core;
    now_ms = 0U;
    assert(rbsp_core_init(&lease_core, &hal, RBSP_CAN_CLASSICAL, &info));
    lease_core.node_id = 27U;
    uint8_t ordinary_segment[32U];
    memset(ordinary_segment, 0, sizeof(ordinary_segment));
    put_u32(ordinary_segment, 1U);
    put_u64(ordinary_segment + 4U, 10000000U);
    put_u64(ordinary_segment + 12U, 2000000U);
    ordinary_segment[20U] = 1U;
    ordinary_segment[21U] = 1U;
    put_u32(ordinary_segment + 24U, 0x09000000U);
    put_u32(ordinary_segment + 28U, 4U);
    clear_sent();
    request_size = make_request(request, 0x0900U, 1U, 0U,
                                ordinary_segment,
                                sizeof(ordinary_segment));
    feed_packet(&lease_core, 0x61BU, 1U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 25U);
    assert(response[24U] == 4U && lease_core.motion.size == 0U);

    put_u32(lease_request + 4U, 100U);
    clear_sent();
    request_size = make_request(request, 0x0035U, 2U, 0U,
                                lease_request, sizeof(lease_request));
    feed_packet(&lease_core, 0x61BU, 2U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 52U);
    assert(response[24U] == 0U);
    clear_sent();
    request_size = make_request(request, 0x0900U, 3U, 0U,
                                ordinary_segment,
                                sizeof(ordinary_segment));
    feed_packet(&lease_core, 0x61BU, 3U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 49U);
    assert(response[24U] == 0U && lease_core.motion.size == 1U);
    assert(lease_core.motion_owner_session_id == 0x12345678U);
    now_ms = 100U;
    clear_sent();
    rbsp_core_poll(&lease_core);
    assert(!lease_core.stepgen_leases[0].active);
    assert(lease_core.motion.size == 0U &&
           lease_core.motion.fault == RBSP_MOTION_FAULT_ABORTED);
    assert(lease_core.motion_owner_session_id == 0U);

    clear_sent();
    request_size = make_request(request, 0x0903U, 4U, 0U, NULL, 0U);
    feed_packet(&lease_core, 0x61BU, 4U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 25U);
    assert(response[24U] == 0U);

    /* PREPARE 后租约到期，COMMIT 必须再次拒绝且不得留下运动段。 */
    clear_sent();
    request_size = make_request(request, 0x0035U, 5U, 0U,
                                lease_request, sizeof(lease_request));
    feed_packet(&lease_core, 0x61BU, 5U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 52U);
    uint8_t lease_group_prepare[116U];
    memset(lease_group_prepare, 0, sizeof(lease_group_prepare));
    make_motion_group_identity(lease_group_prepare, 31U, 110000000U);
    put_u16(lease_group_prepare + 80U, 32U);
    memcpy(lease_group_prepare + 84U, ordinary_segment,
           sizeof(ordinary_segment));
    put_u32(lease_group_prepare + 84U, 2U);
    put_u64(lease_group_prepare + 88U, 110000000U);
    clear_sent();
    request_size = make_request(request, 0x0910U, 6U, 0U,
                                lease_group_prepare,
                                sizeof(lease_group_prepare));
    feed_packet(&lease_core, 0x61BU, 6U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 113U);
    assert(response[105U] == 0U &&
           lease_core.motion_group.state == RBSP_MOTION_GROUP_PREPARED);
    now_ms = 200U;
    clear_sent();
    request_size = make_request(request, 0x0911U, 7U, 0U,
                                lease_group_prepare, 80U);
    feed_packet(&lease_core, 0x61BU, 7U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 113U);
    assert(response[24U] == 0U && response[105U] == 1U);
    assert(lease_core.motion_group.state == RBSP_MOTION_GROUP_ABORTED);
    assert(lease_core.motion.size == 0U);

    /* release_session 清理租约和预备事务，也清除该会话的旧去重路由。 */
    put_u32(lease_request + 4U, 1000U);
    clear_sent();
    request_size = make_request(request, 0x0035U, 8U, 0U,
                                lease_request, sizeof(lease_request));
    feed_packet(&lease_core, 0x61BU, 8U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 52U);
    const uint64_t release_session_lease_id = get_u64(response + 29U);
    put_u64(lease_group_prepare + 4U, 9002U);
    put_u32(lease_group_prepare + 16U, 2U);
    put_u64(lease_group_prepare + 40U, 220000000U);
    put_u32(lease_group_prepare + 84U, 2U);
    put_u64(lease_group_prepare + 88U, 220000000U);
    clear_sent();
    request_size = make_request(request, 0x0910U, 9U, 0U,
                                lease_group_prepare,
                                sizeof(lease_group_prepare));
    feed_packet(&lease_core, 0x61BU, 9U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 113U);
    assert(response[105U] == 0U);
    assert(rbsp_core_release_session(&lease_core, 0x12345678U) == 1U);
    assert(!lease_core.stepgen_leases[0].active);
    assert(lease_core.motion_group.state == RBSP_MOTION_GROUP_ABORTED);
    clear_sent();
    request_size = make_request(request, 0x0035U, 8U, 0U,
                                lease_request, sizeof(lease_request));
    feed_packet(&lease_core, 0x61BU, 10U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 52U);
    const uint64_t reacquired_lease_id = get_u64(response + 29U);
    assert(response[24U] == 0U &&
           reacquired_lease_id != release_session_lease_id);
    put_u32(lease_token, 0x09000000U);
    put_u64(lease_token + 4U, reacquired_lease_id);
    put_u32(lease_token + 12U, 0U);
    clear_sent();
    request_size = make_request(request, 0x0037U, 10U, 0U,
                                lease_token, sizeof(lease_token));
    feed_packet(&lease_core, 0x61BU, 11U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 25U);
    assert(response[24U] == 0U && !lease_core.stepgen_leases[0].active);
    clear_sent();
    feed_packet(&lease_core, 0x61BU, 12U, request, request_size);
    assert(reassemble_sent(response, 0x59BU) == 25U);
    assert(response[24U] == 0U);

    /* 节点重新初始化不会继承租约、会话或运动队列。 */
    assert(rbsp_core_init(&lease_core, &hal, RBSP_CAN_CLASSICAL, &info));
    assert(!lease_core.stepgen_leases[0].active &&
           lease_core.motion_owner_session_id == 0U &&
           lease_core.motion.size == 0U);
    now_ms = 0U;
#endif

    clear_sent();
    const uint8_t create[] = {7U, 0U, 1U, 0U};
    request_size = make_request(request, 0x0100U, 3U, 0U,
                                create, sizeof(create));
    feed_packet(&core, 0x619U, 3U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t gpio_object = get_u32(response + 12U);
    assert(gpio_object != 0U);

    /* 后续既有测试仍需使用刚创建的 GPIO 对象。 */
    core.gpio_objects[0].used = true;
    assert(core.gpio_objects[0].owner_session_id == 0x12345678U);

    /* 其他会话不能读取、写入或关闭现有对象，原会话仍可继续使用。 */
    const uint32_t other_gpio_session = 0x87654321U;
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0101U, other_gpio_session, 40U, gpio_object, NULL, 0U);
    feed_packet(&core, 0x619U, 40U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 4U);

    const uint8_t high[] = {1U};
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0102U, other_gpio_session, 41U, gpio_object,
        high, sizeof(high));
    feed_packet(&core, 0x619U, 41U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 4U && !gpio_values[7]);

    const uint8_t close_v1[] = {1U};
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0103U, other_gpio_session, 42U, gpio_object,
        close_v1, sizeof(close_v1));
    feed_packet(&core, 0x619U, 42U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 4U && core.gpio_objects[0].used);

    clear_sent();
    request_size = make_request(request, 0x0102U, 4U, gpio_object,
                                high, sizeof(high));
    feed_packet(&core, 0x619U, 4U, request, request_size);
    assert(gpio_write_count == 1U && gpio_values[7]);
    clear_sent();
    feed_packet(&core, 0x619U, 5U, request, request_size);
    assert(gpio_write_count == 1U);
    reassemble_sent(response, 0x599U);

    clear_sent();
    request_size = make_request(request, 0x0101U, 43U, gpio_object,
                                NULL, 0U);
    feed_packet(&core, 0x619U, 43U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 26U);
    assert(response[24U] == 0U && response[25U] == 1U);

    /* GPIO_CLOSE 自身先写低；失败保留对象，成功后允许同引脚重建。 */
    gpio_write_must_fail = true;
    clear_sent();
    request_size = make_request(request, 0x0103U, 50U, gpio_object,
                                close_v1, sizeof(close_v1));
    feed_packet(&core, 0x619U, 50U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 7U && core.gpio_objects[0].used &&
           gpio_values[7]);

    gpio_write_must_fail = false;
    clear_sent();
    request_size = make_request(request, 0x0103U, 51U, gpio_object,
                                close_v1, sizeof(close_v1));
    feed_packet(&core, 0x619U, 51U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U && !core.gpio_objects[0].used &&
           !gpio_values[7]);

    clear_sent();
    request_size = make_request(request, 0x0100U, 52U, 0U,
                                create, sizeof(create));
    feed_packet(&core, 0x619U, 52U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U && core.gpio_objects[0].used);
    const uint32_t recreated_gpio_object = get_u32(response + 12U);
    assert(recreated_gpio_object != 0U &&
           recreated_gpio_object != gpio_object);

    clear_sent();
    request_size = make_request(request, 0x0103U, 53U,
                                recreated_gpio_object,
                                close_v1, sizeof(close_v1));
    feed_packet(&core, 0x619U, 53U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U);
    clear_sent();
    request_size = make_request(request, 0x0103U, 54U,
                                recreated_gpio_object,
                                close_v1, sizeof(close_v1));
    feed_packet(&core, 0x619U, 54U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U);

    /* 会话结束只清理本会话对象；输出必须先确认写低，失败则保留。 */
    const uint32_t first_gpio_session = 0x11111111U;
    const uint32_t second_gpio_session = 0x22222222U;
    const uint8_t create_pin8[] = {8U, 0U, 1U, 0U};
    const uint8_t create_pin9[] = {9U, 0U, 1U, 0U};
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0100U, first_gpio_session, 1U, 0U,
        create_pin8, sizeof(create_pin8));
    feed_packet(&core, 0x619U, 60U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    const uint32_t first_session_object = get_u32(response + 12U);
    assert(response[24U] == 0U && first_session_object != 0U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0102U, first_gpio_session, 2U,
        first_session_object, high, sizeof(high));
    feed_packet(&core, 0x619U, 61U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U && gpio_values[8]);

    clear_sent();
    request_size = make_request_for_session(
        request, 0x0100U, second_gpio_session, 1U, 0U,
        create_pin9, sizeof(create_pin9));
    feed_packet(&core, 0x619U, 62U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    const uint32_t second_session_object = get_u32(response + 12U);
    assert(response[24U] == 0U && second_session_object != 0U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0102U, second_gpio_session, 2U,
        second_session_object, high, sizeof(high));
    feed_packet(&core, 0x619U, 63U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24U] == 0U && gpio_values[9]);

    assert(rbsp_core_release_session(&core, first_gpio_session) == 1U);
    assert(!gpio_values[8] && gpio_values[9]);
    assert(!core.gpio_objects[0].used && core.gpio_objects[1].used);
    assert(core.gpio_objects[1].owner_session_id == second_gpio_session);

    gpio_write_must_fail = true;
    assert(rbsp_core_release_session(&core, second_gpio_session) == 0U);
    assert(core.gpio_objects[1].used && gpio_values[9]);
    gpio_write_must_fail = false;
    assert(rbsp_core_release_session(&core, second_gpio_session) == 1U);
    assert(!core.gpio_objects[1].used && !gpio_values[9]);
    assert(rbsp_core_release_session(&core, second_gpio_session) == 0U);

    /*
     * UART、PWM 和定时位流对象同样绑定创建会话。其他会话不能借用
     * object_id 访问资源；会话释放只清理其所有对象并执行安全停机。
     */
    rbsp_core_t object_session_core;
    assert(rbsp_core_init(
        &object_session_core, &hal, RBSP_CAN_CLASSICAL, &info));
    object_session_core.node_id = 28U;
    const uint32_t object_owner_session = 0x33333333U;
    const uint32_t object_peer_session = 0x44444444U;

    uint8_t owner_uart_config[8U] = {0U};
    put_u32(owner_uart_config + 1U, 115200U);
    owner_uart_config[5U] = 8U;
    owner_uart_config[6U] = 1U;
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0200U, object_owner_session, 1U, 0U,
        owner_uart_config, sizeof(owner_uart_config));
    feed_packet(&object_session_core, 0x61CU, 1U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    const uint32_t owner_uart_object = get_u32(response + 12U);
    assert(response[24U] == 0U && owner_uart_object != 0U);

    uint8_t peer_uart_config[8U] = {1U};
    put_u32(peer_uart_config + 1U, 57600U);
    peer_uart_config[5U] = 8U;
    peer_uart_config[6U] = 1U;
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0200U, object_peer_session, 1U, 0U,
        peer_uart_config, sizeof(peer_uart_config));
    feed_packet(&object_session_core, 0x61CU, 2U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    const uint32_t peer_uart_object = get_u32(response + 12U);
    assert(response[24U] == 0U && peer_uart_object != 0U);

    uint8_t owner_pwm_config[8U] = {0U};
    put_u32(owner_pwm_config + 1U, 20000U);
    put_u16(owner_pwm_config + 5U, 2500U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0600U, object_owner_session, 2U, 0U,
        owner_pwm_config, sizeof(owner_pwm_config));
    feed_packet(&object_session_core, 0x61CU, 3U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    const uint32_t owner_pwm_object = get_u32(response + 12U);
    assert(response[24U] == 0U && owner_pwm_object != 0U &&
           pwm_duty[0U] == 2500U && !pwm_stopped[0U]);

    uint8_t peer_pwm_config[8U] = {1U};
    put_u32(peer_pwm_config + 1U, 10000U);
    put_u16(peer_pwm_config + 5U, 3500U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0600U, object_peer_session, 2U, 0U,
        peer_pwm_config, sizeof(peer_pwm_config));
    feed_packet(&object_session_core, 0x61CU, 4U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    const uint32_t peer_pwm_object = get_u32(response + 12U);
    assert(response[24U] == 0U && peer_pwm_object != 0U &&
           pwm_duty[1U] == 3500U && !pwm_stopped[1U]);

    uint8_t owner_bitstream_config[17U] = {0U};
    put_u32(owner_bitstream_config + 1U, 1250U);
    put_u32(owner_bitstream_config + 5U, 350U);
    put_u32(owner_bitstream_config + 9U, 700U);
    put_u32(owner_bitstream_config + 13U, 80U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0700U, object_owner_session, 3U, 0U,
        owner_bitstream_config, sizeof(owner_bitstream_config));
    feed_packet(&object_session_core, 0x61CU, 5U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    const uint32_t owner_bitstream_object = get_u32(response + 12U);
    assert(response[24U] == 0U && owner_bitstream_object != 0U);

    const uint8_t owner_bitstream_write[] = {8U, 0U, 0x5AU};
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0701U, object_owner_session, 4U,
        owner_bitstream_object, owner_bitstream_write,
        sizeof(owner_bitstream_write));
    feed_packet(&object_session_core, 0x61CU, 6U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 0U && timed_bit_busy &&
           timed_bit_count == 8U && timed_bit_data[0U] == 0x5AU);

    const unsigned reads_before_denied = uart_read_count;
    const unsigned writes_before_denied = uart_write_count;
    const unsigned stops_before_denied = pwm_stop_count;
    const unsigned bit_writes_before_denied = timed_bitstream_write_count;
    const unsigned aborts_before_denied = timed_bitstream_abort_count;
    uint8_t one_byte_read[2U];
    put_u16(one_byte_read, 1U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0201U, object_peer_session, 10U,
        owner_uart_object, one_byte_read, sizeof(one_byte_read));
    feed_packet(&object_session_core, 0x61CU, 10U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U && uart_read_count == reads_before_denied);

    const uint8_t uart_byte = 0xA5U;
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0202U, object_peer_session, 11U,
        owner_uart_object, &uart_byte, sizeof(uart_byte));
    feed_packet(&object_session_core, 0x61CU, 11U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U && uart_write_count == writes_before_denied);

    uint8_t denied_pwm_duty[2U];
    put_u16(denied_pwm_duty, 9000U);
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0601U, object_peer_session, 12U,
        owner_pwm_object, denied_pwm_duty, sizeof(denied_pwm_duty));
    feed_packet(&object_session_core, 0x61CU, 12U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U && pwm_duty[0U] == 2500U);

    clear_sent();
    request_size = make_request_for_session(
        request, 0x0602U, object_peer_session, 13U,
        owner_pwm_object, NULL, 0U);
    feed_packet(&object_session_core, 0x61CU, 13U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U &&
           pwm_stop_count == stops_before_denied &&
           object_session_core.pwm_objects[0U].used);

    const uint8_t denied_bitstream_write[] = {8U, 0U, 0xC3U};
    clear_sent();
    request_size = make_request_for_session(
        request, 0x0701U, object_peer_session, 14U,
        owner_bitstream_object, denied_bitstream_write,
        sizeof(denied_bitstream_write));
    feed_packet(&object_session_core, 0x61CU, 14U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U &&
           timed_bitstream_write_count == bit_writes_before_denied &&
           timed_bit_data[0U] == 0x5AU);

    clear_sent();
    request_size = make_request_for_session(
        request, 0x0702U, object_peer_session, 15U,
        owner_bitstream_object, NULL, 0U);
    feed_packet(&object_session_core, 0x61CU, 15U, request, request_size);
    assert(reassemble_sent(response, 0x59CU) == 25U);
    assert(response[24U] == 4U &&
           timed_bitstream_abort_count == aborts_before_denied &&
           timed_bit_busy);

    const unsigned resets_before_release = uart_reset_count;
    uart_reset_must_fail = true;
    assert(rbsp_core_release_session(
               &object_session_core, object_owner_session) == 2U);
    assert(object_session_core.uart_objects[0U].used &&
           object_session_core.uart_objects[0U].object_id ==
               owner_uart_object &&
           object_session_core.uart_objects[1U].used &&
           object_session_core.uart_objects[1U].object_id ==
               peer_uart_object);
    assert(!object_session_core.pwm_objects[0U].used &&
           object_session_core.pwm_objects[1U].used &&
           object_session_core.pwm_objects[1U].object_id ==
               peer_pwm_object);
    assert(pwm_stopped[0U] && !pwm_stopped[1U] &&
           pwm_stop_count == stops_before_denied + 1U);
    assert(!object_session_core.timed_bitstream_objects[0U].used &&
           !timed_bit_busy &&
           timed_bitstream_abort_count == aborts_before_denied + 1U);
    assert(uart_reset_count == resets_before_release + 1U &&
           object_session_core.uart_status[0U].backend_failed);

    uart_reset_must_fail = false;
    assert(rbsp_core_release_session(
               &object_session_core, object_owner_session) == 1U);
    assert(!object_session_core.uart_objects[0U].used &&
           !object_session_core.uart_status[0U].backend_failed &&
           uart_reset_count == resets_before_release + 2U);
    assert(rbsp_core_release_session(
               &object_session_core, object_owner_session) == 0U);
    assert(rbsp_core_release_session(
               &object_session_core, object_peer_session) == 2U);
    assert(!object_session_core.uart_objects[1U].used &&
           !object_session_core.pwm_objects[1U].used &&
           pwm_stopped[1U]);

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

    /* 实体状态合并板级缓冲/溢出与 Core 对象占用。 */
    uint8_t observed_resource_id[4U];
    put_u32(observed_resource_id, 0x02000000U);
    clear_sent();
    request_size = make_request(request, 0x0032U, 300U, 0U,
                                observed_resource_id,
                                sizeof(observed_resource_id));
    feed_packet(&core, 0x619U, 300U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 50U);
    assert(response[24U] == 0U && response[29U] == 2U);
    assert(get_u32(response + 30U) == 1U);
    assert(get_u32(response + 34U) == 7U);
    assert(get_u32(response + 38U) == 3U);
    assert(get_u32(response + 42U) == 2U);

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
    uint8_t uart_stream_config[9] = {1U};
    put_u32(uart_stream_config + 1U, 9600U);
    uart_stream_config[5] = 8U;
    uart_stream_config[6] = 1U;
    uart_stream_config[8] = 1U;
    request_size = make_request(
        request, 0x0200U, 20U, 0U, uart_stream_config,
        sizeof(uart_stream_config));
    feed_packet(&core, 0x619U, 20U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t uart_stream_object = get_u32(response + 12U);
    assert(response[24] == 0U && uart_stream_object != 0U);

    clear_sent();
    now_ms = 10U;
    rbsp_core_poll(&core);
    assert(reassemble_sent(response, 0x519U) ==
           24U + CONFIG_UART_EVENT_CHUNK_SIZE);
    assert(response[1] == 3U);
    assert(response[2] == 0x80U && response[3] == 0x02U);
    assert(get_u32(response + 8U) == 1U);
    assert(get_u32(response + 12U) == uart_stream_object);
    for (size_t index = 0;
         index < CONFIG_UART_EVENT_CHUNK_SIZE; ++index) {
        assert(response[24U + index] == 0x5AU);
    }

    clear_sent();
    put_u16(uart_read_length, 1U);
    request_size = make_request(
        request, 0x0201U, 21U, uart_stream_object,
        uart_read_length, sizeof(uart_read_length));
    feed_packet(&core, 0x619U, 21U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24] == 4U);
    core.uart_objects[1].streaming = false;

    clear_sent();
    uint8_t pwm_config[8] = {0U};
    put_u32(pwm_config + 1U, 20000U);
    put_u16(pwm_config + 5U, 2500U);
    request_size = make_request(request, 0x0600U, 30U, 0U,
                                pwm_config, sizeof(pwm_config));
    feed_packet(&core, 0x619U, 30U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t pwm_object = get_u32(response + 12U);
    assert(response[24] == 0U && pwm_object != 0U);
    assert(pwm_duty[0] == 2500U);

    put_u32(observed_resource_id, 0x06000000U);
    clear_sent();
    request_size = make_request(request, 0x0032U, 301U, 0U,
                                observed_resource_id,
                                sizeof(observed_resource_id));
    feed_packet(&core, 0x619U, 301U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 50U);
    assert(response[24U] == 0U && response[29U] == 1U);

    clear_sent();
    uint8_t pwm_write[2];
    put_u16(pwm_write, 7500U);
    request_size = make_request(request, 0x0601U, 31U, pwm_object,
                                pwm_write, sizeof(pwm_write));
    feed_packet(&core, 0x619U, 31U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U && pwm_duty[0] == 7500U);

    clear_sent();
    request_size = make_request(request, 0x0602U, 32U, pwm_object,
                                NULL, 0U);
    feed_packet(&core, 0x619U, 32U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U && pwm_stopped[0]);

    /* STOP 成功后释放对象槽；重复请求仍由去重缓存返回同一成功响应。 */
    clear_sent();
    feed_packet(&core, 0x619U, 32U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U);

    clear_sent();
    request_size = make_request(request, 0x0601U, 304U, pwm_object,
                                pwm_write, sizeof(pwm_write));
    feed_packet(&core, 0x619U, 304U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 3U);

    clear_sent();
    put_u32(pwm_config + 1U, 100000U);
    request_size = make_request(request, 0x0600U, 305U, 0U,
                                pwm_config, sizeof(pwm_config));
    feed_packet(&core, 0x619U, 305U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t recreated_pwm_object = get_u32(response + 12U);
    assert(response[24] == 0U && recreated_pwm_object != 0U &&
           recreated_pwm_object != pwm_object);

    clear_sent();
    request_size = make_request(request, 0x0602U, 306U,
                                recreated_pwm_object, NULL, 0U);
    feed_packet(&core, 0x619U, 306U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U && pwm_stopped[0]);

    clear_sent();
    uint8_t bitstream_config[17] = {0U};
    put_u32(bitstream_config + 1U, 1250U);
    put_u32(bitstream_config + 5U, 350U);
    put_u32(bitstream_config + 9U, 700U);
    put_u32(bitstream_config + 13U, 80U);
    request_size = make_request(request, 0x0700U, 33U, 0U,
                                bitstream_config,
                                sizeof(bitstream_config));
    feed_packet(&core, 0x619U, 33U, request, request_size);
    reassemble_sent(response, 0x599U);
    const uint32_t bitstream_object = get_u32(response + 12U);
    assert(response[24] == 0U && bitstream_object != 0U);

    clear_sent();
    const uint8_t bitstream_write[] = {24U, 0U, 0x12U, 0x34U, 0x56U};
    request_size = make_request(request, 0x0701U, 34U,
                                bitstream_object, bitstream_write,
                                sizeof(bitstream_write));
    feed_packet(&core, 0x619U, 34U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U && timed_bit_count == 24U);
    assert(memcmp(timed_bit_data, bitstream_write + 2U, 3U) == 0);

    put_u32(observed_resource_id, 0x0A000000U);
    clear_sent();
    request_size = make_request(request, 0x0032U, 302U, 0U,
                                observed_resource_id,
                                sizeof(observed_resource_id));
    feed_packet(&core, 0x619U, 302U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 50U);
    assert(response[24U] == 0U && response[29U] == 1U);

    core.timed_bitstream_status[0].backend_failed = true;
    clear_sent();
    request_size = make_request(request, 0x0032U, 303U, 0U,
                                observed_resource_id,
                                sizeof(observed_resource_id));
    feed_packet(&core, 0x619U, 303U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 50U);
    assert(response[24U] == 0U && response[29U] == 3U);
    assert(get_u32(response + 30U) == 4U);

    clear_sent();
    request_size = make_request(request, 0x0702U, 35U,
                                bitstream_object, NULL, 0U);
    feed_packet(&core, 0x619U, 35U, request, request_size);
    reassemble_sent(response, 0x599U);
    assert(response[24] == 0U && !timed_bit_busy);

    clear_sent();
    const uint8_t bootloader_confirmation[] = {
        'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};
    request_size = make_request(
        request, 0x0013U, 9U, 0U,
        bootloader_confirmation,
        sizeof(bootloader_confirmation));
    now_ms = 100U;
    feed_packet(&core, 0x619U, 12U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24] == 0U);
    assert(core.bootloader_request_pending);
    assert(bootloader_enter_count == 0U);

    now_ms = 199U;
    rbsp_core_poll(&core);
    assert(bootloader_enter_count == 0U);
    now_ms = 200U;
    rbsp_core_poll(&core);
    assert(bootloader_enter_count == 1U);
    assert(last_bootloader_mode == RBSP_BOOTLOADER_CAN);
    assert(!core.bootloader_request_pending);

    clear_sent();
    request_size = make_request(
        request, 0x0014U, 10U, 0U,
        bootloader_confirmation,
        sizeof(bootloader_confirmation));
    now_ms = 300U;
    feed_packet(&core, 0x619U, 13U, request, request_size);
    assert(reassemble_sent(response, 0x599U) == 25U);
    assert(response[24] == 0U);
    assert(core.bootloader_request_pending);

    now_ms = 400U;
    rbsp_core_poll(&core);
    assert(bootloader_enter_count == 2U);
    assert(last_bootloader_mode == RBSP_BOOTLOADER_USB);
    assert(!core.bootloader_request_pending);

    clear_sent();
    now_ms = 500U;
    rbsp_core_poll(&core);
    reassemble_sent(response, 0x519U);
    assert(response[1] == 3U);
    assert(response[2] == 4U);

    puts("嵌入式远程核心测试通过");
    return 0;
}
