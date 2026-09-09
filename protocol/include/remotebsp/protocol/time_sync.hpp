#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint8_t kTimeSyncPayloadVersion = 1U;
constexpr std::size_t kTimeSyncRequestPayloadSize = 4U;
constexpr std::size_t kTimeSyncResponsePayloadSize = 36U;

struct TimeSyncRequestPayload {
    std::uint8_t version{kTimeSyncPayloadVersion};
    std::uint8_t flags{};
};

struct TimeSyncResponsePayload {
    std::uint8_t version{kTimeSyncPayloadVersion};
    std::uint8_t counter_bits{64U};
    std::uint16_t flags{};
    std::uint64_t boot_epoch{};
    std::uint64_t nominal_tick_rate_hz{};
    std::uint64_t node_receive_tick{};
    std::uint64_t node_send_tick{};
};

enum class TimeSyncPayloadError : std::uint8_t {
    InvalidLength = 0,
    UnsupportedVersion,
    UnsupportedFlags,
    InvalidBootEpoch,
    InvalidCounterBits,
    InvalidTickRate,
    CounterOutOfRange,
    InvalidTickOrder,
};

class TimeSyncPayloadException : public std::runtime_error {
public:
    TimeSyncPayloadException(TimeSyncPayloadError code,
                             const char* message);
    TimeSyncPayloadError code() const noexcept;

private:
    TimeSyncPayloadError code_;
};

std::vector<std::uint8_t> encode_time_sync_request(
    const TimeSyncRequestPayload& payload = {});
TimeSyncRequestPayload decode_time_sync_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_time_sync_response(
    const TimeSyncResponsePayload& payload);
TimeSyncResponsePayload decode_time_sync_response(
    const std::vector<std::uint8_t>& payload);

}
