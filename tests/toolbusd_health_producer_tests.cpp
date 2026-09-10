#include "remotebsp/protocol/health.hpp"
#include "remotebsp/toolbusd/health_producer.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

protocol::Packet make_request() {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(
        protocol::Command::Ping);
    request.header.session_id = 7U;
    request.header.object_id = 1U;
    return request;
}

toolbusd::TrafficSnapshot make_traffic() {
    toolbusd::TrafficConfig config;
    const auto start = toolbusd::TrafficController::TimePoint{};
    toolbusd::TrafficController traffic(config, start);
    CHECK(traffic.admit(toolbusd::TrafficClass::Interactive, {8U},
                        toolbusd::AdmissionPolicy::Enforce, start));
    return traffic.snapshot(start);
}

const protocol::HealthMetric* find_metric(
    const protocol::HealthSnapshot& snapshot, std::uint16_t id) {
    const auto found = std::find_if(
        snapshot.metrics.begin(), snapshot.metrics.end(),
        [id](const auto& metric) { return metric.metric_id == id; });
    return found == snapshot.metrics.end() ? nullptr : &*found;
}

void test_real_software_state_to_wire_contract() {
    toolbusd::RequestManager requests;
    requests.submit(make_request(), toolbusd::RequestManager::TimePoint{});

    toolbusd::ToolbusdHealthProducer producer(0x1234U, 1000U);
    toolbusd::ToolbusdHealthObservation observation;
    observation.sample_time_ms = 1250U;
    observation.request_queue_depth = requests.pending_count();
    observation.traffic = make_traffic();
    observation.active_lease_count = 2U;
    observation.resource_fault_count = 0U;
    observation.operation_ledger_mutation_available = true;
    observation.operation_ledger_operation_count = 7U;
    toolbusd::BusTelemetrySnapshot bus;
    bus.admitted_total = 3U;
    bus.rate_limited_total = 2U;
    bus.busy_total = 1U;
    bus.contract_rejected_total = 4U;
    bus.remote_ok_total = 8U;
    bus.remote_nack_total = 5U;
    bus.remote_timeout_total = 6U;
    bus.remote_fault_total = 7U;
    bus.resources = {{1U, 0x0C000001U, 3U, 2U, 1U, 4U,
                      8U, 5U, 6U, 0U, 7U, 0U}};
    observation.bus_telemetry = bus;

    const auto snapshot = producer.capture(observation);
    CHECK(snapshot.version == protocol::kHealthContractVersion);
    CHECK(snapshot.source == protocol::HealthSource::Toolbusd);
    CHECK(snapshot.node_id == 0U);
    CHECK(snapshot.producer_generation == 0x1234U);
    CHECK(snapshot.sample_sequence == 1U);
    CHECK(snapshot.sample_time_ms == 250U);
    CHECK(snapshot.overall == protocol::OverallHealth::Unknown);

    const auto* depth = protocol::find_health_metric(
        snapshot, protocol::HealthMetricId::RequestQueueDepth);
    const auto* uptime = protocol::find_health_metric(
        snapshot, protocol::HealthMetricId::UptimeMilliseconds);
    const auto* tx = protocol::find_health_metric(
        snapshot, protocol::HealthMetricId::TxFrameTotal);
    const auto* admitted = find_metric(
        snapshot, static_cast<std::uint16_t>(
            toolbusd::ToolbusdHealthMetricId::TrafficAdmittedPacketTotal));
    const auto* ledger_available = find_metric(
        snapshot, static_cast<std::uint16_t>(
            toolbusd::ToolbusdHealthMetricId::
                RuntimeOperationLedgerMutationAvailable));
    const auto* ledger_operations = find_metric(
        snapshot, static_cast<std::uint16_t>(
            toolbusd::ToolbusdHealthMetricId::
                RuntimeOperationLedgerOperationCount));
    CHECK(depth != nullptr && depth->value == 1U);
    CHECK(uptime != nullptr && uptime->value == 250U);
    // 准入成功不等价于物理发送成功，标准 Tx 指标必须保持不可用。
    CHECK(tx != nullptr &&
          tx->availability == protocol::MetricAvailability::Unavailable &&
          tx->value == 0U);
    CHECK(admitted != nullptr &&
          admitted->availability == protocol::MetricAvailability::Available &&
          admitted->value == 1U);
    CHECK(ledger_available != nullptr && ledger_available->value == 1U);
    CHECK(ledger_operations != nullptr && ledger_operations->value == 7U);
    const auto* bus_limited = find_metric(snapshot, static_cast<std::uint16_t>(
        toolbusd::ToolbusdHealthMetricId::BusRateLimitedTransactionTotal));
    const auto* bus_rejected = find_metric(snapshot, static_cast<std::uint16_t>(
        toolbusd::ToolbusdHealthMetricId::BusContractRejectedTransactionTotal));
    CHECK(bus_limited != nullptr && bus_limited->unit == protocol::HealthMetricUnit::Count &&
          bus_limited->value == 2U);
    CHECK(bus_rejected != nullptr && bus_rejected->value == 4U);
    const auto* remote_nack = find_metric(snapshot, static_cast<std::uint16_t>(
        toolbusd::ToolbusdHealthMetricId::BusRemoteNackTransactionTotal));
    const auto* remote_timeout = find_metric(snapshot, static_cast<std::uint16_t>(
        toolbusd::ToolbusdHealthMetricId::BusRemoteTimeoutTransactionTotal));
    const auto* remote_fault = find_metric(snapshot, static_cast<std::uint16_t>(
        toolbusd::ToolbusdHealthMetricId::BusRemoteFaultTransactionTotal));
    CHECK(remote_nack != nullptr && remote_nack->value == 5U &&
          remote_nack->unit == protocol::HealthMetricUnit::Count);
    CHECK(remote_timeout != nullptr && remote_timeout->value == 6U);
    CHECK(remote_fault != nullptr && remote_fault->value == 7U);

    const auto wire = protocol::encode_health_snapshot(snapshot);
    const auto decoded = protocol::decode_health_snapshot(wire);
    CHECK(decoded.sample_sequence == snapshot.sample_sequence);
    CHECK(decoded.metrics.size() == snapshot.metrics.size());
}

