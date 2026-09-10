#pragma once

#include "remotebsp/protocol/health.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"
#include "remotebsp/toolbusd/bus_runtime.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace remotebsp::toolbusd {

// 0x8000～0xffff 由本地生产者扩展使用；名称明确区分“准入”与物理发送。
enum class ToolbusdHealthMetricId : std::uint16_t {
    TrafficAdmittedPacketTotal = 0x8001U,
    TrafficRejectedPacketTotal = 0x8002U,
    TrafficGuaranteedOverrunTotal = 0x8003U,
    TrafficAdmittedFrameTotal = 0x8004U,
    TrafficEstimatedWireTimeNs = 0x8005U,
    RuntimeOperationLedgerMutationAvailable = 0x8006U,
    RuntimeOperationLedgerOperationCount = 0x8007U,
    BusAdmittedTransactionTotal = 0x8008U,
    BusRateLimitedTransactionTotal = 0x8009U,
    BusBusyTransactionTotal = 0x800AU,
    BusContractRejectedTransactionTotal = 0x800BU,
};

// v1 公共单位暂不含纳秒；扩展单位仍按协议的未知枚举前向兼容规则传递。
constexpr auto kToolbusdNanosecondsUnit =
    static_cast<protocol::HealthMetricUnit>(0x80U);

struct ToolbusdHealthObservation {
    std::uint64_t sample_time_ms{};
    std::uint64_t request_queue_depth{};
    TrafficSnapshot traffic;
    std::optional<std::uint64_t> active_lease_count;
    std::optional<std::uint64_t> resource_fault_count;
    std::optional<bool> operation_ledger_mutation_available;
    std::optional<std::uint64_t> operation_ledger_operation_count;
    std::optional<BusTelemetrySnapshot> bus_telemetry;
};

enum class ToolbusdHealthProducerError {
    InvalidGeneration,
    ClockRegression,
    InvalidTrafficSnapshot,
    InvalidBusTelemetrySnapshot,
    SequenceExhausted,
};

class ToolbusdHealthProducerException : public std::runtime_error {
public:
    ToolbusdHealthProducerException(ToolbusdHealthProducerError code,
                                    const char* message);
    ToolbusdHealthProducerError code() const noexcept;

private:
    ToolbusdHealthProducerError code_;
};

// 该生产者只汇总 toolbusd 自身可证明的软件状态。它不采集、推断或冒充
// MCU 的 CPU、ISR、栈、物理收发帧等指标。
class ToolbusdHealthProducer {
public:
    ToolbusdHealthProducer(std::uint64_t producer_generation,
                           std::uint64_t started_at_ms);

    protocol::HealthSnapshot capture(
        const ToolbusdHealthObservation& observation);

    std::uint64_t producer_generation() const noexcept;

private:
    const std::uint64_t producer_generation_;
    const std::uint64_t started_at_ms_;
    mutable std::mutex mutex_;
    std::uint64_t last_sample_time_ms_{};
    std::uint64_t next_sequence_{1U};
};

}  // namespace remotebsp::toolbusd
