#include "remotebsp/mock_mcu/time_sync_bsp.hpp"

#include <atomic>
#include <limits>
#include <stdexcept>

namespace remotebsp::mock_mcu {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kMaximumMockTickRateHz = 1000000000ULL;

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("Mock 节点自由运行计数器计算溢出");
    }
    return left + right;
}

}

MockTimeSyncBsp::MockTimeSyncBsp(MockTimeSyncConfig config,
                                 TimePoint origin)
    : config_(config), origin_(origin) {
    if (config_.boot_epoch == 0U) {
        config_.boot_epoch = generate_boot_epoch();
    }
    if (config_.nominal_tick_rate_hz == 0U ||
        config_.nominal_tick_rate_hz > kMaximumMockTickRateHz) {
        throw std::invalid_argument(
            "Mock 节点 tick 频率必须位于 1～1000000000 Hz");
    }
    if (config_.counter_bits < 2U || config_.counter_bits > 64U) {
        throw std::invalid_argument("Mock 节点计数器位宽必须位于 2～64");
    }
    if (config_.counter_bits < 64U) {
        const auto modulus = 1ULL << config_.counter_bits;
        if (config_.initial_tick >= modulus) {
            throw std::invalid_argument(
                "Mock 节点初始 tick 超出计数器位宽");
        }
        if (config_.response_turnaround_ticks >= modulus / 2U) {
            throw std::invalid_argument(
                "Mock 节点响应处理时间必须小于计数器半周期");
        }
    }
}

TimeSyncCapture MockTimeSyncBsp::capture(
    TimePoint request_received_at) const {
    if (request_received_at < origin_) {
        throw std::invalid_argument("Mock 时间戳早于节点启动时刻");
    }
    const auto elapsed_signed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            request_received_at - origin_)
            .count();
    const auto elapsed_ns = static_cast<std::uint64_t>(elapsed_signed);
    const auto whole_seconds = elapsed_ns / kNanosecondsPerSecond;
    const auto remaining_ns = elapsed_ns % kNanosecondsPerSecond;
    if (whole_seconds >
        std::numeric_limits<std::uint64_t>::max() /
            config_.nominal_tick_rate_hz) {
        throw std::overflow_error("Mock 节点运行时间无法换算为 tick");
    }
    const auto whole_ticks =
        whole_seconds * config_.nominal_tick_rate_hz;
    const auto fractional_ticks =
        remaining_ns * config_.nominal_tick_rate_hz /
        kNanosecondsPerSecond;
    const auto elapsed_ticks = checked_add(whole_ticks, fractional_ticks);
    const auto unwrapped_receive =
        checked_add(config_.initial_tick, elapsed_ticks);

    std::uint64_t receive_tick = unwrapped_receive;
    std::uint64_t send_tick = 0U;
    if (config_.counter_bits == 64U) {
        send_tick = checked_add(
            receive_tick, config_.response_turnaround_ticks);
    } else {
        const auto mask = (1ULL << config_.counter_bits) - 1U;
        receive_tick &= mask;
        send_tick =
            (receive_tick + config_.response_turnaround_ticks) & mask;
    }
    return {config_.boot_epoch, config_.nominal_tick_rate_hz,
            config_.counter_bits, receive_tick, send_tick};
}

std::uint64_t MockTimeSyncBsp::boot_epoch() const noexcept {
    return config_.boot_epoch;
}

TimeSyncBsp::TimePoint MockTimeSyncBsp::origin() const noexcept {
    return origin_;
}

std::uint64_t MockTimeSyncBsp::generate_boot_epoch() noexcept {
    static std::atomic<std::uint64_t> sequence{1U};
    const auto count = Clock::now().time_since_epoch().count();
    auto value = static_cast<std::uint64_t>(count) ^
                 (sequence.fetch_add(1U, std::memory_order_relaxed) *
                  0x9E3779B97F4A7C15ULL);
    value ^= value >> 30U;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27U;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return value == 0U ? 1U : value;
}

}
