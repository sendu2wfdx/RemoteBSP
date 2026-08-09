#include "remotebsp/protocol/motion.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using remotebsp::protocol::MotionAxisStatusPayload;
using remotebsp::protocol::MotionFaultPayload;
using remotebsp::protocol::MotionMetricsPayload;
using remotebsp::protocol::MotionPayloadException;
using remotebsp::protocol::MotionSegmentPayload;
using remotebsp::protocol::MotionStatePayload;
using remotebsp::protocol::MotionStatusPayload;

void test_segment_round_trip_and_endian() {
    const MotionSegmentPayload source{
        0x11223344U,
        0x0102030405060708ULL,
        1000000ULL,
        true,
        {{0x09000000U, std::numeric_limits<std::int32_t>::min()},
         {0x09000001U, std::numeric_limits<std::int32_t>::max()}}};
    const auto bytes =
        remotebsp::protocol::encode_motion_segment(source);
    assert(bytes.size() == 40);
    assert(bytes[0] == 0x44);
    assert(bytes[1] == 0x33);
    assert(bytes[4] == 0x08);
    assert(bytes[11] == 0x01);
    assert(bytes[20] == 1);
    assert(bytes[21] == 2);
    const auto decoded =
        remotebsp::protocol::decode_motion_segment(bytes);
    assert(decoded.sequence == source.sequence);
    assert(decoded.start_time_ns == source.start_time_ns);
    assert(decoded.duration_ns == source.duration_ns);
    assert(decoded.final_segment);
    assert(decoded.axes[0].steps ==
           std::numeric_limits<std::int32_t>::min());
    assert(decoded.axes[1].steps ==
           std::numeric_limits<std::int32_t>::max());
    const auto acceptance =
        remotebsp::protocol::decode_motion_acceptance(
            remotebsp::protocol::encode_motion_acceptance(
                {source.sequence, source.start_time_ns,
                 source.duration_ns, source.final_segment}));
    assert(acceptance.sequence == source.sequence);
    assert(acceptance.start_time_ns == source.start_time_ns);
    assert(acceptance.final_segment);
}

void test_status_round_trip() {
    MotionStatusPayload source;
    source.state = MotionStatePayload::Faulted;
    source.fault = MotionFaultPayload::TimingDeadlineMissed;
    source.node_time_ns = 123456789;
    source.queue_depth = 2;
    source.queue_capacity = 32;
    source.last_accepted_sequence = 4;
    source.last_completed_sequence = 2;
    source.metrics =
        MotionMetricsPayload{4, 1, 2, 30, 10, 1, 0, 1, 3};
    source.axes = {
        MotionAxisStatusPayload{
            0x09000000U, true, false, true, -123, 456},
        MotionAxisStatusPayload{
            0x09000001U, false, true, false, 789, 1000},
    };
    const auto decoded = remotebsp::protocol::decode_motion_status(
        remotebsp::protocol::encode_motion_status(source));
    assert(decoded.state == MotionStatePayload::Faulted);
    assert(decoded.fault == MotionFaultPayload::TimingDeadlineMissed);
    assert(decoded.queue_depth == 2);
    assert(decoded.metrics.emitted_edges == 30);
    assert(decoded.metrics.maximum_queue_depth == 3);
    assert(decoded.axes.size() == 2);
    assert(decoded.axes[0].position_steps == -123);
    assert(decoded.axes[0].step_level);
    assert(decoded.axes[1].direction_positive);
}

void test_single_node_maximum_axis_budget() {
    MotionSegmentPayload segment;
    segment.sequence = 1;
    segment.duration_ns = 1000000;
    segment.final_segment = true;
    for (std::uint32_t index = 0;
         index < remotebsp::protocol::kMaximumMotionAxes; ++index) {
        segment.axes.push_back(
            {0x09000000U + index, static_cast<std::int32_t>(index)});
    }
    const auto encoded =
        remotebsp::protocol::encode_motion_segment(segment);
    assert(encoded.size() == 536);
    assert(remotebsp::protocol::decode_motion_segment(encoded)
               .axes.size() == 64);

    MotionStatusPayload status;
    status.queue_capacity = 128;
    for (std::uint32_t index = 0;
         index < remotebsp::protocol::kMaximumMotionAxes; ++index) {
        status.axes.push_back(
            {0x09000000U + index, false, true, false,
             static_cast<std::int64_t>(index), index});
    }
    const auto status_encoded =
        remotebsp::protocol::encode_motion_status(status);
    assert(status_encoded.size() == 1628);
    assert(remotebsp::protocol::decode_motion_status(status_encoded)
               .axes.size() == 64);

    segment.axes.push_back({0x09000100U, 0});
    try {
        static_cast<void>(
            remotebsp::protocol::encode_motion_segment(segment));
        assert(false);
    } catch (const MotionPayloadException&) {
    }
}

void test_rejection() {
    try {
        static_cast<void>(
            remotebsp::protocol::decode_motion_segment(
                std::vector<std::uint8_t>(23, 0)));
        assert(false);
    } catch (const MotionPayloadException&) {
    }

    MotionSegmentPayload no_axes;
    no_axes.sequence = 1;
    no_axes.duration_ns = 1;
    try {
        static_cast<void>(
            remotebsp::protocol::encode_motion_segment(no_axes));
        assert(false);
    } catch (const MotionPayloadException&) {
    }

    MotionStatusPayload invalid;
    invalid.queue_depth = 2;
    invalid.queue_capacity = 1;
    invalid.axes.push_back({1});
    try {
        static_cast<void>(
            remotebsp::protocol::encode_motion_status(invalid));
        assert(false);
    } catch (const MotionPayloadException&) {
    }
}

}

int main() {
    test_segment_round_trip_and_endian();
    test_status_round_trip();
    test_single_node_maximum_axis_budget();
    test_rejection();
}
