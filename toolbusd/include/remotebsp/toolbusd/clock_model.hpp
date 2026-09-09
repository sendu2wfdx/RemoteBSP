#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace remotebsp::toolbusd {

enum class ClockSyncState : std::uint8_t {
    Unsynced = 0,
    Synced = 1,
    Degraded = 2,
};

const char* clock_sync_state_name(ClockSyncState state) noexcept;

enum class ClockSampleResult : std::uint8_t {
    Accepted = 0,
    InvalidHostOrder,
    HostTimeWentBackwards,
    CounterOutOfRange,
    CounterAmbiguous,
    InvalidNodeOrder,
    RoundTripTooLarge,
};

const char* clock_sample_result_name(ClockSampleResult result) noexcept;

/*
 * host_send_ns/host_receive_ns 使用主机单调时钟。
 * node_receive_tick/node_send_tick 是同一 MCU 自由运行计数器的原始读数。
 */
struct FourTimestampSample {
    std::uint64_t host_send_ns{};
    std::uint64_t node_receive_tick{};
    std::uint64_t node_send_tick{};
    std::uint64_t host_receive_ns{};
};

struct ClockModelConfig {
    std::uint64_t nominal_tick_rate_hz{1000000ULL};
    std::uint8_t node_counter_bits{64U};
    std::size_t window_size{32U};
    std::size_t low_rtt_sample_count{12U};
    std::size_t minimum_samples{6U};
    std::uint64_t minimum_fit_span_ns{10000000ULL};
    std::uint64_t maximum_round_trip_ns{10000000ULL};
    std::uint64_t synchronized_max_age_ns{1000000000ULL};
    std::uint64_t model_expiry_ns{5000000000ULL};
    std::uint64_t maximum_error_bound_ns{250000ULL};
    std::uint32_t minimum_drift_uncertainty_ppm{25U};
    std::uint32_t maximum_rate_deviation_ppm{5000U};
};

struct ClockObservation {
    FourTimestampSample sample;
    std::uint64_t host_midpoint_ns{};
    std::uint64_t extended_node_receive_tick{};
    std::uint64_t extended_node_send_tick{};
    long double node_midpoint_tick{};
    std::uint64_t host_round_trip_ns{};
    std::uint64_t estimated_network_rtt_ns{};
    long double nominal_offset_ticks{};
};

struct ClockEstimate {
    bool valid{};
    ClockSyncState state{ClockSyncState::Unsynced};
    std::size_t sample_count{};
    std::size_t selected_sample_count{};
    std::uint64_t reference_host_time_ns{};
    long double reference_node_tick{};
    long double scale_ticks_per_ns{};
    long double rate_deviation_ppm{};
    std::uint64_t minimum_network_rtt_ns{};
    std::uint64_t error_bound_ns{};
    std::uint32_t drift_uncertainty_ppm{};
    std::uint64_t last_sample_host_time_ns{};
    std::uint64_t sample_age_ns{};
};

class ClockModel {
public:
    explicit ClockModel(ClockModelConfig config = {});

    ClockSampleResult add_sample(const FourTimestampSample& sample);
    ClockEstimate estimate(std::uint64_t host_now_ns) const;

    std::optional<std::uint64_t> host_to_node_ticks(
        std::uint64_t host_time_ns) const;
    std::optional<std::uint64_t> node_to_host_time_ns(
        std::uint64_t extended_node_tick) const;

    const std::deque<ClockObservation>& observations() const noexcept;
    const ClockModelConfig& config() const noexcept;
    void reset() noexcept;

private:
    ClockSampleResult unwrap_node_timestamps(
        const FourTimestampSample& sample,
        std::uint64_t* receive_tick,
        std::uint64_t* send_tick) const;
    void rebuild();

    ClockModelConfig config_;
    std::deque<ClockObservation> observations_;
    std::vector<std::size_t> selected_indices_;
    bool have_counter_reference_{};
    std::uint64_t last_raw_node_receive_tick_{};
    std::uint64_t last_extended_node_receive_tick_{};
    bool have_host_reference_{};
    std::uint64_t last_host_midpoint_ns_{};
    bool model_valid_{};
    std::uint64_t reference_host_time_ns_{};
    long double reference_node_tick_{};
    long double scale_ticks_per_ns_{};
    long double rate_deviation_ppm_{};
    std::uint64_t minimum_network_rtt_ns_{};
    std::uint64_t base_error_bound_ns_{};
    std::uint32_t drift_uncertainty_ppm_{};
    std::uint64_t last_sample_host_time_ns_{};
};

}
