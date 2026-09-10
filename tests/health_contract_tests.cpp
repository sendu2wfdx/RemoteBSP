#include "remotebsp/protocol/health.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace remotebsp::protocol;

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

HealthMetric metric(HealthMetricId id, MetricAvailability availability,
                    HealthMetricUnit unit, std::uint64_t value = 0U) {
    return {static_cast<std::uint16_t>(id), availability, unit, value};
}

HealthSnapshot valid_snapshot() {
    HealthSnapshot snapshot;
    snapshot.source = HealthSource::Toolbusd;
    snapshot.overall = OverallHealth::Degraded;
    snapshot.sample_sequence = 7U;
    snapshot.sample_time_ms = 1234U;
    snapshot.node_id = 0U;
    snapshot.producer_generation = 9U;
    snapshot.metrics = {
        metric(HealthMetricId::CpuLoadPermille,
               MetricAvailability::Unavailable,
               HealthMetricUnit::Permille),
        metric(HealthMetricId::IsrLoadPermille,
               MetricAvailability::Unknown,
               HealthMetricUnit::Permille),
        metric(HealthMetricId::RequestQueueDepth,
               MetricAvailability::Available,
               HealthMetricUnit::Count, 0U),
        metric(HealthMetricId::RequestQueueCapacity,
               MetricAvailability::Available,
               HealthMetricUnit::Count, 8U),
        metric(HealthMetricId::RetryTotal,
               MetricAvailability::Available,
               HealthMetricUnit::Count, 0U),
        metric(HealthMetricId::TimeoutTotal,
               MetricAvailability::Available,
               HealthMetricUnit::Count, 2U),
        metric(HealthMetricId::BootGeneration,
               MetricAvailability::Unavailable,
               HealthMetricUnit::Generation),
        metric(HealthMetricId::ClockModelGeneration,
               MetricAvailability::Available,
               HealthMetricUnit::Generation, 4U),
        {4000U, MetricAvailability::Available,
         static_cast<HealthMetricUnit>(200U), 42U},
    };
    return snapshot;
}

template <typename Callback>
void expect_error(Callback callback) {
    bool rejected = false;
    try {
        callback();
    } catch (const HealthContractException&) {
        rejected = true;
    }
    CHECK(rejected);
}

void check_round_trip_and_availability_semantics() {
    const auto snapshot = valid_snapshot();
    const auto encoded = encode_health_snapshot(snapshot);
    const auto decoded = decode_health_snapshot(encoded);
    CHECK(decoded.version == kHealthContractVersion);
    CHECK(decoded.source == HealthSource::Toolbusd);
    CHECK(decoded.overall == OverallHealth::Degraded);
    CHECK(decoded.sample_sequence == 7U);
    CHECK(decoded.sample_time_ms == 1234U);
    CHECK(decoded.producer_generation == 9U);
    CHECK(decoded.metrics.size() == snapshot.metrics.size());

    const auto* cpu =
        find_health_metric(decoded, HealthMetricId::CpuLoadPermille);
    const auto* retry =
        find_health_metric(decoded, HealthMetricId::RetryTotal);
    CHECK(cpu != nullptr && retry != nullptr);
    if (cpu != nullptr && retry != nullptr) {
        CHECK(cpu->availability == MetricAvailability::Unavailable);
        CHECK(cpu->value == 0U);
        CHECK(retry->availability == MetricAvailability::Available);
        CHECK(retry->value == 0U);
    }
    CHECK(decoded.metrics.back().metric_id == 4000U);
    CHECK(static_cast<std::uint8_t>(decoded.metrics.back().unit) == 200U);
    CHECK(encode_health_snapshot(decoded) == encoded);
}

void check_forward_compatible_source_and_metric() {
    auto snapshot = valid_snapshot();
    snapshot.source = static_cast<HealthSource>(200U);
    const auto decoded = decode_health_snapshot(
        encode_health_snapshot(snapshot));
    CHECK(static_cast<std::uint8_t>(decoded.source) == 200U);
    CHECK(decoded.metrics.back().metric_id == 4000U);

    snapshot.source = static_cast<HealthSource>(0U);
    expect_error([&] { encode_health_snapshot(snapshot); });
}

void check_metric_validation() {
    auto snapshot = valid_snapshot();
    snapshot.metrics[0].unit = HealthMetricUnit::Count;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[0].value = 1U;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[1].availability =
        static_cast<MetricAvailability>(0U);
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[1].metric_id = snapshot.metrics[0].metric_id;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[0].metric_id = 4000U;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics.back().unit = static_cast<HealthMetricUnit>(0U);
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[0] =
        metric(HealthMetricId::CpuLoadPermille,
               MetricAvailability::Available,
               HealthMetricUnit::Permille, 1001U);
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[2].value = 9U;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[3].value = 0U;
    expect_error([&] { encode_health_snapshot(snapshot); });

    snapshot = valid_snapshot();
    snapshot.metrics[7].value = 0U;
    expect_error([&] { encode_health_snapshot(snapshot); });
}

void check_size_header_and_length_validation() {
    auto encoded = encode_health_snapshot(valid_snapshot());
    auto malformed = encoded;
    malformed.pop_back();
    expect_error([&] { decode_health_snapshot(malformed); });

    malformed = encoded;
    malformed.push_back(0U);
    expect_error([&] { decode_health_snapshot(malformed); });

    malformed = encoded;
    malformed[0U] = 2U;
    expect_error([&] { decode_health_snapshot(malformed); });

    malformed = encoded;
    malformed[34U] = 1U;
    expect_error([&] { decode_health_snapshot(malformed); });

    malformed = encoded;
    malformed[3U] = 255U;
    expect_error([&] { decode_health_snapshot(malformed); });

    malformed = encoded;
    for (std::size_t index = 24U; index < 32U; ++index) {
        malformed[index] = 0U;
    }
    expect_error([&] { decode_health_snapshot(malformed); });

    auto snapshot_with_zero_generation = valid_snapshot();
    snapshot_with_zero_generation.producer_generation = 0U;
    expect_error([&] {
        encode_health_snapshot(snapshot_with_zero_generation);
    });

    expect_error([] { decode_health_snapshot({}); });

    auto snapshot = valid_snapshot();
    snapshot.metrics.clear();
    for (std::uint16_t index = 0U;
         index < kMaximumHealthMetrics + 1U; ++index) {
        snapshot.metrics.push_back(
            {static_cast<std::uint16_t>(100U + index),
             MetricAvailability::Available, HealthMetricUnit::Count, 0U});
    }
    expect_error([&] { encode_health_snapshot(snapshot); });
}

}  // namespace

int main() {
    check_round_trip_and_availability_semantics();
    check_forward_compatible_source_and_metric();
    check_metric_validation();
    check_size_header_and_length_validation();
    return failures == 0 ? 0 : 1;
}
