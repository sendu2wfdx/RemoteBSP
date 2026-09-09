#include "remotebsp/toolbusd/clock_model.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

using remotebsp::toolbusd::ClockModel;
using remotebsp::toolbusd::ClockModelConfig;
using remotebsp::toolbusd::ClockSampleResult;
using remotebsp::toolbusd::ClockSyncState;
using remotebsp::toolbusd::FourTimestampSample;

constexpr std::uint64_t kNominalRateHz = 1000000ULL;

ClockModelConfig test_config() {
    ClockModelConfig config;
    config.nominal_tick_rate_hz = kNominalRateHz;
    config.window_size = 20U;
    config.low_rtt_sample_count = 10U;
    config.minimum_samples = 6U;
    config.minimum_fit_span_ns = 100000000ULL;
    config.maximum_round_trip_ns = 10000000ULL;
    config.synchronized_max_age_ns = 100000000ULL;
    config.model_expiry_ns = 500000000ULL;
    config.maximum_error_bound_ns = 500000ULL;
    config.minimum_drift_uncertainty_ppm = 25U;
    config.maximum_rate_deviation_ppm = 2000U;
    return config;
}

std::uint64_t local_tick(std::uint64_t host_time_ns,
                         long double drift_ppm,
                         long double offset_ticks) {
    const long double scale =
        static_cast<long double>(kNominalRateHz) / 1000000000.0L *
        (1.0L + drift_ppm / 1000000.0L);
    return static_cast<std::uint64_t>(std::floor(
        offset_ticks + scale *
                           static_cast<long double>(host_time_ns) +
        0.5L));
}

FourTimestampSample make_sample(std::uint64_t host_send_ns,
                                std::uint64_t outbound_delay_ns,
                                std::uint64_t processing_ns,
                                std::uint64_t inbound_delay_ns,
                                long double drift_ppm,
                                long double offset_ticks) {
    const auto receive_time = host_send_ns + outbound_delay_ns;
    const auto send_time = receive_time + processing_ns;
    return {
        host_send_ns,
        local_tick(receive_time, drift_ppm, offset_ticks),
        local_tick(send_time, drift_ppm, offset_ticks),
        send_time + inbound_delay_ns,
    };
}

std::uint64_t absolute_difference(std::uint64_t left,
                                  std::uint64_t right) {
    return left >= right ? left - right : right - left;
}

void add_regular_samples(ClockModel& model, long double drift_ppm,
                         long double offset_ticks,
                         std::uint64_t outbound_delay_ns = 50000ULL,
                         std::uint64_t inbound_delay_ns = 50000ULL) {
    for (std::uint64_t index = 0U; index < 12U; ++index) {
        const auto sample = make_sample(
            1000000000ULL + index * 100000000ULL,
            outbound_delay_ns, 20000ULL, inbound_delay_ns,
            drift_ppm, offset_ticks);
        assert(model.add_sample(sample) ==
               ClockSampleResult::Accepted);
    }
}

void test_positive_and_negative_drift() {
    ClockModel positive(test_config());
    add_regular_samples(positive, 80.0L, 2000000.0L);
    const auto positive_now = 2100120000ULL;
    const auto positive_estimate = positive.estimate(positive_now);
    assert(positive_estimate.valid);
    assert(positive_estimate.state == ClockSyncState::Synced);
    assert(std::fabs(positive_estimate.rate_deviation_ppm - 80.0L) <
           0.1L);
    const auto future_host = 2500000000ULL;
    const auto future_tick =
        positive.host_to_node_ticks(future_host);
    assert(future_tick.has_value());
    assert(absolute_difference(
               *future_tick,
               local_tick(future_host, 80.0L, 2000000.0L)) <= 1U);
    const auto recovered_host =
        positive.node_to_host_time_ns(*future_tick);
    assert(recovered_host.has_value());
    assert(absolute_difference(*recovered_host, future_host) <= 1000U);

    ClockModel negative(test_config());
    add_regular_samples(negative, -120.0L, 3000000.0L);
    const auto negative_estimate = negative.estimate(positive_now);
    assert(negative_estimate.valid);
    assert(negative_estimate.state == ClockSyncState::Synced);
    assert(std::fabs(negative_estimate.rate_deviation_ppm + 120.0L) <
           0.1L);
    const auto negative_tick =
        negative.host_to_node_ticks(future_host);
    assert(negative_tick.has_value());
    assert(absolute_difference(
               *negative_tick,
               local_tick(future_host, -120.0L, 3000000.0L)) <= 1U);
}

void test_asymmetric_delay_is_covered_by_error_bound() {
    auto config = test_config();
    config.maximum_error_bound_ns = 100000ULL;
    ClockModel model(config);
    add_regular_samples(
        model, 40.0L, 1000000.0L, 50000ULL, 250000ULL);
    const auto estimate = model.estimate(2100320000ULL);
    assert(estimate.valid);
    assert(estimate.state == ClockSyncState::Degraded);
    assert(estimate.minimum_network_rtt_ns >= 299000ULL);
    assert(estimate.error_bound_ns >= 149000ULL);

    const auto target_host = 2400000000ULL;
    const auto mapped = model.host_to_node_ticks(target_host);
    assert(mapped.has_value());
    const auto expected =
        local_tick(target_host, 40.0L, 1000000.0L);
    const auto mapping_error_ns =
        absolute_difference(*mapped, expected) * 1000ULL;
    assert(mapping_error_ns <= estimate.error_bound_ns);
}

