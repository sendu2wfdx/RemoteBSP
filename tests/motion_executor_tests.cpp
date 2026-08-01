#include "remotebsp/mock_mcu/board_manifest.hpp"
#include "remotebsp/mock_mcu/motion_executor.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using remotebsp::mock_mcu::DigitalTwin;
using remotebsp::mock_mcu::MotionAxisConfig;
using remotebsp::mock_mcu::MotionEdge;
using remotebsp::mock_mcu::MotionError;
using remotebsp::mock_mcu::MotionException;
using remotebsp::mock_mcu::MotionExecutor;
using remotebsp::mock_mcu::MotionFault;
using remotebsp::mock_mcu::MotionSegment;
using remotebsp::mock_mcu::MotionSignal;
using remotebsp::mock_mcu::MotionState;

constexpr std::uint32_t kAxisX = 0x09000000U;
constexpr std::uint32_t kAxisY = 0x09000001U;

std::vector<MotionAxisConfig> two_axes() {
    return {
        {kAxisX, 100000, 2000, 2000, 2000},
        {kAxisY, 100000, 2000, 2000, 2000},
    };
}

MotionSegment segment(std::uint32_t sequence,
                      std::uint64_t start_ns,
                      std::uint64_t duration_ns,
                      bool final_segment,
                      std::int32_t x_steps,
                      std::int32_t y_steps) {
    return {sequence,
            start_ns,
            duration_ns,
            final_segment,
            {{kAxisX, x_steps}, {kAxisY, y_steps}}};
}

template <typename Callable>
void expect_motion_error(MotionError expected, Callable&& callable) {
    try {
        callable();
        assert(false);
    } catch (const MotionException& error) {
        assert(error.code() == expected);
    }
}

std::size_t count_edges(const std::vector<MotionEdge>& edges,
                        std::uint32_t axis,
                        MotionSignal signal,
                        bool level) {
    return static_cast<std::size_t>(std::count_if(
        edges.begin(), edges.end(), [&](const MotionEdge& edge) {
            return edge.axis_resource_id == axis &&
                   edge.signal == signal && edge.level == level;
        }));
}

void test_synchronized_multi_axis_segment() {
    MotionExecutor executor(two_axes());
    const auto accepted = executor.enqueue(
        segment(1, 10000000ULL, 1000000ULL, true, 3, -2), 0);
    assert(accepted.start_time_ns == 10000000ULL);
    assert(executor.status().state == MotionState::Armed);
    assert(executor.advance_to(9999999ULL).empty());

    const auto edges = executor.advance_to(11000000ULL);
    assert(count_edges(edges, kAxisX, MotionSignal::Step, true) == 3);
    assert(count_edges(edges, kAxisX, MotionSignal::Step, false) == 3);
    assert(count_edges(edges, kAxisY, MotionSignal::Step, true) == 2);
    assert(count_edges(edges, kAxisY, MotionSignal::Step, false) == 2);
    assert(count_edges(edges, kAxisX, MotionSignal::Enable, true) == 1);
    assert(count_edges(edges, kAxisY, MotionSignal::Enable, true) == 1);
    assert(count_edges(edges, kAxisX, MotionSignal::Enable, false) == 1);
    assert(count_edges(edges, kAxisY, MotionSignal::Enable, false) == 1);

    const auto first_x = std::find_if(
        edges.begin(), edges.end(), [](const MotionEdge& edge) {
            return edge.axis_resource_id == kAxisX &&
                   edge.signal == MotionSignal::Step && edge.level;
        });
    const auto first_y = std::find_if(
        edges.begin(), edges.end(), [](const MotionEdge& edge) {
            return edge.axis_resource_id == kAxisY &&
                   edge.signal == MotionSignal::Step && edge.level;
        });
    assert(first_x != edges.end());
    assert(first_y != edges.end());
    assert(first_x->global_time_ns == 10002000ULL);
    assert(first_x->global_time_ns == first_y->global_time_ns);
    for (const auto& edge : edges) {
        assert(edge.local_tick == edge.global_time_ns);
    }

    const auto status = executor.status();
    assert(status.state == MotionState::Idle);
    assert(status.fault == MotionFault::None);
    assert(status.last_completed_sequence == 1);
    assert(status.axes[0].position_steps == 3);
    assert(status.axes[1].position_steps == -2);
    assert(!status.axes[0].enabled);
    assert(!status.axes[1].enabled);
    assert(status.metrics.emitted_steps == 5);
}

void test_auto_append_and_queue_underrun() {
    MotionExecutor completed(two_axes());
    const auto first = completed.enqueue(
        segment(1, 0, 1000000ULL, false, 1, 0), 0);
    const auto second = completed.enqueue(
        segment(2, 0, 1000000ULL, true, 0, 1), 0);
    assert(first.start_time_ns == 1000000ULL);
    assert(second.start_time_ns == 2000000ULL);
    completed.advance_to(3000000ULL);
    assert(completed.status().state == MotionState::Idle);
    assert(completed.status().metrics.completed_segments == 2);

    MotionExecutor starved(two_axes());
    const auto only = starved.enqueue(
        segment(1, 0, 1000000ULL, false, 1, 0), 0);
    starved.advance_to(only.start_time_ns + only.duration_ns);
    const auto status = starved.status();
    assert(status.state == MotionState::Faulted);
    assert(status.fault == MotionFault::QueueUnderrun);
    assert(status.metrics.queue_underruns == 1);
    assert(status.metrics.safety_stops == 1);
}

