#include "remotebsp/protocol/waveform.hpp"

#include <cstddef>

namespace remotebsp::protocol {
namespace {

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

void validate_pwm(const PwmCreatePayload& payload) {
    if (payload.frequency_hz == 0 || payload.duty > kPwmDutyScale) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "PWM 参数超出范围");
    }
}

void validate_bitstream(const TimedBitstreamCreatePayload& payload) {
    if (payload.bit_period_ns == 0 || payload.zero_high_ns == 0 ||
        payload.one_high_ns == 0 ||
        payload.zero_high_ns >= payload.bit_period_ns ||
        payload.one_high_ns >= payload.bit_period_ns ||
        payload.reset_time_us == 0) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "定时位流参数超出范围");
    }
}

}

WaveformPayloadException::WaveformPayloadException(
    WaveformPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

WaveformPayloadError WaveformPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_pwm_create(
    const PwmCreatePayload& payload) {
    validate_pwm(payload);
    std::vector<std::uint8_t> output{payload.channel};
    append_u32(output, payload.frequency_hz);
    append_u16(output, payload.duty);
    output.push_back(payload.active_low ? 1U : 0U);
    return output;
}

PwmCreatePayload decode_pwm_create(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidLength, "PWM_CREATE 载荷长度无效");
    }
    const PwmCreatePayload decoded{
        payload[0], read_u32(payload.data() + 1),
        read_u16(payload.data() + 5), payload[7] != 0};
    if (payload[7] > 1U) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "PWM 有效极性无效");
    }
    validate_pwm(decoded);
    return decoded;
}

std::vector<std::uint8_t> encode_pwm_duty(std::uint16_t duty) {
    if (duty > kPwmDutyScale) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "PWM 占空比超出范围");
    }
    std::vector<std::uint8_t> output;
    append_u16(output, duty);
    return output;
}

std::uint16_t decode_pwm_duty(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 2) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidLength, "PWM_WRITE 载荷长度无效");
    }
    const auto duty = read_u16(payload.data());
    if (duty > kPwmDutyScale) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "PWM 占空比超出范围");
    }
    return duty;
}

std::vector<std::uint8_t> encode_timed_bitstream_create(
    const TimedBitstreamCreatePayload& payload) {
    validate_bitstream(payload);
    std::vector<std::uint8_t> output{payload.channel};
    append_u32(output, payload.bit_period_ns);
    append_u32(output, payload.zero_high_ns);
    append_u32(output, payload.one_high_ns);
    append_u32(output, payload.reset_time_us);
    return output;
}

TimedBitstreamCreatePayload decode_timed_bitstream_create(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 17) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidLength,
            "TIMED_BITSTREAM_CREATE 载荷长度无效");
    }
    const TimedBitstreamCreatePayload decoded{
        payload[0], read_u32(payload.data() + 1),
        read_u32(payload.data() + 5), read_u32(payload.data() + 9),
        read_u32(payload.data() + 13)};
    validate_bitstream(decoded);
    return decoded;
}

std::vector<std::uint8_t> encode_timed_bitstream_write(
    const TimedBitstreamWritePayload& payload) {
    const std::size_t expected = (payload.bit_count + 7U) / 8U;
    if (payload.bit_count == 0 || payload.data.size() != expected) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidValue, "定时位流数据长度无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(payload.data.size() + 2U);
    append_u16(output, payload.bit_count);
    output.insert(output.end(), payload.data.begin(), payload.data.end());
    return output;
}

TimedBitstreamWritePayload decode_timed_bitstream_write(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 3) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidLength,
            "TIMED_BITSTREAM_WRITE 载荷过短");
    }
    const auto bit_count = read_u16(payload.data());
    if (bit_count == 0 || payload.size() != 2U + (bit_count + 7U) / 8U) {
        throw WaveformPayloadException(
            WaveformPayloadError::InvalidLength,
            "TIMED_BITSTREAM_WRITE 数据长度不匹配");
    }
    return {bit_count,
            std::vector<std::uint8_t>(payload.begin() + 2, payload.end())};
}

}
