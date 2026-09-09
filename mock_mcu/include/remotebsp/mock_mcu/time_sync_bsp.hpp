#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace remotebsp::mock_mcu {

struct TimeSyncCapture {
    std::uint64_t boot_epoch{};
    std::uint64_t nominal_tick_rate_hz{};
    std::uint8_t counter_bits{64U};
    std::uint64_t node_receive_tick{};
    std::uint64_t node_send_tick{};
};

class TimeSyncBsp {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    virtual ~TimeSyncBsp() = default;
    virtual TimeSyncCapture capture(TimePoint request_received_at) const = 0;
};

struct MockTimeSyncConfig {
    /* 零表示由 Mock 在构造时生成本进程内唯一的启动代次。 */
    std::uint64_t boot_epoch{};
    std::uint64_t nominal_tick_rate_hz{1000000ULL};
    std::uint8_t counter_bits{64U};
    std::uint64_t initial_tick{};
    std::uint64_t response_turnaround_ticks{20U};
};

class MockTimeSyncBsp final : public TimeSyncBsp {
public:
    explicit MockTimeSyncBsp(
        MockTimeSyncConfig config = {},
        TimePoint origin = Clock::now());

    TimeSyncCapture capture(
        TimePoint request_received_at) const override;
    std::uint64_t boot_epoch() const noexcept;
    TimePoint origin() const noexcept;
    std::uint64_t elapsed_ns(TimePoint now) const;
    std::optional<std::uint64_t> motion_time_ns_for_tick(
        std::uint64_t extended_tick, TimePoint now) const;

private:
    static std::uint64_t generate_boot_epoch() noexcept;

    MockTimeSyncConfig config_;
    TimePoint origin_;
};

}
