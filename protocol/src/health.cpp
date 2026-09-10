#include "remotebsp/protocol/health.hpp"

#include <optional>

namespace remotebsp::protocol {
namespace {

constexpr std::size_t kHeaderSize = 36U;
constexpr std::size_t kMetricSize = 12U;

[[noreturn]] void fail(HealthContractError code, const char* message) {
    throw HealthContractException(code, message);
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) noexcept {
    return static_cast<std::uint16_t>(input[0]) |
           (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) noexcept {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* input) noexcept {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

std::optional<HealthMetricUnit> known_unit(std::uint16_t metric_id) {
    switch (static_cast<HealthMetricId>(metric_id)) {
        case HealthMetricId::CpuLoadPermille:
        case HealthMetricId::IsrLoadPermille:
            return HealthMetricUnit::Permille;
        case HealthMetricId::MinimumStackFreeBytes:
        case HealthMetricId::StreamBufferedBytes:
        case HealthMetricId::StreamBufferCapacityBytes:
            return HealthMetricUnit::Bytes;
        case HealthMetricId::BootGeneration:
        case HealthMetricId::SessionGeneration:
        case HealthMetricId::ClockModelGeneration:
            return HealthMetricUnit::Generation;
        case HealthMetricId::UptimeMilliseconds:
            return HealthMetricUnit::Milliseconds;
        case HealthMetricId::RequestQueueDepth:
        case HealthMetricId::RequestQueueCapacity:
        case HealthMetricId::MotionQueueDepth:
        case HealthMetricId::MotionQueueCapacity:
        case HealthMetricId::RetryTotal:
        case HealthMetricId::TimeoutTotal:
        case HealthMetricId::DuplicateResponseTotal:
        case HealthMetricId::UnexpectedResponseTotal:
        case HealthMetricId::RxFrameTotal:
        case HealthMetricId::TxFrameTotal:
        case HealthMetricId::DroppedFrameTotal:
        case HealthMetricId::ActiveLeaseCount:
        case HealthMetricId::ResourceFaultCount:
        case HealthMetricId::SampleOverrunTotal:
            return HealthMetricUnit::Count;
    }
    return std::nullopt;
}

bool valid_overall(OverallHealth health) noexcept {
    return health == OverallHealth::Unknown ||
           health == OverallHealth::Healthy ||
           health == OverallHealth::Degraded ||
           health == OverallHealth::Fault;
}

bool valid_availability(MetricAvailability availability) noexcept {
    return availability == MetricAvailability::Available ||
           availability == MetricAvailability::Unavailable ||
           availability == MetricAvailability::Unknown;
}

const HealthMetric* find_raw(const HealthSnapshot& snapshot,
                             HealthMetricId id) noexcept {
    const auto expected = static_cast<std::uint16_t>(id);
    for (const auto& metric : snapshot.metrics) {
        if (metric.metric_id == expected) return &metric;
    }
    return nullptr;
}

void validate_pair(const HealthSnapshot& snapshot, HealthMetricId depth_id,
                   HealthMetricId capacity_id) {
    const auto* depth = find_raw(snapshot, depth_id);
    const auto* capacity = find_raw(snapshot, capacity_id);
    if (capacity != nullptr &&
        capacity->availability == MetricAvailability::Available &&
        capacity->value == 0U) {
        fail(HealthContractError::InconsistentMetrics,
             "遥测队列容量不能为零");
    }
    if (depth != nullptr && capacity != nullptr &&
        depth->availability == MetricAvailability::Available &&
        capacity->availability == MetricAvailability::Available &&
        depth->value > capacity->value) {
        fail(HealthContractError::InconsistentMetrics,
             "遥测队列深度超过容量");
    }
}

void validate_snapshot(const HealthSnapshot& snapshot) {
    if (snapshot.version != kHealthContractVersion) {
        fail(HealthContractError::UnsupportedVersion,
             "遥测健康契约版本不受支持");
    }
    if (static_cast<std::uint8_t>(snapshot.source) == 0U ||
        !valid_overall(snapshot.overall) || snapshot.sample_sequence == 0U ||
        snapshot.node_id > 127U || snapshot.producer_generation == 0U) {
        fail(HealthContractError::InvalidHeader, "遥测健康头字段无效");
    }
    if (snapshot.metrics.empty() ||
        snapshot.metrics.size() > kMaximumHealthMetrics) {
        fail(HealthContractError::TooLarge, "遥测指标数量无效");
    }

    std::uint16_t previous_id = 0U;
    for (const auto& metric : snapshot.metrics) {
        if (metric.metric_id == 0U || metric.metric_id <= previous_id) {
            fail(HealthContractError::NonCanonicalMetrics,
                 "遥测指标必须按 ID 严格递增且不得重复");
        }
        previous_id = metric.metric_id;
        if (!valid_availability(metric.availability) ||
            static_cast<std::uint8_t>(metric.unit) == 0U ||
            (metric.availability != MetricAvailability::Available &&
             metric.value != 0U)) {
            fail(HealthContractError::InvalidMetric,
                 "遥测指标状态、单位或值无效");
        }
        const auto unit = known_unit(metric.metric_id);
        if (unit.has_value() && metric.unit != *unit) {
            fail(HealthContractError::InvalidMetric,
                 "标准遥测指标单位不匹配");
        }
        if (metric.availability == MetricAvailability::Available &&
            (metric.metric_id == static_cast<std::uint16_t>(
                                     HealthMetricId::CpuLoadPermille) ||
             metric.metric_id == static_cast<std::uint16_t>(
                                     HealthMetricId::IsrLoadPermille)) &&
            metric.value > 1000U) {
            fail(HealthContractError::InvalidMetric,
                 "CPU 或 ISR 负载超出千分比范围");
        }
        if (metric.availability == MetricAvailability::Available &&
            (metric.metric_id == static_cast<std::uint16_t>(
                                     HealthMetricId::BootGeneration) ||
             metric.metric_id == static_cast<std::uint16_t>(
                                     HealthMetricId::SessionGeneration) ||
             metric.metric_id == static_cast<std::uint16_t>(
                                     HealthMetricId::ClockModelGeneration)) &&
            metric.value == 0U) {
            fail(HealthContractError::InvalidMetric,
                 "可用的代际指标必须大于零");
        }
    }
    validate_pair(snapshot, HealthMetricId::RequestQueueDepth,
                  HealthMetricId::RequestQueueCapacity);
    validate_pair(snapshot, HealthMetricId::MotionQueueDepth,
                  HealthMetricId::MotionQueueCapacity);
    validate_pair(snapshot, HealthMetricId::StreamBufferedBytes,
                  HealthMetricId::StreamBufferCapacityBytes);
}

}  // namespace

HealthContractException::HealthContractException(HealthContractError code,
                                                 const char* message)
    : std::runtime_error(message), code_(code) {}

HealthContractError HealthContractException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_health_snapshot(
    const HealthSnapshot& snapshot) {
    validate_snapshot(snapshot);
    std::vector<std::uint8_t> output;
    output.reserve(kHeaderSize + snapshot.metrics.size() * kMetricSize);
    append_u16(output, snapshot.version);
    output.push_back(static_cast<std::uint8_t>(snapshot.source));
    output.push_back(static_cast<std::uint8_t>(snapshot.overall));
    append_u64(output, snapshot.sample_sequence);
    append_u64(output, snapshot.sample_time_ms);
    append_u32(output, snapshot.node_id);
    append_u64(output, snapshot.producer_generation);
    append_u16(output, static_cast<std::uint16_t>(snapshot.metrics.size()));
    append_u16(output, 0U);
    for (const auto& metric : snapshot.metrics) {
        append_u16(output, metric.metric_id);
        output.push_back(static_cast<std::uint8_t>(metric.availability));
        output.push_back(static_cast<std::uint8_t>(metric.unit));
        append_u64(output, metric.value);
    }
    return output;
}

HealthSnapshot decode_health_snapshot(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kHeaderSize) {
        fail(HealthContractError::TooShort, "遥测健康载荷过短");
    }
    const auto count = read_u16(payload.data() + 32U);
    if (count == 0U || count > kMaximumHealthMetrics) {
        fail(HealthContractError::TooLarge, "遥测指标数量无效");
    }
    const auto expected_size = kHeaderSize + count * kMetricSize;
    if (payload.size() != expected_size) {
        fail(HealthContractError::LengthMismatch, "遥测健康载荷长度不匹配");
    }
    if (read_u16(payload.data() + 34U) != 0U) {
        fail(HealthContractError::InvalidHeader, "遥测健康保留位必须为零");
    }

    HealthSnapshot snapshot;
    snapshot.version = read_u16(payload.data());
    snapshot.source = static_cast<HealthSource>(payload[2U]);
    snapshot.overall = static_cast<OverallHealth>(payload[3U]);
    snapshot.sample_sequence = read_u64(payload.data() + 4U);
    snapshot.sample_time_ms = read_u64(payload.data() + 12U);
    snapshot.node_id = read_u32(payload.data() + 20U);
    snapshot.producer_generation = read_u64(payload.data() + 24U);
    snapshot.metrics.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto* input = payload.data() + kHeaderSize + index * kMetricSize;
        snapshot.metrics.push_back({
            read_u16(input), static_cast<MetricAvailability>(input[2U]),
            static_cast<HealthMetricUnit>(input[3U]), read_u64(input + 4U)});
    }
    validate_snapshot(snapshot);
    return snapshot;
}

const HealthMetric* find_health_metric(
    const HealthSnapshot& snapshot, HealthMetricId metric_id) noexcept {
    return find_raw(snapshot, metric_id);
}

}  // namespace remotebsp::protocol