void test_low_rtt_selection_rejects_delay_outlier() {
    auto config = test_config();
    config.low_rtt_sample_count = 8U;
    ClockModel model(config);
    for (std::uint64_t index = 0U; index < 14U; ++index) {
        const bool outlier = index == 6U;
        const auto outbound_delay =
            outlier ? 6000000ULL : 50000ULL;
        const auto inbound_delay = outlier ? 0ULL : 50000ULL;
        const auto sample = make_sample(
            1000000000ULL + index * 100000000ULL,
            outbound_delay, 20000ULL, inbound_delay,
            60.0L, 1500000.0L);
        assert(model.add_sample(sample) ==
               ClockSampleResult::Accepted);
    }
    const auto estimate = model.estimate(2300120000ULL);
    assert(estimate.valid);
    assert(estimate.selected_sample_count == 8U);
    assert(estimate.minimum_network_rtt_ns <= 101000ULL);
    assert(std::fabs(estimate.rate_deviation_ppm - 60.0L) < 0.1L);
    assert(estimate.error_bound_ns < 100000ULL);
}

void test_excluded_sample_does_not_refresh_model_age() {
    ClockModel model(test_config());
    add_regular_samples(model, 30.0L, 1500000.0L);
    constexpr std::uint64_t last_selected_receive = 2100120000ULL;
    const auto outlier = make_sample(
        2200000000ULL, 6000000ULL, 20000ULL, 0ULL,
        30.0L, 1500000.0L);
    assert(model.add_sample(outlier) == ClockSampleResult::Accepted);

    const auto estimate = model.estimate(outlier.host_receive_ns);
    assert(estimate.valid);
    assert(estimate.last_sample_host_time_ns == last_selected_receive);
    assert(estimate.sample_age_ns ==
           outlier.host_receive_ns - last_selected_receive);
    assert(estimate.state == ClockSyncState::Degraded);
}

void test_model_aging_states_and_error_growth() {
    ClockModel model(test_config());
    add_regular_samples(model, 25.0L, 1000000.0L);
    constexpr std::uint64_t last_receive = 2100120000ULL;
    const auto fresh = model.estimate(last_receive);
    assert(fresh.state == ClockSyncState::Synced);
    const auto degraded = model.estimate(
        last_receive + model.config().synchronized_max_age_ns + 1U);
    assert(degraded.state == ClockSyncState::Degraded);
    assert(degraded.error_bound_ns > fresh.error_bound_ns);
    const auto expired = model.estimate(
        last_receive + model.config().model_expiry_ns + 1U);
    assert(expired.state == ClockSyncState::Unsynced);
    assert(expired.error_bound_ns >= degraded.error_bound_ns);
    assert(model.estimate(last_receive - 1U).state ==
           ClockSyncState::Unsynced);
}

void test_counter_wrap_and_ambiguous_input() {
    auto config = test_config();
    config.node_counter_bits = 32U;
    config.window_size = 8U;
    config.low_rtt_sample_count = 6U;
    config.minimum_samples = 4U;
    config.minimum_fit_span_ns = 1000000ULL;
    ClockModel model(config);

    constexpr std::uint64_t mask = 0xffffffffULL;
    constexpr std::uint64_t first_receive = mask - 7U;
    for (std::uint64_t index = 0U; index < 6U; ++index) {
        const auto extended_receive =
            first_receive + index * 1000ULL;
        const auto extended_send = extended_receive + 10ULL;
        const FourTimestampSample sample = {
            1000000ULL + index * 1000000ULL,
            extended_receive & mask,
            extended_send & mask,
            1110000ULL + index * 1000000ULL,
        };
        assert(model.add_sample(sample) ==
               ClockSampleResult::Accepted);
    }
    assert(model.observations().front().extended_node_send_tick > mask);
    assert(model.observations().back().extended_node_receive_tick > mask);
    const auto estimate = model.estimate(6110000ULL);
    assert(estimate.valid);
    assert(estimate.state == ClockSyncState::Synced);
    assert(std::fabs(estimate.rate_deviation_ppm) < 0.1L);

    const auto last_raw =
        model.observations().back().sample.node_receive_tick;
    const FourTimestampSample backwards = {
        7000000ULL,
        (last_raw - 1U) & mask,
        (last_raw + 9U) & mask,
        7110000ULL,
    };
    assert(model.add_sample(backwards) ==
           ClockSampleResult::CounterAmbiguous);
    assert(model.observations().size() == 6U);

    auto narrow_config = test_config();
    narrow_config.node_counter_bits = 16U;
    ClockModel narrow(narrow_config);
    assert(narrow.add_sample({0U, 0x10000U, 0U, 1000U}) ==
           ClockSampleResult::CounterOutOfRange);
}

void test_invalid_samples_do_not_mutate_model() {
    ClockModel model(test_config());
    assert(model.add_sample({100U, 0U, 1U, 99U}) ==
           ClockSampleResult::InvalidHostOrder);
    assert(model.add_sample({0U, 0U, 200U, 1000U}) ==
           ClockSampleResult::InvalidNodeOrder);
    assert(model.add_sample({0U, 0U, 1U, 20000000ULL}) ==
           ClockSampleResult::RoundTripTooLarge);
    assert(model.observations().empty());
    assert(!model.host_to_node_ticks(1000U).has_value());
    assert(model.estimate(1000U).state == ClockSyncState::Unsynced);

    bool threw = false;
    try {
        auto invalid = test_config();
        invalid.low_rtt_sample_count = invalid.minimum_samples - 1U;
        ClockModel rejected(invalid);
        static_cast<void>(rejected);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

}

int main() {
    test_positive_and_negative_drift();
    test_asymmetric_delay_is_covered_by_error_bound();
    test_low_rtt_selection_rejects_delay_outlier();
    test_excluded_sample_does_not_refresh_model_age();
    test_model_aging_states_and_error_growth();
    test_counter_wrap_and_ambiguous_input();
    test_invalid_samples_do_not_mutate_model();
}
