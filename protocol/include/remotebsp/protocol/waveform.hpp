#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kPwmDutyScale = 10000;

struct PwmCreatePayload {
    std::uint8_t channel{};
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
};

struct TimedBitstreamCreatePayload {
    std::uint8_t channel{};
    std::uint32_t bit_period_ns{};
    std::uint32_t zero_high_ns{};
    std::uint32_t one_high_ns{};
    std::uint32_t reset_time_us{};
};

struct TimedBitstreamWritePayload {
    std::uint16_t bit_count{};
    std::vector<std::uint8_t> data;
};

enum class WaveformPayloadError {
    InvalidLength,
    InvalidValue,
};

class WaveformPayloadException : public std::runtime_error {
public:
    WaveformPayloadException(WaveformPayloadError code,
                             const char* message);
    WaveformPayloadError code() const noexcept;

private:
    WaveformPayloadError code_;
};

std::vector<std::uint8_t> encode_pwm_create(
    const PwmCreatePayload& payload);
PwmCreatePayload decode_pwm_create(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_pwm_duty(std::uint16_t duty);
std::uint16_t decode_pwm_duty(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_timed_bitstream_create(
    const TimedBitstreamCreatePayload& payload);
TimedBitstreamCreatePayload decode_timed_bitstream_create(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_timed_bitstream_write(
    const TimedBitstreamWritePayload& payload);
TimedBitstreamWritePayload decode_timed_bitstream_write(
    const std::vector<std::uint8_t>& payload);

}
