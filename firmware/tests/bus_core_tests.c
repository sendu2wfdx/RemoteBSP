#include "remotebsp_embedded/core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    I2C_BUS_ID = 0x0B000001U,
    I2C_DEVICE_ID = 0x0C000001U,
    I2C_DEVICE_2_ID = 0x0C000002U,
    SPI_BUS_ID = 0x0D000001U,
    SPI_DEVICE_ID = 0x0E000001U,
};

static rbsp_link_frame_t sent_frames[256];
static size_t sent_count;
static uint32_t now_ms;
static unsigned i2c_calls;
static unsigned spi_calls;
static rbsp_bus_transaction_status_t i2c_status = RBSP_BUS_TRANSACTION_OK;
static bool i2c_claims_unwritten_data;
static bool i2c_returns_short_success;

static const rbsp_bus_resource_config_t resources[] = {
    {I2C_BUS_ID, 0U, 400000U, 100U, 100000U, 1000U,
     128U, 1U, 0U, 1U, RBSP_BUS_I2C_BUS,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START |
         RBSP_BUS_CONTRACT_I2C_RECOVERY,
     0U, 0U},
    {I2C_DEVICE_ID, I2C_BUS_ID, 400000U, 100U, 100000U, 1000U,
     128U, 1U, 0x48U, 1U, RBSP_BUS_I2C_DEVICE,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START |
         RBSP_BUS_CONTRACT_I2C_RECOVERY,
     0U, 0U},
    {I2C_DEVICE_2_ID, I2C_BUS_ID, 100000U, 100U, 100000U, 1000U,
     128U, 1U, 0x49U, 1U, RBSP_BUS_I2C_DEVICE,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START,
     0U, 0U},
    {SPI_BUS_ID, 0U, 8000000U, 100U, 100000U, 1000U,
     128U, 1U, 0U, 2U, RBSP_BUS_SPI_BUS,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     0U, 0U},
    {SPI_DEVICE_ID, SPI_BUS_ID, 4000000U, 100U, 100000U, 1000U,
     128U, 1U, 10U, 2U, RBSP_BUS_SPI_DEVICE,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     3U, 8U},
};

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
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8U) |
           ((uint32_t)input[2] << 16U) | ((uint32_t)input[3] << 24U);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static uint16_t make_request(uint8_t* packet, uint16_t command,
                             uint32_t session_id, uint32_t request_id,
                             const uint8_t* payload, uint16_t payload_length) {
    const uint16_t size = (uint16_t)(24U + payload_length);
    memset(packet, 0, size);
    packet[0] = 1U;
    packet[1] = 1U;
    put_u16(packet + 2U, command);
    put_u32(packet + 4U, session_id);
    put_u32(packet + 8U, request_id);
    put_u16(packet + 16U, payload_length);
    if (payload_length != 0U) memcpy(packet + 24U, payload, payload_length);
    uint32_t crc = crc32_update(0xFFFFFFFFU, packet, 20U);
    crc = crc32_update(crc, packet + 24U, payload_length);
    put_u32(packet + 20U, ~crc);
    return size;
}

static bool send_frame(const rbsp_link_frame_t* frame) {
    assert(sent_count < sizeof(sent_frames) / sizeof(sent_frames[0]));
    sent_frames[sent_count++] = *frame;
    return true;
}

static uint32_t milliseconds(void) { return now_ms; }

static rbsp_bus_transaction_status_t i2c_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, const uint8_t* write_data, uint16_t write_length,
    uint8_t* read_data, uint16_t read_length,
    uint16_t* transmitted, uint16_t* received) {
    assert((device->resource_id == I2C_DEVICE_ID ||
            device->resource_id == I2C_DEVICE_2_ID) &&
           timeout_us >= 100U);
    (void)flags;
    (void)write_data;
    ++i2c_calls;
    if (i2c_status != RBSP_BUS_TRANSACTION_OK) {
        const rbsp_bus_transaction_status_t result = i2c_status;
        i2c_status = RBSP_BUS_TRANSACTION_OK;
        if (i2c_claims_unwritten_data) {
            *received = read_length;
            i2c_claims_unwritten_data = false;
        }
        return result;
    }
    if (i2c_returns_short_success) {
        i2c_returns_short_success = false;
        return RBSP_BUS_TRANSACTION_OK;
    }
    *transmitted = write_length;
    *received = read_length;
    memset(read_data, 0xA5, read_length);
    return RBSP_BUS_TRANSACTION_OK;
}

static rbsp_bus_transaction_status_t spi_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, uint8_t dummy_byte,
    const uint8_t* transmit_data, uint16_t transmit_length,
    uint8_t* receive_data, uint16_t receive_length,
    uint16_t* transmitted, uint16_t* received) {
    assert(device->resource_id == SPI_DEVICE_ID && timeout_us >= 100U);
    (void)flags;
    (void)dummy_byte;
    (void)transmit_data;
    ++spi_calls;
    *transmitted = transmit_length;
    *received = receive_length;
    memset(receive_data, 0x5A, receive_length);
    return RBSP_BUS_TRANSACTION_OK;
}

