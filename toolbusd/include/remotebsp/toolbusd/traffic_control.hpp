#pragma once

#include "remotebsp/protocol/packet.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::toolbusd {

enum class TrafficBusMode : std::uint8_t {
    Classical = 0,
    CanFd = 1,
    Usb = 2,
};

enum class TrafficClass : std::uint8_t {
    Safety = 0,
    Motion = 1,
    System = 2,
    Interactive = 3,
    Streaming = 4,
    Bulk = 5,
    Count = 6,
};

constexpr std::size_t kTrafficClassCount =
    static_cast<std::size_t>(TrafficClass::Count);

struct TrafficConfig {
    TrafficBusMode mode{TrafficBusMode::Classical};
    std::uint32_t arbitration_bits_per_second{1000000};
    std::uint32_t data_bits_per_second{1000000};
    std::uint16_t maximum_utilization_permille{700};
    std::chrono::milliseconds burst_window{250};
    std::array<std::uint16_t, kTrafficClassCount>
        class_limit_permille{{1000, 500, 250, 500, 200, 100}};
};

struct CanFrameCost {
    std::uint16_t nominal_phase_bits{};
    std::uint16_t data_phase_bits{};
    std::uint64_t wire_time_ns{};
    std::uint8_t wire_payload_length{};
};

enum class TrafficError {
    InvalidConfiguration,
    InvalidFrameLength,
};

class TrafficException : public std::runtime_error {
public:
    TrafficException(TrafficError code, const char* message);
    TrafficError code() const noexcept;

private:
    TrafficError code_;
};

CanFrameCost estimate_can_frame_cost(
    TrafficBusMode mode, std::size_t payload_length,
    std::uint32_t arbitration_bits_per_second,
    std::uint32_t data_bits_per_second,
    bool bit_rate_switch = true);

TrafficClass classify_traffic(const protocol::Packet& packet) noexcept;
const char* traffic_class_name(TrafficClass traffic_class) noexcept;

struct TrafficClassCounters {
    std::uint64_t admitted_packets{};
    std::uint64_t rejected_packets{};
    std::uint64_t admitted_frames{};
    std::uint64_t estimated_wire_time_ns{};
};

struct TrafficSnapshot {
    static constexpr std::uint16_t kVersion = 1;

    std::uint16_t version{kVersion};
    TrafficBusMode mode{TrafficBusMode::Classical};
    std::uint32_t arbitration_bits_per_second{};
    std::uint32_t data_bits_per_second{};
    std::uint16_t maximum_utilization_permille{};
    std::uint32_t burst_window_ms{};
    std::uint64_t global_capacity_ns{};
    std::uint64_t global_available_ns{};
    std::uint64_t admitted_packets{};
    std::uint64_t rejected_packets{};
    std::uint64_t guaranteed_overruns{};
    std::uint64_t admitted_frames{};
    std::uint64_t estimated_wire_time_ns{};
    std::array<TrafficClassCounters, kTrafficClassCount> classes{};
};

enum class AdmissionPolicy {
    Enforce,
    Guaranteed,
};

class TrafficController {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit TrafficController(TrafficConfig config,
                               TimePoint now = Clock::now());

    bool admit(TrafficClass traffic_class,
               const std::vector<std::size_t>& frame_payload_lengths,
               AdmissionPolicy policy = AdmissionPolicy::Enforce,
               TimePoint now = Clock::now());
    TrafficSnapshot snapshot(TimePoint now = Clock::now());
    const TrafficConfig& config() const noexcept;

private:
    struct Bucket {
        long double available_ns{};
        long double capacity_ns{};
        std::uint16_t refill_permille{};
    };

    void refill(TimePoint now);

    TrafficConfig config_;
    TimePoint last_refill_;
    Bucket global_;
    std::array<Bucket, kTrafficClassCount> classes_;
    TrafficSnapshot counters_;
};

}
