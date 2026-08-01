#include "remotebsp/mock_mcu/motion_executor.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

namespace remotebsp::mock_mcu {
namespace {

std::uint64_t absolute_steps(std::int32_t steps) {
    return steps < 0
               ? static_cast<std::uint64_t>(
                     -static_cast<std::int64_t>(steps))
               : static_cast<std::uint64_t>(steps);
}

int edge_order(const MotionEdge& edge) {
    if (edge.signal == MotionSignal::Step) {
        return edge.level ? 3 : 0;
    }
    return edge.signal == MotionSignal::Enable ? 1 : 2;
}

bool edge_less(const MotionEdge& left, const MotionEdge& right) {
    if (left.global_time_ns != right.global_time_ns) {
        return left.global_time_ns < right.global_time_ns;
    }
    const int left_order = edge_order(left);
    const int right_order = edge_order(right);
    if (left_order != right_order) {
        return left_order < right_order;
    }
    return left.axis_resource_id < right.axis_resource_id;
}

std::uint64_t interpolate_offset(std::uint64_t span,
                                 std::uint64_t index,
                                 std::uint64_t denominator) {
    if (denominator == 0) {
        return 0;
    }
    return (span / denominator) * index +
           ((span % denominator) * index) / denominator;
}

bool rate_allows(std::uint64_t step_count,
                 std::uint64_t duration_ns,
                 std::uint32_t maximum_step_rate_hz) {
    constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
    const auto whole_seconds = duration_ns / kNanosecondsPerSecond;
    const auto remaining_ns = duration_ns % kNanosecondsPerSecond;

    // 先作商比较，避免 maximum_step_rate_hz * duration_ns 溢出。
    if (whole_seconds >
        step_count / static_cast<std::uint64_t>(maximum_step_rate_hz)) {
        return true;
    }
    const auto allowed_steps =
        whole_seconds * maximum_step_rate_hz +
        (remaining_ns * maximum_step_rate_hz) /
            kNanosecondsPerSecond;
    return step_count <= allowed_steps;
}

}

MotionException::MotionException(MotionError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MotionError MotionException::code() const noexcept { return code_; }

MotionExecutor::MotionExecutor(std::vector<MotionAxisConfig> axes,
                               std::size_t queue_capacity,
                               std::uint64_t minimum_lead_time_ns)
    : axis_configs_(std::move(axes)),
      queue_capacity_(queue_capacity),
      minimum_lead_time_ns_(minimum_lead_time_ns) {
    if (axis_configs_.empty() ||
        axis_configs_.size() > protocol::kMaximumMotionAxes ||
        queue_capacity_ == 0 || queue_capacity_ > 1024 ||
        minimum_lead_time_ns_ == 0) {
        throw MotionException(MotionError::InvalidConfiguration,
                              "运动执行器配置数量或预算无效");
    }
    axes_.reserve(axis_configs_.size());
    for (const auto& config : axis_configs_) {
        if (config.resource_id == 0 ||
            config.maximum_step_rate_hz == 0 ||
            config.step_pulse_width_ns == 0 ||
            config.minimum_step_low_ns == 0 ||
            config.direction_setup_ns == 0 ||
            !axis_indices_
                 .emplace(config.resource_id, axes_.size())
                 .second) {
            throw MotionException(MotionError::InvalidConfiguration,
                                  "运动轴配置无效或资源 ID 重复");
        }
        const std::uint64_t minimum_period =
            static_cast<std::uint64_t>(config.step_pulse_width_ns) +
            config.minimum_step_low_ns;
        if (static_cast<std::uint64_t>(
                config.maximum_step_rate_hz) *
                minimum_period >
            1000000000ULL) {
            throw MotionException(
                MotionError::InvalidConfiguration,
                "运动轴最大步频与STEP高低电平时序冲突");
        }
        axes_.push_back({config});
    }
}

MotionSegment MotionExecutor::enqueue(MotionSegment segment,
                                      std::uint64_t now_ns) {
    if (fault_ != MotionFault::None) {
        reject(MotionError::FaultLatched,
               "运动故障已锁存，必须先清除");
    }
    if (now_ns < now_ns_) {
        reject(MotionError::TimeWentBackwards,
               "运动入队时间不能倒退");
    }
    if (queue_.size() >= queue_capacity_) {
        reject(MotionError::QueueFull, "运动段队列已满");
    }
    if (!queue_.empty() && queue_.back().segment.final_segment) {
        reject(MotionError::InvalidSegment,
               "最终运动段之后不能继续入队");
    }
    if (segment.sequence == 0 ||
        segment.sequence != last_accepted_sequence_ + 1U) {
        reject(MotionError::SequenceMismatch,
               "运动段序号必须从1开始严格连续递增");
    }
    if (segment.duration_ns == 0 ||
        segment.axes.size() != axes_.size()) {
        reject(MotionError::InvalidSegment,
               "运动段持续时间或轴数量无效");
    }
    if (segment.start_time_ns == 0) {
        const auto earliest =
            now_ns > std::numeric_limits<std::uint64_t>::max() -
                         minimum_lead_time_ns_
                ? std::numeric_limits<std::uint64_t>::max()
                : now_ns + minimum_lead_time_ns_;
        segment.start_time_ns =
            std::max(earliest, next_available_time_ns_);
    } else if (
        segment.start_time_ns < now_ns ||
        segment.start_time_ns - now_ns < minimum_lead_time_ns_) {
        reject(MotionError::SegmentLate,
               "运动段没有满足最小排程提前量");
    }
    if (!queue_.empty() &&
        segment.start_time_ns != next_available_time_ns_) {
        reject(MotionError::SegmentOverlap,
               "连续运动段必须首尾无缝衔接");
    }
    if (segment.start_time_ns >
        std::numeric_limits<std::uint64_t>::max() -
            segment.duration_ns) {
        reject(MotionError::InvalidSegment,
               "运动段结束时间溢出");
    }

    std::set<std::uint32_t> seen_axes;
    SegmentRuntime runtime;
    runtime.segment = segment;
    runtime.moves.reserve(segment.axes.size());
    for (const auto& move : segment.axes) {
        if (!seen_axes.insert(move.resource_id).second) {
            reject(MotionError::DuplicateAxis,
                   "运动段包含重复轴");
        }
        const auto& axis = require_axis(move.resource_id);
        const auto count = absolute_steps(move.steps);
        if (count != 0) {
            const std::uint64_t available_after_setup =
                segment.duration_ns > axis.config.direction_setup_ns
                    ? segment.duration_ns -
                          axis.config.direction_setup_ns
                    : 0;
            const std::uint64_t minimum_period =
                static_cast<std::uint64_t>(
                    axis.config.step_pulse_width_ns) +
                axis.config.minimum_step_low_ns;
            if (available_after_setup <
                    static_cast<std::uint64_t>(
                        axis.config.step_pulse_width_ns) ||
                count >
                    available_after_setup / minimum_period + 1U ||
                !rate_allows(count, segment.duration_ns,
                             axis.config.maximum_step_rate_hz)) {
                reject(MotionError::RateExceeded,
                       "运动段超过轴步频或脉冲时序能力");
            }
        }
        runtime.moves.push_back({move, count});
    }

    next_available_time_ns_ =
        segment.start_time_ns + segment.duration_ns;
    last_accepted_sequence_ = segment.sequence;
    queue_.push_back(std::move(runtime));
    state_ = MotionState::Armed;
    ++metrics_.accepted_segments;
    metrics_.maximum_queue_depth =
        std::max(metrics_.maximum_queue_depth, queue_.size());
    return segment;
}

std::vector<MotionEdge> MotionExecutor::advance_to(
    std::uint64_t now_ns) {
    if (now_ns < now_ns_) {
        throw MotionException(MotionError::TimeWentBackwards,
                              "运动执行时间不能倒退");
    }
    std::vector<MotionEdge> output;
    while (!queue_.empty()) {
        auto& current = queue_.front();
        if (now_ns < current.segment.start_time_ns) {
            break;
        }
        state_ = MotionState::Running;
        auto edges = collect_segment_edges(current, now_ns);
        output.insert(output.end(), edges.begin(), edges.end());
        const auto end_time =
            current.segment.start_time_ns +
            current.segment.duration_ns;
        if (now_ns < end_time) {
            break;
        }

        const bool final_segment = current.segment.final_segment;
        last_completed_sequence_ = current.segment.sequence;
        queue_.pop_front();
        ++metrics_.completed_segments;
        if (final_segment) {
            auto safe =
                enter_safe_state(end_time, MotionFault::None);
            std::sort(safe.begin(), safe.end(), edge_less);
            apply_edges(safe);
            metrics_.emitted_edges += safe.size();
            output.insert(output.end(), safe.begin(), safe.end());
            break;
        }
        if (queue_.empty()) {
            ++metrics_.queue_underruns;
            auto safe = enter_safe_state(
                end_time, MotionFault::QueueUnderrun);
            std::sort(safe.begin(), safe.end(), edge_less);
            apply_edges(safe);
            metrics_.emitted_edges += safe.size();
            output.insert(output.end(), safe.begin(), safe.end());
            break;
        }
    }
    std::sort(output.begin(), output.end(), edge_less);
    now_ns_ = now_ns;
    if (queue_.empty() && fault_ == MotionFault::None &&
        state_ != MotionState::Faulted) {
        state_ = MotionState::Idle;
    }
    return output;
}

std::vector<MotionEdge> MotionExecutor::abort(
    std::uint64_t now_ns, MotionFault fault) {
    if (fault == MotionFault::None ||
        fault == MotionFault::QueueUnderrun) {
        throw MotionException(MotionError::InvalidSegment,
                              "主动停止故障原因无效");
    }
    if (fault_ != MotionFault::None) {
        throw MotionException(MotionError::FaultLatched,
                              "运动故障已经锁存");
    }
    auto output = advance_to(now_ns);
    auto safe = enter_safe_state(now_ns, fault);
    std::sort(safe.begin(), safe.end(), edge_less);
    apply_edges(safe);
    metrics_.emitted_edges += safe.size();
    output.insert(output.end(), safe.begin(), safe.end());
    std::sort(output.begin(), output.end(), edge_less);
    return output;
}

std::vector<MotionEdge> MotionExecutor::trigger_limit(
    std::uint32_t axis_resource_id, std::uint64_t now_ns) {
    static_cast<void>(require_axis(axis_resource_id));
    ++metrics_.limit_stops;
    return abort(now_ns, MotionFault::LimitTriggered);
}

void MotionExecutor::clear_fault() {
    if (!queue_.empty()) {
        throw MotionException(MotionError::FaultLatched,
                              "运动队列未清空，不能清除故障");
    }
    fault_ = MotionFault::None;
    state_ = MotionState::Idle;
    next_available_time_ns_ = now_ns_;
}

MotionStatus MotionExecutor::status() const {
    MotionStatus result;
    result.state = state_;
    result.fault = fault_;
    result.node_time_ns = now_ns_;
    result.queue_depth = queue_.size();
    result.queue_capacity = queue_capacity_;
    result.last_accepted_sequence = last_accepted_sequence_;
    result.last_completed_sequence = last_completed_sequence_;
    result.metrics = metrics_;
    result.axes.reserve(axes_.size());
    for (const auto& axis : axes_) {
        result.axes.push_back(
            {axis.config.resource_id, axis.enabled,
             axis.direction_positive, axis.step_level,
             axis.position_steps, axis.emitted_steps});
    }
    return result;
}

std::uint64_t MotionExecutor::next_available_time_ns() const noexcept {
    return next_available_time_ns_;
}

const std::vector<MotionAxisConfig>& MotionExecutor::axes() const noexcept {
    return axis_configs_;
}

[[noreturn]] void MotionExecutor::reject(MotionError code,
                                         const char* message) {
    ++metrics_.rejected_segments;
    throw MotionException(code, message);
}

MotionExecutor::AxisRuntime& MotionExecutor::require_axis(
    std::uint32_t resource_id) {
    const auto found = axis_indices_.find(resource_id);
    if (found == axis_indices_.end()) {
        reject(MotionError::InvalidAxis,
               "运动段引用了不存在的轴");
    }
    return axes_[found->second];
}

const MotionExecutor::AxisRuntime& MotionExecutor::require_axis(
    std::uint32_t resource_id) const {
    const auto found = axis_indices_.find(resource_id);
    if (found == axis_indices_.end()) {
        throw MotionException(MotionError::InvalidAxis,
                              "运动轴不存在");
    }
    return axes_[found->second];
}

std::vector<MotionEdge> MotionExecutor::collect_segment_edges(
    SegmentRuntime& runtime, std::uint64_t until_ns) {
    std::vector<MotionEdge> edges;
    const auto start = runtime.segment.start_time_ns;
    const auto end = start + runtime.segment.duration_ns;
    const auto effective_until = std::min(until_ns, end);
    for (auto& move : runtime.moves) {
        auto& axis = require_axis(move.move.resource_id);
        if (move.step_count == 0) {
            continue;
        }
        if (!move.controls_emitted && effective_until >= start) {
            if (!axis.enabled) {
                edges.push_back(
                    {start, start, move.move.resource_id,
                     MotionSignal::Enable, true});
            }
            const bool direction_positive = move.move.steps >= 0;
            if (axis.direction_positive != direction_positive) {
                edges.push_back(
                    {start, start, move.move.resource_id,
                     MotionSignal::Direction, direction_positive});
            }
            move.controls_emitted = true;
        }
        const auto first_rise =
            start + axis.config.direction_setup_ns;
        const auto last_rise =
            end - axis.config.step_pulse_width_ns;
        const auto span = last_rise - first_rise;
        while (move.rises_emitted < move.step_count) {
            const auto index = move.rises_emitted;
            const auto rise_time =
                first_rise +
                (move.step_count == 1
                     ? 0
                     : interpolate_offset(
                           span, index, move.step_count - 1U));
            if (rise_time > effective_until) {
                break;
            }
            edges.push_back(
                {rise_time, rise_time, move.move.resource_id,
                 MotionSignal::Step, true});
            ++move.rises_emitted;
        }
        while (move.falls_emitted < move.step_count) {
            const auto index = move.falls_emitted;
            const auto rise_time =
                first_rise +
                (move.step_count == 1
                     ? 0
                     : interpolate_offset(
                           span, index, move.step_count - 1U));
            const auto fall_time =
                rise_time + axis.config.step_pulse_width_ns;
            if (fall_time > effective_until) {
                break;
            }
            edges.push_back(
                {fall_time, fall_time, move.move.resource_id,
                 MotionSignal::Step, false});
            ++move.falls_emitted;
        }
    }
    std::sort(edges.begin(), edges.end(), edge_less);
    apply_edges(edges);
    metrics_.emitted_edges += edges.size();
    return edges;
}

void MotionExecutor::apply_edges(const std::vector<MotionEdge>& edges) {
    for (const auto& edge : edges) {
        auto& axis = require_axis(edge.axis_resource_id);
        switch (edge.signal) {
            case MotionSignal::Enable:
                axis.enabled = edge.level;
                break;
            case MotionSignal::Direction:
                axis.direction_positive = edge.level;
                break;
            case MotionSignal::Step:
                axis.step_level = edge.level;
                if (edge.level) {
                    axis.position_steps +=
                        axis.direction_positive ? 1 : -1;
                    ++axis.emitted_steps;
                    ++metrics_.emitted_steps;
                }
                break;
        }
    }
}

std::vector<MotionEdge> MotionExecutor::enter_safe_state(
    std::uint64_t at_ns, MotionFault fault) {
    std::vector<MotionEdge> edges;
    queue_.clear();
    next_available_time_ns_ = at_ns;
    for (const auto& axis : axes_) {
        if (axis.step_level) {
            edges.push_back(
                {at_ns, at_ns, axis.config.resource_id,
                 MotionSignal::Step, false});
        }
        if (axis.enabled) {
            edges.push_back(
                {at_ns, at_ns, axis.config.resource_id,
                 MotionSignal::Enable, false});
        }
    }
    if (fault == MotionFault::None) {
        state_ = MotionState::Idle;
        fault_ = MotionFault::None;
    } else {
        state_ = MotionState::Faulted;
        fault_ = fault;
        ++metrics_.safety_stops;
    }
    return edges;
}

}