static void feed(rbsp_core_t* core, const uint8_t* packet, uint16_t size,
                 uint16_t transfer_id) {
    uint16_t offset = 0U;
    uint16_t sequence = 0U;
    while (offset < size) {
        const uint16_t remaining = (uint16_t)(size - offset);
        const uint8_t length = remaining > 3U ? 3U : (uint8_t)remaining;
        rbsp_link_frame_t frame = {0};
        frame.route = 0x619U;
        put_u16(frame.data, transfer_id);
        put_u16(frame.data + 2U, sequence);
        frame.data[4U] = (uint8_t)(length << 2U);
        if (sequence == 0U) frame.data[4U] |= 1U;
        if ((uint16_t)(offset + length) == size) frame.data[4U] |= 2U;
        memcpy(frame.data + 5U, packet + offset, length);
        frame.length = (uint8_t)(5U + length);
        rbsp_core_accept_link(core, &frame);
        offset = (uint16_t)(offset + length);
        ++sequence;
    }
}

static uint16_t response(uint8_t* packet) {
    uint16_t size = 0U;
    for (size_t index = 0U; index < sent_count; ++index) {
        assert(sent_frames[index].route == 0x599U);
        const uint8_t length = sent_frames[index].data[4U] >> 2U;
        memcpy(packet + size, sent_frames[index].data + 5U, length);
        size = (uint16_t)(size + length);
    }
    assert(size == (uint16_t)(24U + packet[16U] +
                              ((uint16_t)packet[17U] << 8U)));
    return size;
}

static uint16_t exchange(rbsp_core_t* core, uint8_t* request,
                         uint16_t request_size, uint32_t request_id,
                         uint8_t* output) {
    sent_count = 0U;
    feed(core, request, request_size, (uint16_t)request_id);
    return response(output);
}

static void acquire(rbsp_core_t* core, uint32_t resource_id,
                    uint32_t session_id, uint32_t request_id) {
    uint8_t payload[9U] = {0};
    uint8_t request[64U];
    uint8_t output[256U];
    put_u32(payload, resource_id);
    put_u32(payload + 4U, 1000U);
    payload[8U] = 2U;
    const uint16_t size = make_request(request, 0x0035U, session_id,
                                       request_id, payload, sizeof(payload));
    assert(exchange(core, request, size, request_id, output) == 52U);
    assert(output[24U] == 0U && get_u32(output + 25U) == resource_id);
}

