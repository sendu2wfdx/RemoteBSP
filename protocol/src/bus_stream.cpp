#include "remotebsp/protocol/bus_stream.hpp"

#include <algorithm>
#include <cstddef>

namespace remotebsp::protocol {
namespace {

constexpr std::size_t kBusContractSize = 32;
constexpr std::size_t kI2cHeaderSize = 14;
constexpr std::size_t kSpiHeaderSize = 15;
constexpr std::size_t kBusResultHeaderSize = 5;
constexpr std::size_t kStreamContractSize = 32;
constexpr std::size_t kStreamOpenSize = 12;
constexpr std::size_t kStreamDataHeaderSize = 20;
constexpr std::size_t kStreamCreditSize = 12;
constexpr std::size_t kStreamStatusSize = 21;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(input[1] << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

bool valid_bus_kind(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(BusResourceKind::I2cBus) &&
           value <= static_cast<std::uint8_t>(BusResourceKind::SpiDevice);
}

bool valid_direction(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(StreamDirection::HostToNode) &&
           value <= static_cast<std::uint8_t>(StreamDirection::Bidirectional);
}

bool valid_bus_flags(BusResourceKind kind, std::uint8_t flags) {
    std::uint8_t allowed = 0;
    if (kind == BusResourceKind::I2cBus ||
        kind == BusResourceKind::I2cDevice) {
        allowed = kBusContractRepeatedStart | kBusContractRecovery;
    } else {
        allowed = kBusContractFullDuplex |
                  kBusContractKeepChipSelect;
    }
    return (flags & static_cast<std::uint8_t>(~allowed)) == 0;
}

void validate_transfer_size(std::size_t first, std::size_t second) {
    if (first > kMaximumAtomicBusTransferBytes ||
        second > kMaximumAtomicBusTransferBytes ||
        first + second > kMaximumAtomicBusTransferBytes) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "原子总线事务超过协议上限");
    }
}

void validate_spi_transfer_size(std::size_t transmit,
                                std::size_t receive) {
    if (transmit > kMaximumAtomicBusTransferBytes ||
        receive > kMaximumAtomicBusTransferBytes ||
        std::max(transmit, receive) > kMaximumAtomicBusTransferBytes) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "SPI 原子事务超过协议上限");
    }
}

}  // namespace

BusStreamPayloadException::BusStreamPayloadException(
    BusStreamPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

BusStreamPayloadError BusStreamPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_bus_resource_contract(
    const BusResourceContract& contract) {
    if (contract.version != kBusResourceContractVersion) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidVersion,
                                        "总线资源合同版本无效");
    }
    if (!valid_bus_kind(static_cast<std::uint8_t>(contract.kind))) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidKind,
                                        "总线资源类型无效");
    }
    const bool is_bus = contract.kind == BusResourceKind::I2cBus ||
                        contract.kind == BusResourceKind::SpiBus;
    if (!valid_bus_flags(contract.kind, contract.flags)) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "总线资源合同包含未知标志");
    }
    if (contract.resource_id == 0 || contract.maximum_clock_hz == 0 ||
        (is_bus && contract.parent_bus_resource_id != 0) ||
        (!is_bus && contract.parent_bus_resource_id == 0) ||
        contract.maximum_transfer_bytes == 0 ||
        contract.maximum_transfer_bytes > kMaximumAtomicBusTransferBytes ||
        contract.queue_capacity == 0 || contract.minimum_timeout_us == 0 ||
        contract.maximum_timeout_us < contract.minimum_timeout_us) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "总线资源合同边界无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kBusContractSize);
    append_u32(output, contract.resource_id);
    append_u16(output, contract.version);
    output.push_back(static_cast<std::uint8_t>(contract.kind));
    output.push_back(contract.flags);
    append_u32(output, contract.parent_bus_resource_id);
    append_u32(output, contract.maximum_clock_hz);
    append_u16(output, contract.maximum_transfer_bytes);
    append_u16(output, contract.queue_capacity);
    append_u32(output, contract.minimum_timeout_us);
    append_u32(output, contract.maximum_timeout_us);
    append_u32(output, contract.maximum_operations_per_second);
    return output;
}

