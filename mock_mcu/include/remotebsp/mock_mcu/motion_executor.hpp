#pragma once

#include "remotebsp/protocol/motion.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

struct MotionAxisConfig {
    std::uint32_t resource_id{};
    std::uint32_t maximum_step_rate_hz{};
    std::uint32_t step_pulse_width_ns{};
    std::uint32_t minimum_step_low_ns{};
    std::uint32_t direction_setup_ns{};
    std::uint32_t driver_resource_id{};
    std::uint8_t driver_type{};
};

struct MotionAxisMove {
    std::uint32_t resource_id{};
    std::int32_t steps{};
};

struct MotionSegment {
    std::uint32_t sequence{};
    std::uint64_t start_time_ns{};
    std::uint64_t duration_ns{};
    bool final_segment{};
    std::vector<MotionAxisMove> axes;
};

enum class MotionSignal : std::uint8_t {
    Enable = 0,
    Direction = 1,
    Step = 2,
};

struct MotionEdge {
    std::uint64_t global_time_ns{};
    std::uint64_t local_tick{};
    std::uint32_t axis_resource_id{};
    MotionSignal signal{MotionSignal::Step};
    bool level{};
};

enum class MotionState : std::uint8_t {
    Idle = 0,
    Armed = 1,
    Running = 2,
    Faulted = 3,
};

enum class MotionFault : std::uint8_t {
    None = 0,
    Aborted = 1,
    LimitTriggered = 2,
    QueueUnderrun = 3,
    TimingDeadlineMissed = 4,
};

struct MotionAxisStatus {
    std::uint32_t resource_id{};
    bool enabled{};
    bool direction_positive{};
    bool step_level{};
    std::int64_t position_steps{};
    std::uint64_t emitted_steps{};
};

struct MotionMetrics {
    std::uint64_t accepted_segments{};
    std::uint64_t rejected_segments{};
    std::uint64_t completed_segments{};
    std::uint64_t emitted_edges{};
    std::uint64_t emitted_steps{};
    std::uint64_t safety_stops{};
    std::uint64_t limit_stops{};
    std::uint64_t queue_underruns{};
    std::size_t maximum_queue_depth{};
};

struct MotionStatus {
    MotionState state{MotionState::Idle};
    MotionFault fault{MotionFault::None};
    std::uint64_t node_time_ns{};
    std::size_t queue_depth{};
    std::size_t queue_capacity{};
    std::size_t queue_low_watermark{1U};
    bool queue_low{};
    std::uint32_t last_accepted_sequence{};
    std::uint32_t last_completed_sequence{};
    std::vector<MotionAxisStatus> axes;
    MotionMetrics metrics;
};

enum class MotionError {
    InvalidConfiguration,
    InvalidSegment,
    InvalidAxis,
    DuplicateAxis,
    SequenceMismatch,
    SegmentLate,
    SegmentOverlap,
    QueueFull,
    RateExceeded,
    FaultLatched,
    TimeWentBackwards,
};

class MotionException : public std::runtime_error {
public:
    MotionException(MotionError code, const char* message);
    MotionError code() const noexcept;

private:
    MotionError code_;
};

class MotionExecutor {
public:
    explicit MotionExecutor(std::vector<MotionAxisConfig> axes,
                            std::size_t queue_capacity = 32,
                            std::uint64_t minimum_lead_time_ns = 1000000,
                            std::uint32_t maximum_total_step_rate_hz = 0);

    MotionSegment enqueue(MotionSegment segment,
                          std::uint64_t now_ns);
    // 在有界副本上运行与 enqueue 完全相同的校验，不修改队列、序号或指标。
    MotionSegment validate_enqueue(MotionSegment segment,
                                   std::uint64_t now_ns) const;
    std::vector<MotionEdge> advance_to(std::uint64_t now_ns);
    std::vector<MotionEdge> abort(
        std::uint64_t now_ns,
        MotionFault fault = MotionFault::Aborted);
    std::vector<MotionEdge> trigger_limit(
        std::uint32_t axis_resource_id, std::uint64_t now_ns);
    void clear_fault();
    void replace_configuration(MotionExecutor&& replacement) noexcept;

    MotionStatus status() const;
    std::uint64_t next_available_time_ns() const noexcept;
    const std::vector<MotionAxisConfig>& axes() const noexcept;
    std::uint64_t minimum_lead_time_ns() const noexcept;
    std::uint32_t maximum_total_step_rate_hz() const noexcept;

private:
    struct AxisRuntime {
        MotionAxisConfig config;
        bool enabled{};
        bool direction_positive{};
        bool step_level{};
        std::int64_t position_steps{};
        std::uint64_t emitted_steps{};
    };

    struct MoveRuntime {
        MotionAxisMove move;
        std::uint64_t step_count{};
        std::uint64_t rises_emitted{};
        std::uint64_t falls_emitted{};
        bool controls_emitted{};
    };

    struct SegmentRuntime {
        MotionSegment segment;
        std::vector<MoveRuntime> moves;
    };

    [[noreturn]] void reject(MotionError code, const char* message);
    AxisRuntime& require_axis(std::uint32_t resource_id);
    const AxisRuntime& require_axis(std::uint32_t resource_id) const;
    std::vector<MotionEdge> collect_segment_edges(
        SegmentRuntime& segment, std::uint64_t until_ns);
    void apply_edges(const std::vector<MotionEdge>& edges);
    std::vector<MotionEdge> enter_safe_state(
        std::uint64_t at_ns, MotionFault fault);

    std::vector<MotionAxisConfig> axis_configs_;
    std::vector<AxisRuntime> axes_;
    std::unordered_map<std::uint32_t, std::size_t> axis_indices_;
    std::deque<SegmentRuntime> queue_;
    std::size_t queue_capacity_;
    std::uint64_t minimum_lead_time_ns_;
    std::uint32_t maximum_total_step_rate_hz_{};
    std::uint64_t now_ns_{};
    std::uint64_t next_available_time_ns_{};
    std::uint32_t last_accepted_sequence_{};
    std::uint32_t last_completed_sequence_{};
    MotionState state_{MotionState::Idle};
    MotionFault fault_{MotionFault::None};
    MotionMetrics metrics_;
};

}
