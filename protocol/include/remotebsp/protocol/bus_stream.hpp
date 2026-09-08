#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kBusResourceContractVersion = 1;
constexpr std::uint16_t kStreamContractVersion = 1;
constexpr std::uint16_t kMaximumAtomicBusTransferBytes = 1024;
constexpr std::uint16_t kMaximumStreamChunkBytes = 1024;

enum class BusResourceKind : std::uint8_t {
    I2cBus = 1,
    I2cDevice = 2,
    SpiBus = 3,
    SpiDevice = 4,
};

constexpr std::uint8_t kBusContractRepeatedStart = 0x01;
constexpr std::uint8_t kBusContractRecovery = 0x02;
constexpr std::uint8_t kBusContractFullDuplex = 0x04;
constexpr std::uint8_t kBusContractKeepChipSelect = 0x08;

// 设备资源通过 parent_bus_resource_id 绑定到静态总线；总线资源该字段为零。
struct BusResourceContract {
    std::uint32_t resource_id{};
    std::uint16_t version{kBusResourceContractVersion};
    BusResourceKind kind{BusResourceKind::I2cBus};
    std::uint8_t flags{};
    std::uint32_t parent_bus_resource_id{};
    std::uint32_t maximum_clock_hz{};
    std::uint16_t maximum_transfer_bytes{};
    std::uint16_t queue_capacity{};
    std::uint32_t minimum_timeout_us{};
    std::uint32_t maximum_timeout_us{};
    std::uint32_t maximum_operations_per_second{};
};

constexpr std::uint16_t kI2cTransferRepeatedStart = 0x0001;
constexpr std::uint16_t kI2cTransferAllowRecovery = 0x0002;

struct I2cTransferRequest {
    std::uint32_t device_resource_id{};
    std::uint32_t timeout_us{};
    std::uint16_t flags{};
    std::uint16_t read_length{};
    std::vector<std::uint8_t> write_data;
};

constexpr std::uint16_t kSpiTransferKeepChipSelect = 0x0001;

struct SpiTransferRequest {
    std::uint32_t device_resource_id{};
    std::uint32_t timeout_us{};
    std::uint16_t flags{};
    std::uint16_t receive_length{};
    std::uint8_t dummy_byte{0xFF};
    std::vector<std::uint8_t> transmit_data;
};

enum class BusTransactionStatus : std::uint8_t {
    Ok = 0,
    Nack = 1,
    Timeout = 2,
    Busy = 3,
    Fault = 4,
    LimitExceeded = 5,
};

struct BusTransferResult {
    BusTransactionStatus status{BusTransactionStatus::Ok};
    std::uint16_t transmitted{};
    std::uint16_t received{};
    std::vector<std::uint8_t> data;
};

enum class StreamDirection : std::uint8_t {
    HostToNode = 1,
    NodeToHost = 2,
    Bidirectional = 3,
};

constexpr std::uint8_t kStreamTransportCan = 0x01;
constexpr std::uint8_t kStreamTransportUsb = 0x02;
constexpr std::uint8_t kStreamTransportEthernet = 0x04;
constexpr std::uint16_t kStreamFlagLossless = 0x0001;
constexpr std::uint16_t kStreamFlagTimestamped = 0x0002;
constexpr std::uint16_t kStreamFlagCreditRequired = 0x0004;
constexpr std::uint16_t kStreamDataEndOfRecord = 0x0001;
constexpr std::uint16_t kStreamDataTimestampValid = 0x0002;

struct StreamContract {
    std::uint32_t resource_id{};
    std::uint16_t version{kStreamContractVersion};
    StreamDirection direction{StreamDirection::NodeToHost};
    std::uint8_t transport_mask{};
    std::uint16_t flags{};
    std::uint16_t maximum_chunk_bytes{};
    std::uint32_t buffer_capacity_bytes{};
    std::uint32_t sustained_bits_per_second{};
    std::uint32_t peak_bits_per_second{};
    std::uint32_t maximum_latency_us{};
    std::uint32_t maximum_jitter_us{};
};

struct StreamOpenRequest {
    std::uint32_t resource_id{};
    std::uint16_t requested_chunk_bytes{};
    std::uint16_t requested_flags{};
    std::uint32_t initial_credit_bytes{};
};

struct StreamOpenResponse {
    std::uint32_t stream_id{};
    std::uint16_t negotiated_chunk_bytes{};
    std::uint16_t negotiated_flags{};
    std::uint32_t available_credit_bytes{};
};

struct StreamDataPayload {
    std::uint32_t stream_id{};
    std::uint32_t sequence{};
    std::uint16_t flags{};
    std::uint64_t timestamp_ns{};
    std::vector<std::uint8_t> data;
};

struct StreamCreditPayload {
    std::uint32_t stream_id{};
    std::uint32_t credit_bytes{};
    std::uint32_t acknowledged_sequence{};
};

enum class StreamState : std::uint8_t {
    Open = 1,
    Running = 2,
    Backpressured = 3,
    Stopped = 4,
    Failed = 5,
};

struct StreamStatusPayload {
    std::uint32_t stream_id{};
    StreamState state{StreamState::Open};
    std::uint32_t buffered_bytes{};
    std::uint32_t available_credit_bytes{};
    std::uint32_t dropped_bytes{};
    std::uint32_t next_sequence{};
};

enum class BusStreamPayloadError {
    InvalidLength,
    InvalidVersion,
    InvalidKind,
    InvalidStatus,
    InvalidDirection,
    InvalidFlags,
    LimitExceeded,
};

class BusStreamPayloadException : public std::runtime_error {
public:
    BusStreamPayloadException(BusStreamPayloadError code,
                              const char* message);
    BusStreamPayloadError code() const noexcept;

private:
    BusStreamPayloadError code_;
};

std::vector<std::uint8_t> encode_bus_resource_contract(
    const BusResourceContract& contract);
BusResourceContract decode_bus_resource_contract(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_i2c_transfer_request(
    const I2cTransferRequest& request);
I2cTransferRequest decode_i2c_transfer_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_spi_transfer_request(
    const SpiTransferRequest& request);
SpiTransferRequest decode_spi_transfer_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_bus_transfer_result(
    const BusTransferResult& result);
BusTransferResult decode_bus_transfer_result(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_stream_contract(
    const StreamContract& contract);
StreamContract decode_stream_contract(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_stream_open_request(
    const StreamOpenRequest& request);
StreamOpenRequest decode_stream_open_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_stream_open_response(
    const StreamOpenResponse& response);
StreamOpenResponse decode_stream_open_response(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_stream_data(
    const StreamDataPayload& data);
StreamDataPayload decode_stream_data(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_stream_credit(
    const StreamCreditPayload& credit);
StreamCreditPayload decode_stream_credit(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_stream_status(
    const StreamStatusPayload& status);
StreamStatusPayload decode_stream_status(
    const std::vector<std::uint8_t>& payload);

}  // namespace remotebsp::protocol