int main(void) {
    rbsp_hal_t hal = {
        .link_send = send_frame,
        .milliseconds = milliseconds,
        .bus_resources = resources,
        .bus_resource_count = 5U,
        .i2c_transfer = i2c_transfer,
        .spi_transfer = spi_transfer,
    };
    const rbsp_node_info_t info = {{0}, 1U, 0U, 0U, 1U};
    rbsp_core_t core;
    assert(rbsp_core_init(&core, &hal, RBSP_CAN_CLASSICAL, &info));
    core.node_id = 25U;

    uint8_t request[256U];
    uint8_t output[256U];
    uint8_t transfer[160U] = {0};

    uint8_t contract_query[4U];
    put_u32(contract_query, SPI_DEVICE_ID);
    uint16_t size = make_request(request, 0x0400U, 11U, 50U,
                                 contract_query, sizeof(contract_query));
    assert(exchange(&core, request, size, 50U, output) == 57U);
    assert(output[24U] == 0U && get_u32(output + 25U) == SPI_DEVICE_ID);
    assert(output[29U] == 1U && output[31U] == RBSP_BUS_SPI_DEVICE);
    assert(output[32U] == (RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
                           RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT));

    put_u32(transfer, I2C_DEVICE_ID);
    put_u32(transfer + 4U, 1000U);
    put_u16(transfer + 10U, 2U);
    put_u16(transfer + 12U, 1U);
    transfer[14U] = 7U;
    size = make_request(request, 0x0301U, 11U, 1U, transfer, 15U);
    assert(exchange(&core, request, size, 1U, output) == 25U);
    assert(output[24U] == 4U && i2c_calls == 0U);

    acquire(&core, I2C_DEVICE_ID, 11U, 2U);
    size = make_request(request, 0x0301U, 11U, 3U, transfer, 15U);
    assert(exchange(&core, request, size, 3U, output) == 32U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_OK);
    assert(output[30U] == 0xA5U && output[31U] == 0xA5U && i2c_calls == 1U);
    assert(exchange(&core, request, size, 3U, output) == 32U);
    assert(i2c_calls == 1U);

    memset(transfer, 0, sizeof(transfer));
    put_u32(transfer, I2C_DEVICE_ID);
    put_u32(transfer + 4U, 1000U);
    put_u16(transfer + 10U, 128U);
    size = make_request(request, 0x0301U, 11U, 40U, transfer, 14U);
    assert(exchange(&core, request, size, 40U, output) == 158U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_OK);
    assert(output[30U] == 0xA5U && output[157U] == 0xA5U &&
           i2c_calls == 2U);
    assert(exchange(&core, request, size, 40U, output) == 158U);
    assert(i2c_calls == 2U);

    now_ms = 1000U;
    memset(transfer, 0, sizeof(transfer));
    put_u32(transfer, I2C_DEVICE_ID);
    put_u32(transfer + 4U, 1000U);
    put_u16(transfer + 10U, 2U);
    put_u16(transfer + 12U, 1U);
    transfer[14U] = 7U;
    size = make_request(request, 0x0301U, 11U, 4U, transfer, 15U);
    assert(exchange(&core, request, size, 4U, output) == 25U);
    assert(output[24U] == 4U && i2c_calls == 2U);

    now_ms = 1001U;
    acquire(&core, I2C_DEVICE_ID, 11U, 5U);
    i2c_status = RBSP_BUS_TRANSACTION_BUSY;
    size = make_request(request, 0x0301U, 11U, 6U, transfer, 15U);
    assert(exchange(&core, request, size, 6U, output) == 30U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_BUSY);

    /* 错误 HAL 声称收到数据但未写缓冲时，不得带出旧响应内容。 */
    memset(core.tx_packet, 0xCC, sizeof(core.tx_packet));
    i2c_status = RBSP_BUS_TRANSACTION_TIMEOUT;
    i2c_claims_unwritten_data = true;
    size = make_request(request, 0x0301U, 11U, 43U, transfer, 15U);
    assert(exchange(&core, request, size, 43U, output) == 32U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_TIMEOUT);
    assert(output[30U] == 0U && output[31U] == 0U);

    /* 原子事务不能把短收发伪装成成功。 */
    i2c_returns_short_success = true;
    size = make_request(request, 0x0301U, 11U, 44U, transfer, 15U);
    assert(exchange(&core, request, size, 44U, output) == 25U);
    assert(output[24U] == 7U);

    /* 同一 I2C 控制器的一次 Busy 只属于该事务，不污染另一个设备。 */
    acquire(&core, I2C_DEVICE_2_ID, 33U, 41U);
    put_u32(transfer, I2C_DEVICE_2_ID);
    size = make_request(request, 0x0301U, 33U, 42U, transfer, 15U);
    assert(exchange(&core, request, size, 42U, output) == 32U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_OK);
    assert(i2c_calls == 6U);

    acquire(&core, SPI_DEVICE_ID, 22U, 7U);
    memset(transfer, 0, sizeof(transfer));
    put_u32(transfer, SPI_DEVICE_ID);
    put_u32(transfer + 4U, 1000U);
    put_u16(transfer + 8U, RBSP_BUS_SPI_TRANSFER_KEEP_CHIP_SELECT);
    put_u16(transfer + 10U, 2U);
    transfer[12U] = 0xFFU;
    put_u16(transfer + 13U, 1U);
    transfer[15U] = 9U;
    size = make_request(request, 0x0401U, 22U, 8U, transfer, 16U);
    assert(exchange(&core, request, size, 8U, output) == 32U);
    assert(output[24U] == 0U && output[25U] == RBSP_BUS_TRANSACTION_OK);
    assert(output[30U] == 0x5AU && output[31U] == 0x5AU && spi_calls == 1U);

    assert(rbsp_core_release_session(&core, 22U) == 1U);
    assert(!core.bus_leases[4U].active);
    size = make_request(request, 0x0401U, 22U, 9U, transfer, 16U);
    assert(exchange(&core, request, size, 9U, output) == 25U);
    assert(output[24U] == 4U && spi_calls == 1U);

    /* 32 位毫秒计数器回绕期间，租约仍按有符号差值准时失效。 */
    assert(rbsp_core_release_session(&core, 33U) == 1U);
    now_ms = UINT32_MAX - 50U;
    acquire(&core, I2C_DEVICE_2_ID, 44U, 60U);
    now_ms = 20U;
    rbsp_core_poll(&core);
    assert(core.bus_leases[2U].active);
    now_ms = 949U;
    rbsp_core_poll(&core);
    assert(!core.bus_leases[2U].active);

    rbsp_bus_resource_config_t invalid[5U];
    memcpy(invalid, resources, sizeof(invalid));
    invalid[0U].queue_capacity = 2U;
    hal.bus_resources = invalid;
    assert(!rbsp_core_init(&core, &hal, RBSP_CAN_CLASSICAL, &info));

    /* 类型高字节不匹配会与 STEPGEN 等资源命名空间冲突。 */
    memcpy(invalid, resources, sizeof(invalid));
    invalid[1U].resource_id = 0x09000000U;
    hal.bus_resources = invalid;
    assert(!rbsp_core_init(&core, &hal, RBSP_CAN_CLASSICAL, &info));

    puts("嵌入式 I2C/SPI 公共核心测试通过");
    return 0;
}