BusResourceContract decode_bus_resource_contract(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kBusContractSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "总线资源合同长度无效");
    }
    if (read_u16(payload.data() + 4) != kBusResourceContractVersion) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidVersion,
                                        "总线资源合同版本不受支持");
    }
    if (!valid_bus_kind(payload[6])) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidKind,
                                        "总线资源类型无效");
    }
    BusResourceContract contract{
        read_u32(payload.data()), read_u16(payload.data() + 4),
        static_cast<BusResourceKind>(payload[6]), payload[7],
        read_u32(payload.data() + 8), read_u32(payload.data() + 12),
        read_u16(payload.data() + 16), read_u16(payload.data() + 18),
        read_u32(payload.data() + 20), read_u32(payload.data() + 24),
        read_u32(payload.data() + 28)};
    static_cast<void>(encode_bus_resource_contract(contract));
    return contract;
}

std::vector<std::uint8_t> encode_i2c_transfer_request(
    const I2cTransferRequest& request) {
    validate_transfer_size(request.write_data.size(), request.read_length);
    if (request.device_resource_id == 0 || request.timeout_us == 0 ||
        (request.flags & ~(kI2cTransferRepeatedStart |
                           kI2cTransferAllowRecovery)) != 0 ||
        (request.write_data.empty() && request.read_length == 0)) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "I2C 原子事务参数无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kI2cHeaderSize + request.write_data.size());
    append_u32(output, request.device_resource_id);
    append_u32(output, request.timeout_us);
    append_u16(output, request.flags);
    append_u16(output, request.read_length);
    append_u16(output, static_cast<std::uint16_t>(request.write_data.size()));
    output.insert(output.end(), request.write_data.begin(),
                  request.write_data.end());
    return output;
}

I2cTransferRequest decode_i2c_transfer_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kI2cHeaderSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "I2C 原子事务长度无效");
    }
    const auto write_length = read_u16(payload.data() + 12);
    if (payload.size() != kI2cHeaderSize + write_length) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "I2C 写入长度与载荷不匹配");
    }
    I2cTransferRequest request{
        read_u32(payload.data()), read_u32(payload.data() + 4),
        read_u16(payload.data() + 8), read_u16(payload.data() + 10),
        {payload.begin() + static_cast<std::ptrdiff_t>(kI2cHeaderSize),
         payload.end()}};
    static_cast<void>(encode_i2c_transfer_request(request));
    return request;
}

std::vector<std::uint8_t> encode_spi_transfer_request(
    const SpiTransferRequest& request) {
    validate_spi_transfer_size(request.transmit_data.size(),
                               request.receive_length);
    if (request.device_resource_id == 0 || request.timeout_us == 0 ||
        (request.flags & ~kSpiTransferKeepChipSelect) != 0 ||
        (request.transmit_data.empty() && request.receive_length == 0)) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "SPI 原子事务参数无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kSpiHeaderSize + request.transmit_data.size());
    append_u32(output, request.device_resource_id);
    append_u32(output, request.timeout_us);
    append_u16(output, request.flags);
    append_u16(output, request.receive_length);
    output.push_back(request.dummy_byte);
    append_u16(output,
               static_cast<std::uint16_t>(request.transmit_data.size()));
    output.insert(output.end(), request.transmit_data.begin(),
                  request.transmit_data.end());
    return output;
}

SpiTransferRequest decode_spi_transfer_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kSpiHeaderSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "SPI 原子事务长度无效");
    }
    const auto transmit_length = read_u16(payload.data() + 13);
    if (payload.size() != kSpiHeaderSize + transmit_length) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "SPI 发送长度与载荷不匹配");
    }
    SpiTransferRequest request{
        read_u32(payload.data()), read_u32(payload.data() + 4),
        read_u16(payload.data() + 8), read_u16(payload.data() + 10),
        payload[12],
        {payload.begin() + static_cast<std::ptrdiff_t>(kSpiHeaderSize),
         payload.end()}};
    static_cast<void>(encode_spi_transfer_request(request));
    return request;
}

std::vector<std::uint8_t> encode_bus_transfer_result(
    const BusTransferResult& result) {
    if (result.status > BusTransactionStatus::LimitExceeded ||
        result.data.size() != result.received ||
        result.data.size() > kMaximumAtomicBusTransferBytes ||
        result.transmitted > kMaximumAtomicBusTransferBytes) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidStatus,
                                        "总线事务结果无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kBusResultHeaderSize + result.data.size());
    output.push_back(static_cast<std::uint8_t>(result.status));
    append_u16(output, result.transmitted);
    append_u16(output, result.received);
    output.insert(output.end(), result.data.begin(), result.data.end());
    return output;
}

