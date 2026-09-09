#include "remotebsp/toolbusd/clock_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace remotebsp::toolbusd {
namespace {

constexpr long double kNanosecondsPerSecond = 1000000000.0L;
constexpr long double kPartsPerMillion = 1000000.0L;

long double median(std::vector<long double> values) {
    if (values.empty()) {
        return 0.0L;
    }
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2U;
    if ((values.size() & 1U) != 0U) {
        return values[middle];
    }
    return (values[middle - 1U] + values[middle]) / 2.0L;
}

long double signed_time_delta(std::uint64_t value,
                              std::uint64_t reference) {
    if (value >= reference) {
        return static_cast<long double>(value - reference);
    }
    return -static_cast<long double>(reference - value);
}

std::optional<std::uint64_t> rounded_u64(long double value) {
    if (!std::isfinite(value) || value < 0.0L) {
        return std::nullopt;
    }
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (value >= maximum) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::floor(value + 0.5L));
}

std::uint64_t saturating_add(std::uint64_t left,
                             std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

}

const char* clock_sync_state_name(ClockSyncState state) noexcept {
    switch (state) {
        case ClockSyncState::Unsynced:
            return "unsynced";
        case ClockSyncState::Synced:
            return "synced";
        case ClockSyncState::Degraded:
            return "degraded";
    }
    return "unknown";
}

const char* clock_sample_result_name(ClockSampleResult result) noexcept {
    switch (result) {
        case ClockSampleResult::Accepted:
            return "accepted";
        case ClockSampleResult::InvalidHostOrder:
            return "invalid_host_order";
        case ClockSampleResult::HostTimeWentBackwards:
            return "host_time_went_backwards";
        case ClockSampleResult::CounterOutOfRange:
            return "counter_out_of_range";
        case ClockSampleResult::CounterAmbiguous:
            return "counter_ambiguous";
        case ClockSampleResult::InvalidNodeOrder:
            return "invalid_node_order";
        case ClockSampleResult::RoundTripTooLarge:
            return "round_trip_too_large";
    }
    return "unknown";
}

ClockModel::ClockModel(ClockModelConfig config)
    : config_(std::move(config)),
      scale_ticks_per_ns_(
          static_cast<long double>(config_.nominal_tick_rate_hz) /
          kNanosecondsPerSecond),
      drift_uncertainty_ppm_(
          config_.minimum_drift_uncertainty_ppm) {
    if (config_.nominal_tick_rate_hz == 0U ||
        config_.node_counter_bits < 2U ||
        config_.node_counter_bits > 64U || config_.window_size < 2U ||
        config_.minimum_samples < 2U ||
        config_.low_rtt_sample_count < config_.minimum_samples ||
        config_.low_rtt_sample_count > config_.window_size ||
        config_.minimum_fit_span_ns == 0U ||
        config_.maximum_round_trip_ns == 0U ||
        config_.synchronized_max_age_ns == 0U ||
        config_.model_expiry_ns < config_.synchronized_max_age_ns ||
        config_.maximum_error_bound_ns == 0U ||
        config_.maximum_rate_deviation_ppm == 0U) {
        throw std::invalid_argument("时钟模型配置无效");
    }
    base_error_bound_ns_ =
        std::numeric_limits<std::uint64_t>::max();
}

ClockSampleResult ClockModel::unwrap_node_timestamps(
    const FourTimestampSample& sample,
    std::uint64_t* receive_tick,
    std::uint64_t* send_tick) const {
    if (config_.node_counter_bits == 64U) {
        if (sample.node_send_tick < sample.node_receive_tick) {
            return ClockSampleResult::InvalidNodeOrder;
        }
        if (have_counter_reference_ &&
            sample.node_receive_tick < last_raw_node_receive_tick_) {
            return ClockSampleResult::CounterAmbiguous;
        }
        *receive_tick = sample.node_receive_tick;
        *send_tick = sample.node_send_tick;
        return ClockSampleResult::Accepted;
    }

    const std::uint64_t modulus =
        1ULL << config_.node_counter_bits;
    const std::uint64_t mask = modulus - 1U;
    const std::uint64_t half_range = modulus / 2U;
    if (sample.node_receive_tick > mask ||
        sample.node_send_tick > mask) {
        return ClockSampleResult::CounterOutOfRange;
    }

    const auto turnaround =
        (sample.node_send_tick - sample.node_receive_tick) & mask;
    if (turnaround >= half_range) {
        return ClockSampleResult::InvalidNodeOrder;
    }

    std::uint64_t extended_receive = sample.node_receive_tick;
    if (have_counter_reference_) {
        const auto delta =
            (sample.node_receive_tick -
             last_raw_node_receive_tick_) &
            mask;
        if (delta >= half_range ||
            delta > std::numeric_limits<std::uint64_t>::max() -
                        last_extended_node_receive_tick_) {
            return ClockSampleResult::CounterAmbiguous;
        }
        extended_receive = last_extended_node_receive_tick_ + delta;
    }
    if (turnaround > std::numeric_limits<std::uint64_t>::max() -
                         extended_receive) {
        return ClockSampleResult::CounterAmbiguous;
    }
    *receive_tick = extended_receive;
    *send_tick = extended_receive + turnaround;
    return ClockSampleResult::Accepted;
}