void test_validation_and_capacity() {
    MotionExecutor executor(two_axes(), 1);
    expect_motion_error(MotionError::SequenceMismatch, [&] {
        executor.enqueue(segment(2, 0, 1000000ULL, true, 1, 0), 0);
    });
    expect_motion_error(MotionError::DuplicateAxis, [&] {
        auto duplicate = segment(1, 0, 1000000ULL, true, 1, 0);
        duplicate.axes[1].resource_id = kAxisX;
        executor.enqueue(std::move(duplicate), 0);
    });
    expect_motion_error(MotionError::InvalidAxis, [&] {
        auto invalid = segment(1, 0, 1000000ULL, true, 1, 0);
        invalid.axes[1].resource_id = 0x0900FFFFU;
        executor.enqueue(std::move(invalid), 0);
    });
    expect_motion_error(MotionError::SegmentLate, [&] {
        executor.enqueue(
            segment(1, 500000ULL, 1000000ULL, true, 1, 0), 0);
    });
    expect_motion_error(MotionError::RateExceeded, [&] {
        executor.enqueue(
            segment(1, 1000000ULL, 1000000ULL, true, 101, 0), 0);
    });

    executor.enqueue(
        segment(1, 1000000ULL, 1000000ULL, false, 1, 0), 0);
    expect_motion_error(MotionError::QueueFull, [&] {
        executor.enqueue(
            segment(2, 2000000ULL, 1000000ULL, true, 0, 1), 0);
    });

    MotionExecutor final_queue(two_axes());
    final_queue.enqueue(
        segment(1, 1000000ULL, 1000000ULL, true, 1, 0), 0);
    expect_motion_error(MotionError::InvalidSegment, [&] {
        final_queue.enqueue(
            segment(2, 2000000ULL, 1000000ULL, true, 0, 1), 0);
    });
}

void test_limit_switch_safe_stop() {
    MotionExecutor executor(two_axes());
    executor.enqueue(
        segment(1, 1000000ULL, 10000000ULL, true, 10, 0), 0);
    const auto before_stop = executor.advance_to(1002000ULL);
    assert(count_edges(before_stop, kAxisX, MotionSignal::Step, true) == 1);
    assert(executor.status().axes[0].step_level);

    const auto stop_edges = executor.trigger_limit(kAxisX, 1002000ULL);
    assert(count_edges(stop_edges, kAxisX, MotionSignal::Step, false) == 1);
    assert(count_edges(stop_edges, kAxisX, MotionSignal::Enable, false) == 1);
    const auto stopped = executor.status();
    assert(stopped.state == MotionState::Faulted);
    assert(stopped.fault == MotionFault::LimitTriggered);
    assert(stopped.queue_depth == 0);
    assert(!stopped.axes[0].step_level);
    assert(!stopped.axes[0].enabled);
    assert(stopped.metrics.limit_stops == 1);
    assert(stopped.metrics.safety_stops == 1);

    assert(executor.advance_to(20000000ULL).empty());
    assert(executor.status().axes[0].position_steps == 1);
    executor.clear_fault();
    assert(executor.status().state == MotionState::Idle);
    assert(executor.status().fault == MotionFault::None);
}

void test_digital_twin_motion_and_fault_injection() {
    auto manifest =
        remotebsp::mock_mcu::load_board_manifest(TEST_BOARD_MANIFEST);
    const auto scenario =
        remotebsp::mock_mcu::parse_fault_scenario(R"json(
            {
              "schema_version": 1,
              "events": [
                {
                  "at_ms": 3,
                  "action": "motion_limit",
                  "resource_id": 150994944
                }
              ]
            }
        )json");
    DigitalTwin twin(std::move(manifest), scenario);
    assert(twin.motion());
    twin.motion()->enqueue(
        {1,
         1000000ULL,
         10000000ULL,
         true,
         {{0x09000000U, 10},
          {0x09000001U, 0},
          {0x09000002U, 0}}},
        0);

    assert(twin.advance_to(2) == 0);
    const auto early_edges = twin.take_motion_edges();
    assert(!early_edges.empty());
    assert(twin.advance_to(5) == 1);
    const auto stop_edges = twin.take_motion_edges();
    assert(count_edges(
               stop_edges, kAxisX, MotionSignal::Enable, false) == 1);
    assert(twin.motion()->status().fault ==
           MotionFault::LimitTriggered);
}

void test_axis_count_is_board_capability_not_three() {
    std::vector<MotionAxisConfig> configs;
    MotionSegment many_axes;
    many_axes.sequence = 1;
    many_axes.start_time_ns = 1000000ULL;
    many_axes.duration_ns = 1000000ULL;
    many_axes.final_segment = true;
    for (std::uint32_t index = 0; index < 12; ++index) {
        const auto resource_id = 0x09000100U + index;
        configs.push_back({resource_id, 100000, 2000, 2000, 2000});
        many_axes.axes.push_back(
            {resource_id, static_cast<std::int32_t>(index + 1U)});
    }
    MotionExecutor executor(std::move(configs), 8);
    executor.enqueue(std::move(many_axes), 0);
    executor.advance_to(2000000ULL);
    const auto status = executor.status();
    assert(status.axes.size() == 12);
    assert(status.axes.front().position_steps == 1);
    assert(status.axes.back().position_steps == 12);
    assert(status.metrics.emitted_steps == 78);
}

}

int main() {
    test_synchronized_multi_axis_segment();
    test_auto_append_and_queue_underrun();
    test_validation_and_capacity();
    test_limit_switch_safe_stop();
    test_digital_twin_motion_and_fault_injection();
    test_axis_count_is_board_capability_not_three();
}
