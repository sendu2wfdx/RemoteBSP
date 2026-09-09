#include "remotebsp/protocol/time_sync.hpp"

#include <limits>

namespace remotebsp::protocol {
namespace {

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(input[index])
                 << (index * 8U);
    }
    return value;
}

void validate_version_and_flags(std::uint8_t version,
                                std::uint16_t flags) {
    if (version != kTimeSyncPayloadVersion) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::UnsupportedVersion,
            "时钟同步载荷版本不受支持");
    }
    if (flags != 0U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::UnsupportedFlags,
            "时钟同步载荷包含未知标志");
    }
}

void validate_response(const TimeSyncResponsePayload& payload) {
    validate_version_and_flags(payload.version, payload.flags);
    if (payload.boot_epoch == 0U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidBootEpoch,
            "启动代次不能为零");
    }
    if (payload.counter_bits < 2U || payload.counter_bits > 64U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidCounterBits,
            "节点计数器位宽必须位于 2～64");
    }
    if (payload.nominal_tick_rate_hz == 0U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidTickRate,
            "节点计数器频率必须大于零");
    }

    if (payload.counter_bits == 64U) {
        if (payload.node_send_tick < payload.node_receive_tick) {
            throw TimeSyncPayloadException(
                TimeSyncPayloadError::InvalidTickOrder,
                "64 位节点发送时间早于接收时间");
        }
        return;
    }

    const auto modulus = 1ULL << payload.counter_bits;
    const auto mask = modulus - 1U;
    if (payload.node_receive_tick > mask ||
        payload.node_send_tick > mask) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::CounterOutOfRange,
            "节点时间戳超出声明的计数器位宽");
    }
    const auto turnaround =
        (payload.node_send_tick - payload.node_receive_tick) & mask;
    if (turnaround >= modulus / 2U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidTickOrder,
            "节点响应处理时间达到计数器半周期");
    }
}

}

TimeSyncPayloadException::TimeSyncPayloadException(
    TimeSyncPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

TimeSyncPayloadError TimeSyncPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_time_sync_request(
    const TimeSyncRequestPayload& payload) {
    validate_version_and_flags(payload.version, payload.flags);
    return {payload.version, payload.flags, 0U, 0U};
}

TimeSyncRequestPayload decode_time_sync_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kTimeSyncRequestPayloadSize) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidLength,
            "时钟同步请求载荷长度无效");
    }
    if (read_u16(payload.data() + 2U) != 0U) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::UnsupportedFlags,
            "时钟同步请求保留字段必须为零");
    }
    const TimeSyncRequestPayload decoded{payload[0], payload[1]};
    validate_version_and_flags(decoded.version, decoded.flags);
    return decoded;
}

std::vector<std::uint8_t> encode_time_sync_response(
    const TimeSyncResponsePayload& payload) {
    validate_response(payload);
    std::vector<std::uint8_t> output;
    output.reserve(kTimeSyncResponsePayloadSize);
    output.push_back(payload.version);
    output.push_back(payload.counter_bits);
    append_u16(output, payload.flags);
    append_u64(output, payload.boot_epoch);
    append_u64(output, payload.nominal_tick_rate_hz);
    append_u64(output, payload.node_receive_tick);
    append_u64(output, payload.node_send_tick);
    return output;
}

TimeSyncResponsePayload decode_time_sync_response(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kTimeSyncResponsePayloadSize) {
        throw TimeSyncPayloadException(
            TimeSyncPayloadError::InvalidLength,
            "时钟同步响应载荷长度无效");
    }
    TimeSyncResponsePayload decoded;
    decoded.version = payload[0];
    decoded.counter_bits = payload[1];
    decoded.flags = read_u16(payload.data() + 2U);
    decoded.boot_epoch = read_u64(payload.data() + 4U);
    decoded.nominal_tick_rate_hz = read_u64(payload.data() + 12U);
    decoded.node_receive_tick = read_u64(payload.data() + 20U);
    decoded.node_send_tick = read_u64(payload.data() + 28U);
    validate_response(decoded);
    return decoded;
}

}