void test_ledger_unavailable_degrades_health() {
    toolbusd::ToolbusdHealthProducer producer(4U, 0U);
    toolbusd::ToolbusdHealthObservation observation;
    observation.sample_time_ms = 1U;
    observation.traffic = make_traffic();
    observation.operation_ledger_mutation_available = false;
    observation.operation_ledger_operation_count = 3U;
    const auto snapshot = producer.capture(observation);
    CHECK(snapshot.overall == protocol::OverallHealth::Degraded);
    const auto* available = find_metric(
        snapshot, static_cast<std::uint16_t>(
            toolbusd::ToolbusdHealthMetricId::
                RuntimeOperationLedgerMutationAvailable));
    CHECK(available != nullptr && available->value == 0U);
}

void test_unknown_is_not_zero_measurement() {
    toolbusd::ToolbusdHealthProducer producer(2U, 0U);
    toolbusd::ToolbusdHealthObservation observation;
    observation.sample_time_ms = 0U;
    observation.traffic = make_traffic();
    const auto snapshot = producer.capture(observation);
    const auto* leases = protocol::find_health_metric(
        snapshot, protocol::HealthMetricId::ActiveLeaseCount);
    const auto* faults = protocol::find_health_metric(
        snapshot, protocol::HealthMetricId::ResourceFaultCount);
    CHECK(leases != nullptr &&
          leases->availability == protocol::MetricAvailability::Unknown &&
          leases->value == 0U);
    CHECK(faults != nullptr &&
          faults->availability == protocol::MetricAvailability::Unknown &&
          faults->value == 0U);
}

void test_invalid_input_isolated_without_consuming_sequence() {
    toolbusd::ToolbusdHealthProducer producer(3U, 100U);
    toolbusd::ToolbusdHealthObservation invalid;
    invalid.sample_time_ms = 101U;
    invalid.traffic = make_traffic();
    ++invalid.traffic.admitted_packets;
    bool rejected = false;
    try {
        static_cast<void>(producer.capture(invalid));
    } catch (const toolbusd::ToolbusdHealthProducerException& error) {
        rejected = error.code() ==
            toolbusd::ToolbusdHealthProducerError::InvalidTrafficSnapshot;
    }
    CHECK(rejected);

    auto valid = invalid;
    --valid.traffic.admitted_packets;
    const auto first = producer.capture(valid);
    CHECK(first.sample_sequence == 1U);

    valid.sample_time_ms = 99U;
    rejected = false;
    try {
        static_cast<void>(producer.capture(valid));
    } catch (const toolbusd::ToolbusdHealthProducerException& error) {
        rejected = error.code() ==
            toolbusd::ToolbusdHealthProducerError::ClockRegression;
    }
    CHECK(rejected);
}

void test_generation_and_concurrent_sequences_are_isolated() {
    bool rejected = false;
    try {
        toolbusd::ToolbusdHealthProducer invalid(0U, 0U);
        static_cast<void>(invalid);
    } catch (const toolbusd::ToolbusdHealthProducerException& error) {
        rejected = error.code() ==
            toolbusd::ToolbusdHealthProducerError::InvalidGeneration;
    }
    CHECK(rejected);

    toolbusd::ToolbusdHealthProducer producer(9U, 100U);
    const auto traffic = make_traffic();
    std::vector<std::uint64_t> sequences(8U);
    std::vector<std::thread> workers;
    workers.reserve(sequences.size());
    for (std::size_t index = 0U; index < sequences.size(); ++index) {
        workers.emplace_back([&, index] {
            toolbusd::ToolbusdHealthObservation observation;
            observation.sample_time_ms = 100U;
            observation.traffic = traffic;
            sequences[index] = producer.capture(observation).sample_sequence;
        });
    }
    for (auto& worker : workers) worker.join();
    std::sort(sequences.begin(), sequences.end());
    for (std::size_t index = 0U; index < sequences.size(); ++index) {
        CHECK(sequences[index] == index + 1U);
    }

    toolbusd::ToolbusdHealthProducer restarted(10U, 100U);
    toolbusd::ToolbusdHealthObservation observation;
    observation.sample_time_ms = 100U;
    observation.traffic = traffic;
    const auto after_restart = restarted.capture(observation);
    CHECK(after_restart.producer_generation == 10U);
    CHECK(after_restart.sample_sequence == 1U);
}

}  // namespace

int main() {
    test_real_software_state_to_wire_contract();
    test_ledger_unavailable_degrades_health();
    test_unknown_is_not_zero_measurement();
    test_invalid_input_isolated_without_consuming_sequence();
    test_generation_and_concurrent_sequences_are_isolated();
    if (failures != 0) {
        std::cerr << failures << " 项 toolbusd 健康生产链测试失败\n";
        return 1;
    }
    std::cout << "toolbusd 健康生产链测试通过\n";
    return 0;
}