BusTransferResult decode_bus_transfer_result(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kBusResultHeaderSize ||
        payload[0] > static_cast<std::uint8_t>(
                         BusTransactionStatus::LimitExceeded)) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidStatus,
                                        "总线事务结果长度或状态无效");
    }
    const auto received = read_u16(payload.data() + 3);
    if (payload.size() != kBusResultHeaderSize + received) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "总线事务接收长度与载荷不匹配");
    }
    BusTransferResult result{
        static_cast<BusTransactionStatus>(payload[0]),
        read_u16(payload.data() + 1), received,
        {payload.begin() + static_cast<std::ptrdiff_t>(kBusResultHeaderSize),
         payload.end()}};
    static_cast<void>(encode_bus_transfer_result(result));
    return result;
}

std::vector<std::uint8_t> encode_stream_contract(
    const StreamContract& contract) {
    if (contract.version != kStreamContractVersion) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidVersion,
                                        "流合同版本无效");
    }
    if (!valid_direction(static_cast<std::uint8_t>(contract.direction))) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidDirection,
                                        "流方向无效");
    }
    constexpr std::uint8_t known_transports =
        kStreamTransportCan | kStreamTransportUsb |
        kStreamTransportEthernet;
    constexpr std::uint16_t known_flags =
        kStreamFlagLossless | kStreamFlagTimestamped |
        kStreamFlagCreditRequired;
    if ((contract.transport_mask &
         static_cast<std::uint8_t>(~known_transports)) != 0 ||
        (contract.flags & static_cast<std::uint16_t>(~known_flags)) != 0) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "流合同包含未知传输或标志");
    }
    if (contract.resource_id == 0 || contract.transport_mask == 0 ||
        contract.maximum_chunk_bytes == 0 ||
        contract.maximum_chunk_bytes > kMaximumStreamChunkBytes ||
        contract.buffer_capacity_bytes < contract.maximum_chunk_bytes ||
        contract.sustained_bits_per_second > contract.peak_bits_per_second) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "流合同边界无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamContractSize);
    append_u32(output, contract.resource_id);
    append_u16(output, contract.version);
    output.push_back(static_cast<std::uint8_t>(contract.direction));
    output.push_back(contract.transport_mask);
    append_u16(output, contract.flags);
    append_u16(output, contract.maximum_chunk_bytes);
    append_u32(output, contract.buffer_capacity_bytes);
    append_u32(output, contract.sustained_bits_per_second);
    append_u32(output, contract.peak_bits_per_second);
    append_u32(output, contract.maximum_latency_us);
    append_u32(output, contract.maximum_jitter_us);
    return output;
}

StreamContract decode_stream_contract(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kStreamContractSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "流合同长度无效");
    }
    StreamContract contract{
        read_u32(payload.data()), read_u16(payload.data() + 4),
        static_cast<StreamDirection>(payload[6]), payload[7],
        read_u16(payload.data() + 8), read_u16(payload.data() + 10),
        read_u32(payload.data() + 12), read_u32(payload.data() + 16),
        read_u32(payload.data() + 20), read_u32(payload.data() + 24),
        read_u32(payload.data() + 28)};
    static_cast<void>(encode_stream_contract(contract));
    return contract;
}

std::vector<std::uint8_t> encode_stream_open_request(
    const StreamOpenRequest& request) {
    constexpr std::uint16_t known_flags =
        kStreamFlagLossless | kStreamFlagTimestamped |
        kStreamFlagCreditRequired;
    if ((request.requested_flags &
         static_cast<std::uint16_t>(~known_flags)) != 0) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "打开流请求包含未知标志");
    }
    if (request.resource_id == 0 || request.requested_chunk_bytes == 0 ||
        request.requested_chunk_bytes > kMaximumStreamChunkBytes) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "打开流请求边界无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamOpenSize);
    append_u32(output, request.resource_id);
    append_u16(output, request.requested_chunk_bytes);
    append_u16(output, request.requested_flags);
    append_u32(output, request.initial_credit_bytes);
    return output;
}

StreamOpenRequest decode_stream_open_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kStreamOpenSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "打开流请求长度无效");
    }
    StreamOpenRequest request{read_u32(payload.data()),
                              read_u16(payload.data() + 4),
                              read_u16(payload.data() + 6),
                              read_u32(payload.data() + 8)};
    static_cast<void>(encode_stream_open_request(request));
    return request;
}

std::vector<std::uint8_t> encode_stream_open_response(
    const StreamOpenResponse& response) {
    constexpr std::uint16_t known_flags =
        kStreamFlagLossless | kStreamFlagTimestamped |
        kStreamFlagCreditRequired;
    if ((response.negotiated_flags &
         static_cast<std::uint16_t>(~known_flags)) != 0) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "打开流响应包含未知标志");
    }
    if (response.stream_id == 0 || response.negotiated_chunk_bytes == 0 ||
        response.negotiated_chunk_bytes > kMaximumStreamChunkBytes) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "打开流响应边界无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamOpenSize);
    append_u32(output, response.stream_id);
    append_u16(output, response.negotiated_chunk_bytes);
    append_u16(output, response.negotiated_flags);
    append_u32(output, response.available_credit_bytes);
    return output;
}

