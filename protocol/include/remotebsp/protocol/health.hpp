#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kHealthContractVersion = 1U;
constexpr std::size_t kMaximumHealthMetrics = 48U;

enum class HealthSource : std::uint8_t {
    Mcu = 1U,
    RemoteCore = 2U,
    Toolbusd = 3U,
};

enum class OverallHealth : std::uint8_t {
    Unknown = 0U,
    Healthy = 1U,
    Degraded = 2U,
    Fault = 3U,
};

enum class MetricAvailability : std::uint8_t {
    Available = 1U,
    Unavailable = 2U,
    Unknown = 3U,
};

enum class HealthMetricUnit : std::uint8_t {
    Count = 1U,
    Bytes = 2U,
    Permille = 3U,
    Milliseconds = 4U,
    Generation = 5U,
};

enum class HealthMetricId : std::uint16_t {
    CpuLoadPermille = 1U,
    IsrLoadPermille = 2U,
    MinimumStackFreeBytes = 3U,
    RequestQueueDepth = 4U,
    RequestQueueCapacity = 5U,
    MotionQueueDepth = 6U,
    MotionQueueCapacity = 7U,
    StreamBufferedBytes = 8U,
    StreamBufferCapacityBytes = 9U,
    RetryTotal = 10U,
    TimeoutTotal = 11U,
    DuplicateResponseTotal = 12U,
    UnexpectedResponseTotal = 13U,
    RxFrameTotal = 14U,
    TxFrameTotal = 15U,
    DroppedFrameTotal = 16U,
    BootGeneration = 17U,
    SessionGeneration = 18U,
    ClockModelGeneration = 19U,
    UptimeMilliseconds = 20U,
    ActiveLeaseCount = 21U,
    ResourceFaultCount = 22U,
    SampleOverrunTotal = 23U,
};

struct HealthMetric {
    std::uint16_t metric_id{};
    MetricAvailability availability{MetricAvailability::Unknown};
    HealthMetricUnit unit{HealthMetricUnit::Count};
    std::uint64_t value{};
};

struct HealthSnapshot {
    std::uint16_t version{kHealthContractVersion};
    HealthSource source{HealthSource::Mcu};
    OverallHealth overall{OverallHealth::Unknown};
    std::uint64_t sample_sequence{};
    std::uint64_t sample_time_ms{};
    std::uint32_t node_id{};
    // 生产者每次重启或会话重建时必须更换的非零代际。
    // 此字段不证明来源可信；接线层仍须同时核对可信路由/会话、source 与 node_id。
    std::uint64_t producer_generation{};
    std::vector<HealthMetric> metrics;
};

enum class HealthContractError {
    TooShort,
    TooLarge,
    UnsupportedVersion,
    InvalidHeader,
    LengthMismatch,
    InvalidMetric,
    NonCanonicalMetrics,
    InconsistentMetrics,
};

class HealthContractException : public std::runtime_error {
public:
    HealthContractException(HealthContractError code, const char* message);
    HealthContractError code() const noexcept;

private:
    HealthContractError code_;
};

std::vector<std::uint8_t> encode_health_snapshot(
    const HealthSnapshot& snapshot);
HealthSnapshot decode_health_snapshot(
    const std::vector<std::uint8_t>& payload);
const HealthMetric* find_health_metric(
    const HealthSnapshot& snapshot, HealthMetricId metric_id) noexcept;

}  // namespace remotebsp::protocol
