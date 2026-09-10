#include "remotebsp/toolbusd/health_producer.hpp"

#include <limits>

namespace remotebsp::toolbusd {
namespace {

using protocol::HealthMetric;
using protocol::HealthMetricId;
using protocol::HealthMetricUnit;
using protocol::MetricAvailability;

HealthMetric unavailable(HealthMetricId id, HealthMetricUnit unit) {
    return {static_cast<std::uint16_t>(id), MetricAvailability::Unavailable,
            unit, 0U};
}

HealthMetric unknown(HealthMetricId id, HealthMetricUnit unit) {
    return {static_cast<std::uint16_t>(id), MetricAvailability::Unknown,
            unit, 0U};
}

HealthMetric available(HealthMetricId id, HealthMetricUnit unit,
                       std::uint64_t value) {
    return {static_cast<std::uint16_t>(id), MetricAvailability::Available,
            unit, value};
}

HealthMetric extension(ToolbusdHealthMetricId id, HealthMetricUnit unit,
                       std::uint64_t value) {
    return {static_cast<std::uint16_t>(id), MetricAvailability::Available,
            unit, value};
}

bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

bool valid_traffic_snapshot(const TrafficSnapshot& snapshot) noexcept {
    if (snapshot.version != TrafficSnapshot::kVersion ||
        snapshot.mode > TrafficBusMode::Usb ||
        snapshot.arbitration_bits_per_second == 0U ||
        snapshot.data_bits_per_second == 0U ||
        snapshot.maximum_utilization_permille == 0U ||
        snapshot.maximum_utilization_permille > 1000U ||
        snapshot.burst_window_ms == 0U ||
        snapshot.global_available_ns > snapshot.global_capacity_ns) {
        return false;
    }
    std::uint64_t admitted_packets = 0U;
    std::uint64_t rejected_packets = 0U;
    std::uint64_t admitted_frames = 0U;
    std::uint64_t estimated_wire_time_ns = 0U;
    for (const auto& item : snapshot.classes) {
        if (!checked_add(admitted_packets, item.admitted_packets) ||
            !checked_add(rejected_packets, item.rejected_packets) ||
            !checked_add(admitted_frames, item.admitted_frames) ||
            !checked_add(estimated_wire_time_ns,
                         item.estimated_wire_time_ns)) {
            return false;
        }
    }
    return admitted_packets == snapshot.admitted_packets &&
           rejected_packets == snapshot.rejected_packets &&
           admitted_frames == snapshot.admitted_frames &&
           estimated_wire_time_ns == snapshot.estimated_wire_time_ns;
}

bool valid_bus_telemetry(const BusTelemetrySnapshot& snapshot) noexcept {
    if (snapshot.version != BusTelemetrySnapshot::kVersion) return false;
    std::uint64_t admitted = 0U, limited = 0U, busy = 0U, rejected = 0U;
    std::uint64_t remote_ok = 0U, remote_nack = 0U, remote_timeout = 0U;
    std::uint64_t remote_busy = 0U, remote_fault = 0U, remote_limit = 0U;
    std::uint64_t previous = 0U;
    bool first = true;
    for (const auto& item : snapshot.resources) {
        const auto key = (static_cast<std::uint64_t>(item.node_id) << 32U) |
                         item.resource_id;
        if (item.node_id == 0U || item.resource_id == 0U ||
            (!first && key <= previous) ||
            !checked_add(admitted, item.admitted_total) ||
            !checked_add(limited, item.rate_limited_total) ||
            !checked_add(busy, item.busy_total) ||
            !checked_add(rejected, item.contract_rejected_total) ||
            !checked_add(remote_ok, item.remote_ok_total) ||
            !checked_add(remote_nack, item.remote_nack_total) ||
            !checked_add(remote_timeout, item.remote_timeout_total) ||
            !checked_add(remote_busy, item.remote_busy_total) ||
            !checked_add(remote_fault, item.remote_fault_total) ||
            !checked_add(remote_limit,
                         item.remote_limit_exceeded_total)) return false;
        first = false; previous = key;
    }
    return admitted == snapshot.admitted_total &&
           limited == snapshot.rate_limited_total && busy == snapshot.busy_total &&
           rejected == snapshot.contract_rejected_total &&
           remote_ok == snapshot.remote_ok_total &&
           remote_nack == snapshot.remote_nack_total &&
           remote_timeout == snapshot.remote_timeout_total &&
           remote_busy == snapshot.remote_busy_total &&
           remote_fault == snapshot.remote_fault_total &&
           remote_limit == snapshot.remote_limit_exceeded_total;
}

}  // namespace

ToolbusdHealthProducerException::ToolbusdHealthProducerException(
    ToolbusdHealthProducerError code, const char* message)
    : std::runtime_error(message), code_(code) {}

ToolbusdHealthProducerError ToolbusdHealthProducerException::code()
    const noexcept {
    return code_;
}

ToolbusdHealthProducer::ToolbusdHealthProducer(
    std::uint64_t producer_generation, std::uint64_t started_at_ms)
    : producer_generation_(producer_generation),
      started_at_ms_(started_at_ms),
      last_sample_time_ms_(started_at_ms) {
    if (producer_generation == 0U) {
        throw ToolbusdHealthProducerException(
            ToolbusdHealthProducerError::InvalidGeneration,
            "toolbusd 健康生产者代际不能为零");
    }
}

protocol::HealthSnapshot ToolbusdHealthProducer::capture(
    const ToolbusdHealthObservation& observation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (observation.sample_time_ms < started_at_ms_ ||
        observation.sample_time_ms < last_sample_time_ms_) {
        throw ToolbusdHealthProducerException(
            ToolbusdHealthProducerError::ClockRegression,
            "toolbusd 健康采样时钟发生回退");
    }
    if (!valid_traffic_snapshot(observation.traffic)) {
        throw ToolbusdHealthProducerException(
            ToolbusdHealthProducerError::InvalidTrafficSnapshot,
            "toolbusd 流量快照不满足内部一致性约束");
    }
    if (observation.bus_telemetry.has_value() &&
        !valid_bus_telemetry(*observation.bus_telemetry)) {
        throw ToolbusdHealthProducerException(
            ToolbusdHealthProducerError::InvalidBusTelemetrySnapshot,
            "总线遥测快照不满足版本、排序或汇总一致性约束");
    }
    if (next_sequence_ == 0U) {
        throw ToolbusdHealthProducerException(
            ToolbusdHealthProducerError::SequenceExhausted,
            "toolbusd 健康采样序号已耗尽");
    }

    protocol::HealthSnapshot snapshot;
    snapshot.source = protocol::HealthSource::Toolbusd;
    // 账本不可写会关闭 Runtime mutation capability，属于可证明的软件降级；
    // 其余仅有累计计数时仍不臆测系统 Healthy 或 Fault。
    snapshot.overall =
        observation.operation_ledger_mutation_available == false
            ? protocol::OverallHealth::Degraded
            : protocol::OverallHealth::Unknown;
    snapshot.sample_sequence = next_sequence_++;
    snapshot.sample_time_ms = observation.sample_time_ms - started_at_ms_;
    snapshot.node_id = 0U;
    snapshot.producer_generation = producer_generation_;
    snapshot.metrics = {
        unavailable(HealthMetricId::CpuLoadPermille,
                    HealthMetricUnit::Permille),
        unavailable(HealthMetricId::IsrLoadPermille,
                    HealthMetricUnit::Permille),
        unavailable(HealthMetricId::MinimumStackFreeBytes,
                    HealthMetricUnit::Bytes),
        available(HealthMetricId::RequestQueueDepth,
                  HealthMetricUnit::Count,
                  observation.request_queue_depth),
        unavailable(HealthMetricId::RequestQueueCapacity,
                    HealthMetricUnit::Count),
        unavailable(HealthMetricId::RetryTotal, HealthMetricUnit::Count),
        unavailable(HealthMetricId::TimeoutTotal, HealthMetricUnit::Count),
        unavailable(HealthMetricId::DuplicateResponseTotal,
                    HealthMetricUnit::Count),
        unavailable(HealthMetricId::UnexpectedResponseTotal,
                    HealthMetricUnit::Count),
        unavailable(HealthMetricId::RxFrameTotal, HealthMetricUnit::Count),
        unavailable(HealthMetricId::TxFrameTotal, HealthMetricUnit::Count),
        unavailable(HealthMetricId::DroppedFrameTotal,
                    HealthMetricUnit::Count),
        available(HealthMetricId::SessionGeneration,
                  HealthMetricUnit::Generation, producer_generation_),
        available(HealthMetricId::UptimeMilliseconds,
                  HealthMetricUnit::Milliseconds,
                  observation.sample_time_ms - started_at_ms_),
        observation.active_lease_count.has_value()
            ? available(HealthMetricId::ActiveLeaseCount,
                        HealthMetricUnit::Count,
                        *observation.active_lease_count)
            : unknown(HealthMetricId::ActiveLeaseCount,
                      HealthMetricUnit::Count),
        observation.resource_fault_count.has_value()
            ? available(HealthMetricId::ResourceFaultCount,
                        HealthMetricUnit::Count,
                        *observation.resource_fault_count)
            : unknown(HealthMetricId::ResourceFaultCount,
                      HealthMetricUnit::Count),
        extension(ToolbusdHealthMetricId::TrafficAdmittedPacketTotal,
                  HealthMetricUnit::Count,
                  observation.traffic.admitted_packets),
        extension(ToolbusdHealthMetricId::TrafficRejectedPacketTotal,
                  HealthMetricUnit::Count,
                  observation.traffic.rejected_packets),
        extension(ToolbusdHealthMetricId::TrafficGuaranteedOverrunTotal,
                  HealthMetricUnit::Count,
                  observation.traffic.guaranteed_overruns),
        extension(ToolbusdHealthMetricId::TrafficAdmittedFrameTotal,
                  HealthMetricUnit::Count,
                  observation.traffic.admitted_frames),
        extension(ToolbusdHealthMetricId::TrafficEstimatedWireTimeNs,
                  kToolbusdNanosecondsUnit,
                  observation.traffic.estimated_wire_time_ns),
        observation.operation_ledger_mutation_available.has_value()
            ? extension(
                  ToolbusdHealthMetricId::
                      RuntimeOperationLedgerMutationAvailable,
                  HealthMetricUnit::Count,
                  *observation.operation_ledger_mutation_available ? 1U : 0U)
            : unknown(
                  static_cast<HealthMetricId>(
                      ToolbusdHealthMetricId::
                          RuntimeOperationLedgerMutationAvailable),
                  HealthMetricUnit::Count),
        observation.operation_ledger_operation_count.has_value()
            ? extension(
                  ToolbusdHealthMetricId::
                      RuntimeOperationLedgerOperationCount,
                  HealthMetricUnit::Count,
                  *observation.operation_ledger_operation_count)
            : unknown(
                  static_cast<HealthMetricId>(
                      ToolbusdHealthMetricId::
                          RuntimeOperationLedgerOperationCount),
                  HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusAdmittedTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->admitted_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusAdmittedTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusRateLimitedTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->rate_limited_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusRateLimitedTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusBusyTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->busy_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusBusyTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusContractRejectedTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->contract_rejected_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusContractRejectedTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusRemoteNackTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->remote_nack_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusRemoteNackTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusRemoteTimeoutTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->remote_timeout_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusRemoteTimeoutTransactionTotal), HealthMetricUnit::Count),
        observation.bus_telemetry.has_value()
            ? extension(ToolbusdHealthMetricId::BusRemoteFaultTransactionTotal,
                        HealthMetricUnit::Count,
                        observation.bus_telemetry->remote_fault_total)
            : unknown(static_cast<HealthMetricId>(ToolbusdHealthMetricId::BusRemoteFaultTransactionTotal), HealthMetricUnit::Count),
    };
    last_sample_time_ms_ = observation.sample_time_ms;

    // 在生产边界立即执行公共契约校验，避免内部构造绕过 wire 约束。
    const auto encoded = protocol::encode_health_snapshot(snapshot);
    return protocol::decode_health_snapshot(encoded);
}

std::uint64_t ToolbusdHealthProducer::producer_generation() const noexcept {
    return producer_generation_;
}

}  // namespace remotebsp::toolbusd