StreamOpenResponse decode_stream_open_response(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kStreamOpenSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "打开流响应长度无效");
    }
    StreamOpenResponse response{read_u32(payload.data()),
                                read_u16(payload.data() + 4),
                                read_u16(payload.data() + 6),
                                read_u32(payload.data() + 8)};
    static_cast<void>(encode_stream_open_response(response));
    return response;
}

std::vector<std::uint8_t> encode_stream_data(const StreamDataPayload& data) {
    constexpr std::uint16_t known_flags =
        kStreamDataEndOfRecord | kStreamDataTimestampValid;
    if ((data.flags & static_cast<std::uint16_t>(~known_flags)) != 0) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidFlags,
                                        "流数据包含未知标志");
    }
    if (data.stream_id == 0 || data.data.empty() ||
        data.data.size() > kMaximumStreamChunkBytes ||
        ((data.flags & kStreamDataTimestampValid) == 0U &&
         data.timestamp_ns != 0)) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "流数据边界或时间戳标志无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamDataHeaderSize + data.data.size());
    append_u32(output, data.stream_id);
    append_u32(output, data.sequence);
    append_u16(output, data.flags);
    append_u16(output, static_cast<std::uint16_t>(data.data.size()));
    append_u64(output, data.timestamp_ns);
    output.insert(output.end(), data.data.begin(), data.data.end());
    return output;
}

StreamDataPayload decode_stream_data(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kStreamDataHeaderSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "流数据长度无效");
    }
    const auto length = read_u16(payload.data() + 10);
    if (payload.size() != kStreamDataHeaderSize + length) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "流数据声明长度与载荷不匹配");
    }
    StreamDataPayload data{
        read_u32(payload.data()), read_u32(payload.data() + 4),
        read_u16(payload.data() + 8), read_u64(payload.data() + 12),
        {payload.begin() + static_cast<std::ptrdiff_t>(kStreamDataHeaderSize),
         payload.end()}};
    static_cast<void>(encode_stream_data(data));
    return data;
}

std::vector<std::uint8_t> encode_stream_credit(
    const StreamCreditPayload& credit) {
    if (credit.stream_id == 0 || credit.credit_bytes == 0) {
        throw BusStreamPayloadException(BusStreamPayloadError::LimitExceeded,
                                        "流信用参数无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamCreditSize);
    append_u32(output, credit.stream_id);
    append_u32(output, credit.credit_bytes);
    append_u32(output, credit.acknowledged_sequence);
    return output;
}

StreamCreditPayload decode_stream_credit(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kStreamCreditSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "流信用载荷长度无效");
    }
    StreamCreditPayload credit{read_u32(payload.data()),
                               read_u32(payload.data() + 4),
                               read_u32(payload.data() + 8)};
    static_cast<void>(encode_stream_credit(credit));
    return credit;
}

std::vector<std::uint8_t> encode_stream_status(
    const StreamStatusPayload& status) {
    if (status.stream_id == 0 || status.state < StreamState::Open ||
        status.state > StreamState::Failed) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidStatus,
                                        "流状态无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStreamStatusSize);
    append_u32(output, status.stream_id);
    output.push_back(static_cast<std::uint8_t>(status.state));
    append_u32(output, status.buffered_bytes);
    append_u32(output, status.available_credit_bytes);
    append_u32(output, status.dropped_bytes);
    append_u32(output, status.next_sequence);
    return output;
}

StreamStatusPayload decode_stream_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kStreamStatusSize) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidLength,
                                        "流状态载荷长度无效");
    }
    if (payload[4] < static_cast<std::uint8_t>(StreamState::Open) ||
        payload[4] > static_cast<std::uint8_t>(StreamState::Failed)) {
        throw BusStreamPayloadException(BusStreamPayloadError::InvalidStatus,
                                        "流状态值无效");
    }
    StreamStatusPayload status{
        read_u32(payload.data()), static_cast<StreamState>(payload[4]),
        read_u32(payload.data() + 5), read_u32(payload.data() + 9),
        read_u32(payload.data() + 13), read_u32(payload.data() + 17)};
    static_cast<void>(encode_stream_status(status));
    return status;
}

}  // namespace remotebsp::protocol
