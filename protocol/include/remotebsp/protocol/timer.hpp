#pragma once
#include <cstdint>
#include <vector>

namespace remotebsp::protocol {
constexpr std::uint16_t kTimerProtocolVersion = 1;
constexpr std::uint32_t kMaximumTimerOperationUs = 1000000U;
constexpr std::uint16_t kTimerCapabilityCounterWindow = 1U << 0U;
constexpr std::uint16_t kTimerCapabilityPeriodCapture = 1U << 1U;
constexpr std::uint16_t kTimerCapabilityOneShot = 1U << 2U;

enum class TimerOperation : std::uint16_t { CounterWindow = 1, PeriodCapture = 2, OneShot = 3 };

struct TimerContract {
    std::uint16_t version{kTimerProtocolVersion};
    std::uint16_t capabilities{};
    std::uint32_t resource_id{};
    std::uint32_t tick_hz{};
    std::uint32_t maximum_operation_us{kMaximumTimerOperationUs};
};
struct TimerExecuteRequest {
    std::uint32_t resource_id{};
    TimerOperation operation{TimerOperation::CounterWindow};
    std::uint32_t parameter_us{}; // 计数窗口、捕获超时或单次等待时长
    std::uint32_t timeout_us{};
};
struct TimerExecuteResult {
    std::uint32_t resource_id{};
    TimerOperation operation{TimerOperation::CounterWindow};
    std::uint32_t sequence{};
    std::uint64_t value{}; // 计数值、周期tick或实际单次等待tick
    std::uint32_t elapsed_us{};
};
std::vector<std::uint8_t> encode_timer_contract(const TimerContract&);
TimerContract decode_timer_contract(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_timer_execute_request(const TimerExecuteRequest&);
TimerExecuteRequest decode_timer_execute_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_timer_execute_result(const TimerExecuteResult&);
TimerExecuteResult decode_timer_execute_result(const std::vector<std::uint8_t>&);
}