ClockSampleResult ClockModel::add_sample(
    const FourTimestampSample& sample) {
    if (sample.host_receive_ns < sample.host_send_ns) {
        return ClockSampleResult::InvalidHostOrder;
    }
    const auto host_round_trip =
        sample.host_receive_ns - sample.host_send_ns;
    if (host_round_trip > config_.maximum_round_trip_ns) {
        return ClockSampleResult::RoundTripTooLarge;
    }
    const auto host_midpoint =
        sample.host_send_ns + host_round_trip / 2U;
    if (have_host_reference_ &&
        (host_midpoint <= last_host_midpoint_ns_ ||
         sample.host_receive_ns <= last_sample_host_time_ns_)) {
        return ClockSampleResult::HostTimeWentBackwards;
    }

    std::uint64_t node_receive = 0U;
    std::uint64_t node_send = 0U;
    const auto unwrap_result = unwrap_node_timestamps(
        sample, &node_receive, &node_send);
    if (unwrap_result != ClockSampleResult::Accepted) {
        return unwrap_result;
    }

    const auto node_turnaround = node_send - node_receive;
    const long double node_turnaround_ns =
        static_cast<long double>(node_turnaround) *
        kNanosecondsPerSecond /
        static_cast<long double>(config_.nominal_tick_rate_hz);
    const long double tick_period_ns =
        kNanosecondsPerSecond /
        static_cast<long double>(config_.nominal_tick_rate_hz);
    if (node_turnaround_ns >
        static_cast<long double>(host_round_trip) + tick_period_ns) {
        return ClockSampleResult::InvalidNodeOrder;
    }
    const auto rounded_turnaround_ns = rounded_u64(node_turnaround_ns);
    if (!rounded_turnaround_ns.has_value()) {
        return ClockSampleResult::InvalidNodeOrder;
    }
    const auto network_rtt =
        *rounded_turnaround_ns >= host_round_trip
            ? 0U
            : host_round_trip - *rounded_turnaround_ns;
    const long double node_midpoint =
        static_cast<long double>(node_receive) +
        static_cast<long double>(node_turnaround) / 2.0L;
    const long double nominal_scale =
        static_cast<long double>(config_.nominal_tick_rate_hz) /
        kNanosecondsPerSecond;

    observations_.push_back(
        {sample,
         host_midpoint,
         node_receive,
         node_send,
         node_midpoint,
         host_round_trip,
         network_rtt,
         node_midpoint - nominal_scale *
                             static_cast<long double>(host_midpoint)});
    if (observations_.size() > config_.window_size) {
        observations_.pop_front();
    }
    have_counter_reference_ = true;
    last_raw_node_receive_tick_ = sample.node_receive_tick;
    last_extended_node_receive_tick_ = node_receive;
    have_host_reference_ = true;
    last_host_midpoint_ns_ = host_midpoint;
    last_sample_host_time_ns_ = sample.host_receive_ns;
    rebuild();
    return ClockSampleResult::Accepted;
}

