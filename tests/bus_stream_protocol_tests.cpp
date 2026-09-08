#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

template <typename Function>
void expect_payload_error(Function function,
                          remotebsp::protocol::BusStreamPayloadError code) {
    try {
        function();
        assert(false);
    } catch (const remotebsp::protocol::BusStreamPayloadException& error) {
        assert(error.code() == code);
    }
}

}  // namespace

int main() {
    using namespace remotebsp::protocol;

    const BusResourceContract i2c_contract{
        0x0C000001, kBusResourceContractVersion,
        BusResourceKind::I2cDevice,
        static_cast<std::uint8_t>(kBusContractRepeatedStart |
                                  kBusContractRecovery),
        0x0B000000, 400000, 128, 4, 100, 20000, 500};
    const auto decoded_contract = decode_bus_resource_contract(
        encode_bus_resource_contract(i2c_contract));
    assert(decoded_contract.resource_id == i2c_contract.resource_id);
    assert(decoded_contract.kind == BusResourceKind::I2cDevice);
    assert(decoded_contract.parent_bus_resource_id == 0x0B000000);
    assert(decoded_contract.maximum_transfer_bytes == 128);

    const I2cTransferRequest i2c_request{
        0x0C000001, 2000, kI2cTransferRepeatedStart, 4, {0x10}};
    const auto decoded_i2c = decode_i2c_transfer_request(
        encode_i2c_transfer_request(i2c_request));
    assert(decoded_i2c.timeout_us == 2000);
    assert(decoded_i2c.read_length == 4);
    assert(decoded_i2c.write_data == std::vector<std::uint8_t>{0x10});

    const SpiTransferRequest spi_request{
        0x0E000001, 1000, kSpiTransferKeepChipSelect, 3, 0xAA,
        {0x80, 0x01}};
    const auto decoded_spi = decode_spi_transfer_request(
        encode_spi_transfer_request(spi_request));
    assert(decoded_spi.flags == kSpiTransferKeepChipSelect);
    assert(decoded_spi.receive_length == 3);
    assert(decoded_spi.dummy_byte == 0xAA);

    const BusTransferResult result{BusTransactionStatus::Nack, 1, 0, {}};
    const auto decoded_result = decode_bus_transfer_result(
        encode_bus_transfer_result(result));
    assert(decoded_result.status == BusTransactionStatus::Nack);
    assert(decoded_result.transmitted == 1);

    const StreamContract stream_contract{
        0x0F000001, kStreamContractVersion,
        StreamDirection::NodeToHost,
        static_cast<std::uint8_t>(kStreamTransportCan |
                                  kStreamTransportUsb),
        static_cast<std::uint16_t>(kStreamFlagTimestamped |
                                   kStreamFlagCreditRequired),
        256, 4096, 1000000, 4000000, 10000, 1000};
    const auto decoded_stream_contract = decode_stream_contract(
        encode_stream_contract(stream_contract));
    assert(decoded_stream_contract.maximum_chunk_bytes == 256);
    assert(decoded_stream_contract.buffer_capacity_bytes == 4096);

    const StreamOpenRequest open_request{0x0F000001, 128,
                                         kStreamFlagCreditRequired, 512};
    assert(decode_stream_open_request(encode_stream_open_request(open_request))
               .initial_credit_bytes == 512);
    const StreamOpenResponse open_response{7, 128,
                                           kStreamFlagCreditRequired, 512};
    assert(decode_stream_open_response(
               encode_stream_open_response(open_response))
               .stream_id == 7);

    const StreamDataPayload stream_data{
        7, 12,
        static_cast<std::uint16_t>(kStreamDataEndOfRecord |
                                   kStreamDataTimestampValid),
        123456789, {1, 2, 3, 4}};
    const auto decoded_data = decode_stream_data(encode_stream_data(stream_data));
    assert(decoded_data.sequence == 12);
    assert(decoded_data.timestamp_ns == 123456789);
    assert(decoded_data.data == stream_data.data);

    const StreamCreditPayload credit{7, 256, 12};
    assert(decode_stream_credit(encode_stream_credit(credit)).credit_bytes ==
           256);
    const StreamStatusPayload status{7, StreamState::Running, 128, 256, 3,
                                     13};
    assert(decode_stream_status(encode_stream_status(status)).dropped_bytes ==
           3);

    // 旧 v1 扁平资源数值保持不变，新增类型只追加在枚举尾部。
    static_assert(static_cast<std::uint8_t>(ResourceType::Spi) == 3);
    static_assert(static_cast<std::uint8_t>(ResourceType::I2c) == 4);
    const ResourceDescriptor legacy{3, ResourceType::Spi, 0, 0, 0, 0};
    assert(decode_resource_descriptor(encode_resource_descriptor(legacy)).type ==
           ResourceType::Spi);

    expect_payload_error(
        [] {
            encode_i2c_transfer_request(
                {1, 1000, 0, kMaximumAtomicBusTransferBytes,
                 std::vector<std::uint8_t>(1, 0)});
        },
        BusStreamPayloadError::LimitExceeded);
    expect_payload_error(
        [] {
            decode_stream_data(std::vector<std::uint8_t>(21, 0));
        },
        BusStreamPayloadError::InvalidLength);

    expect_payload_error(
        [] {
            BusResourceContract invalid{
                0, kBusResourceContractVersion, BusResourceKind::I2cBus,
                0, 0, 100000, 16, 1, 100, 1000, 100};
            encode_bus_resource_contract(invalid);
        },
        BusStreamPayloadError::LimitExceeded);
    expect_payload_error(
        [] {
            BusResourceContract invalid{
                1, kBusResourceContractVersion, BusResourceKind::SpiBus,
                0x80, 0, 1000000, 16, 1, 100, 1000, 100};
            encode_bus_resource_contract(invalid);
        },
        BusStreamPayloadError::InvalidFlags);
    expect_payload_error(
        [] {
            BusResourceContract invalid{
                1, kBusResourceContractVersion, BusResourceKind::SpiBus,
                0, 9, 1000000, 16, 1, 100, 1000, 100};
            encode_bus_resource_contract(invalid);
        },
        BusStreamPayloadError::LimitExceeded);
    expect_payload_error(
        [] {
            BusResourceContract invalid{
                1, kBusResourceContractVersion, BusResourceKind::I2cBus,
                0, 0, 0, 16, 1, 100, 1000, 100};
            encode_bus_resource_contract(invalid);
        },
        BusStreamPayloadError::LimitExceeded);
    expect_payload_error(
        [] {
            BusResourceContract invalid{
                1, kBusResourceContractVersion,
                BusResourceKind::I2cDevice, 0, 0, 100000, 16, 1,
                100, 1000, 100};
            encode_bus_resource_contract(invalid);
        },
        BusStreamPayloadError::LimitExceeded);
    expect_payload_error(
        [] {
            std::vector<std::uint8_t> invalid{0, 0x01, 0x04, 0, 0};
            decode_bus_transfer_result(invalid);
        },
        BusStreamPayloadError::InvalidStatus);
    expect_payload_error(
        [] {
            StreamContract invalid{
                1, kStreamContractVersion, StreamDirection::NodeToHost,
                0x80, kStreamFlagCreditRequired, 64, 128, 100, 100,
                1000, 100};
            encode_stream_contract(invalid);
        },
        BusStreamPayloadError::InvalidFlags);
    expect_payload_error(
        [] {
            StreamContract invalid{
                1, kStreamContractVersion, StreamDirection::NodeToHost,
                kStreamTransportUsb, 0x8000, 64, 128, 100, 100,
                1000, 100};
            encode_stream_contract(invalid);
        },
        BusStreamPayloadError::InvalidFlags);
    expect_payload_error(
        [] {
            encode_stream_open_request({1, 64, 0x8000, 0});
        },
        BusStreamPayloadError::InvalidFlags);
    expect_payload_error(
        [] {
            encode_stream_data({1, 0, 0x8000, 0, {1}});
        },
        BusStreamPayloadError::InvalidFlags);
}