void ClockModel::rebuild() {
    selected_indices_.clear();
    model_valid_ = false;
    scale_ticks_per_ns_ =
        static_cast<long double>(config_.nominal_tick_rate_hz) /
        kNanosecondsPerSecond;
    rate_deviation_ppm_ = 0.0L;
    base_error_bound_ns_ = std::numeric_limits<std::uint64_t>::max();
    drift_uncertainty_ppm_ =
        config_.minimum_drift_uncertainty_ppm;
    if (observations_.size() < config_.minimum_samples) {
        return;
    }

    selected_indices_.reserve(observations_.size());
    for (std::size_t index = 0U;
         index < observations_.size(); ++index) {
        selected_indices_.push_back(index);
    }
    std::stable_sort(
        selected_indices_.begin(), selected_indices_.end(),
        [&](std::size_t left, std::size_t right) {
            const auto& left_sample = observations_[left];
            const auto& right_sample = observations_[right];
            if (left_sample.estimated_network_rtt_ns !=
                right_sample.estimated_network_rtt_ns) {
                return left_sample.estimated_network_rtt_ns <
                       right_sample.estimated_network_rtt_ns;
            }
            /* RTT 相同时优先保留较新样本，避免窗口尚未填满时只拟合旧数据。 */
            return left_sample.host_midpoint_ns >
                   right_sample.host_midpoint_ns;
        });
    if (selected_indices_.size() >
        config_.low_rtt_sample_count) {
        selected_indices_.resize(config_.low_rtt_sample_count);
    }
    std::sort(
        selected_indices_.begin(), selected_indices_.end(),
        [&](std::size_t left, std::size_t right) {
            return observations_[left].host_midpoint_ns <
                   observations_[right].host_midpoint_ns;
        });
    if (selected_indices_.size() < config_.minimum_samples) {
        return;
    }
    const auto first_host =
        observations_[selected_indices_.front()].host_midpoint_ns;
    const auto last_host =
        observations_[selected_indices_.back()].host_midpoint_ns;
    if (last_host - first_host < config_.minimum_fit_span_ns) {
        return;
    }

    std::vector<long double> slopes;
    for (std::size_t left = 0U;
         left < selected_indices_.size(); ++left) {
        const auto& first = observations_[selected_indices_[left]];
        for (std::size_t right = left + 1U;
             right < selected_indices_.size(); ++right) {
            const auto& second =
                observations_[selected_indices_[right]];
            const auto host_delta =
                second.host_midpoint_ns - first.host_midpoint_ns;
            if (host_delta == 0U) {
                continue;
            }
            slopes.push_back(
                (second.node_midpoint_tick -
                 first.node_midpoint_tick) /
                static_cast<long double>(host_delta));
        }
    }
    if (slopes.empty()) {
        return;
    }
    const auto fitted_scale = median(slopes);
    const auto nominal_scale =
        static_cast<long double>(config_.nominal_tick_rate_hz) /
        kNanosecondsPerSecond;
    const auto tick_period_ns =
        kNanosecondsPerSecond /
        static_cast<long double>(config_.nominal_tick_rate_hz);
    if (!std::isfinite(fitted_scale) || fitted_scale <= 0.0L) {
        return;
    }
    const auto rate_deviation_ppm =
        (fitted_scale / nominal_scale - 1.0L) *
        kPartsPerMillion;
    if (std::fabs(rate_deviation_ppm) >
        static_cast<long double>(
            config_.maximum_rate_deviation_ppm)) {
        return;
    }

    const auto reference_host = observations_[
        selected_indices_[selected_indices_.size() / 2U]]
                                    .host_midpoint_ns;
    std::vector<long double> reference_values;
    reference_values.reserve(selected_indices_.size());
    for (const auto index : selected_indices_) {
        const auto& observation = observations_[index];
        reference_values.push_back(
            observation.node_midpoint_tick -
            fitted_scale * signed_time_delta(
                               observation.host_midpoint_ns,
                               reference_host));
    }
    const auto reference_node = median(reference_values);

    long double maximum_error_ns = 0.0L;
    for (const auto index : selected_indices_) {
        const auto& observation = observations_[index];
        const auto predicted =
            reference_node +
            fitted_scale * signed_time_delta(
                               observation.host_midpoint_ns,
                               reference_host);
        const auto residual_ns =
            std::fabs(observation.node_midpoint_tick - predicted) /
            fitted_scale;
        const auto uncertainty_ns =
            residual_ns +
            static_cast<long double>(
                observation.estimated_network_rtt_ns) /
                2.0L +
            tick_period_ns;
        maximum_error_ns =
            std::max(maximum_error_ns, uncertainty_ns);
    }

    std::vector<long double> slope_deviations_ppm;
    slope_deviations_ppm.reserve(slopes.size());
    for (const auto slope : slopes) {
        slope_deviations_ppm.push_back(
            std::fabs(slope - fitted_scale) / fitted_scale *
            kPartsPerMillion);
    }
    const auto robust_drift_ppm =
        static_cast<std::uint64_t>(std::ceil(
            median(std::move(slope_deviations_ppm)) * 3.0L));
    const auto bounded_drift_ppm = std::min<std::uint64_t>(
        robust_drift_ppm,
        std::numeric_limits<std::uint32_t>::max());

    model_valid_ = true;
    reference_host_time_ns_ = reference_host;
    reference_node_tick_ = reference_node;
    scale_ticks_per_ns_ = fitted_scale;
    rate_deviation_ppm_ = rate_deviation_ppm;
    minimum_network_rtt_ns_ =
        observations_[selected_indices_.front()]
            .estimated_network_rtt_ns;
    for (const auto index : selected_indices_) {
        minimum_network_rtt_ns_ = std::min(
            minimum_network_rtt_ns_,
            observations_[index].estimated_network_rtt_ns);
    }
    base_error_bound_ns_ =
        maximum_error_ns >= static_cast<long double>(
                                std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(
                  std::ceil(maximum_error_ns));
    drift_uncertainty_ppm_ = std::max(
        config_.minimum_drift_uncertainty_ppm,
        static_cast<std::uint32_t>(bounded_drift_ppm));
    /* 高 RTT 未入选样本不能刷新模型的新鲜度。 */
    last_sample_host_time_ns_ =
        observations_[selected_indices_.back()].sample.host_receive_ns;
}

ClockEstimate ClockModel::estimate(std::uint64_t host_now_ns) const {
    ClockEstimate result;
    result.valid = model_valid_;
    result.sample_count = observations_.size();
    result.selected_sample_count = selected_indices_.size();
    result.reference_host_time_ns = reference_host_time_ns_;
    result.reference_node_tick = reference_node_tick_;
    result.scale_ticks_per_ns = scale_ticks_per_ns_;
    result.rate_deviation_ppm = rate_deviation_ppm_;
    result.minimum_network_rtt_ns = minimum_network_rtt_ns_;
    result.drift_uncertainty_ppm = drift_uncertainty_ppm_;
    result.last_sample_host_time_ns = last_sample_host_time_ns_;
    result.error_bound_ns = base_error_bound_ns_;
    if (!model_valid_ || host_now_ns < last_sample_host_time_ns_) {
        result.state = ClockSyncState::Unsynced;
        return result;
    }

    result.sample_age_ns = host_now_ns - last_sample_host_time_ns_;
    const long double growth =
        static_cast<long double>(result.sample_age_ns) *
        static_cast<long double>(drift_uncertainty_ppm_) /
        kPartsPerMillion;
    const auto growth_ns =
        growth >= static_cast<long double>(
                      std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(std::ceil(growth));
    result.error_bound_ns =
        saturating_add(base_error_bound_ns_, growth_ns);

    if (result.sample_age_ns > config_.model_expiry_ns) {
        result.state = ClockSyncState::Unsynced;
    } else if (
        result.sample_age_ns > config_.synchronized_max_age_ns ||
        result.error_bound_ns > config_.maximum_error_bound_ns) {
        result.state = ClockSyncState::Degraded;
    } else {
        result.state = ClockSyncState::Synced;
    }
    return result;
}

std::optional<std::uint64_t> ClockModel::host_to_node_ticks(
    std::uint64_t host_time_ns) const {
    if (!model_valid_) {
        return std::nullopt;
    }
    return rounded_u64(
        reference_node_tick_ +
        scale_ticks_per_ns_ * signed_time_delta(
                                  host_time_ns,
                                  reference_host_time_ns_));
}

std::optional<std::uint64_t> ClockModel::node_to_host_time_ns(
    std::uint64_t extended_node_tick) const {
    if (!model_valid_ || scale_ticks_per_ns_ <= 0.0L) {
        return std::nullopt;
    }
    const auto node_delta =
        static_cast<long double>(extended_node_tick) -
        reference_node_tick_;
    return rounded_u64(
        static_cast<long double>(reference_host_time_ns_) +
        node_delta / scale_ticks_per_ns_);
}

const std::deque<ClockObservation>& ClockModel::observations()
    const noexcept {
    return observations_;
}

const ClockModelConfig& ClockModel::config() const noexcept {
    return config_;
}

void ClockModel::reset() noexcept {
    observations_.clear();
    selected_indices_.clear();
    have_counter_reference_ = false;
    last_raw_node_receive_tick_ = 0U;
    last_extended_node_receive_tick_ = 0U;
    have_host_reference_ = false;
    last_host_midpoint_ns_ = 0U;
    model_valid_ = false;
    reference_host_time_ns_ = 0U;
    reference_node_tick_ = 0.0L;
    scale_ticks_per_ns_ =
        static_cast<long double>(config_.nominal_tick_rate_hz) /
        kNanosecondsPerSecond;
    rate_deviation_ppm_ = 0.0L;
    minimum_network_rtt_ns_ = 0U;
    base_error_bound_ns_ = std::numeric_limits<std::uint64_t>::max();
    drift_uncertainty_ppm_ =
        config_.minimum_drift_uncertainty_ppm;
    last_sample_host_time_ns_ = 0U;
}

}
