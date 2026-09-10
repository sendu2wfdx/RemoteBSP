#include "remotebsp/protocol/motion.hpp"
#include "remotebsp/protocol/motion_group.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using remotebsp::protocol::MotionAxisStatusPayload;
using remotebsp::protocol::MotionContractPayload;
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
    source.queue_low_watermark = 1;
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
    assert(decoded.queue_low_watermark == 1U);
    assert(!decoded.queue_low);
    assert(decoded.axes.size() == 2);
    assert(decoded.axes[0].position_steps == -123);
    assert(decoded.axes[0].step_level);
    assert(decoded.axes[1].direction_positive);
}

void test_contract_round_trip_and_admission() {
    MotionContractPayload source;
    source.queue_capacity = 32;
    source.minimum_lead_time_ns = 1000000ULL;
    source.maximum_total_step_rate_hz = 150000U;
    source.axes = {
        {0x09000000U, 100000U, 2000U, 2000U, 2000U},
        {0x09000001U, 100000U, 2000U, 2000U, 2000U},
    };
    const auto encoded =
        remotebsp::protocol::encode_motion_contract(source);
    assert(encoded.size() == 64U);
    assert(encoded[0] == 1U && encoded[1] == 0U);
    assert(encoded[4] == 2U && encoded[5] == 0U);
    const auto decoded =
        remotebsp::protocol::decode_motion_contract(encoded);
    assert(decoded.queue_capacity == 32U);
    assert(decoded.minimum_lead_time_ns == 1000000ULL);
    assert(decoded.maximum_total_step_rate_hz == 150000U);
    assert(decoded.axes.size() == 2U);
    assert(decoded.axes[1].resource_id == 0x09000001U);

    MotionSegmentPayload accepted{
        1U, 0U, 1000000000ULL, true,
        {{0x09000000U, 80000}, {0x09000001U, -60000}}};
    remotebsp::protocol::validate_motion_segment_against_contract(
        accepted, decoded);

    auto total_rate_exceeded = accepted;
    total_rate_exceeded.axes[0].steps = 100000;
    try {
        remotebsp::protocol::validate_motion_segment_against_contract(
            total_rate_exceeded, decoded);
        assert(false);
    } catch (const MotionPayloadException& error) {
        assert(error.code() ==
               remotebsp::protocol::MotionPayloadError::RateExceeded);
    }

    auto wrong_axis = accepted;
    wrong_axis.axes[1].resource_id = 0x09000002U;
    try {
        remotebsp::protocol::validate_motion_segment_against_contract(
            wrong_axis, decoded);
        assert(false);
    } catch (const MotionPayloadException& error) {
        assert(error.code() ==
               remotebsp::protocol::MotionPayloadError::ContractMismatch);
    }
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

    MotionContractPayload contract;
    contract.queue_capacity = 128;
    contract.minimum_lead_time_ns = 1000000ULL;
    contract.maximum_total_step_rate_hz = 500000U;
    for (std::uint32_t index = 0;
         index < remotebsp::protocol::kMaximumMotionAxes; ++index) {
        contract.axes.push_back(
            {0x09000000U + index, 100000U,
             2000U, 2000U, 2000U});
    }
    const auto contract_encoded =
        remotebsp::protocol::encode_motion_contract(contract);
    assert(contract_encoded.size() == 1304U);
    assert(remotebsp::protocol::decode_motion_contract(contract_encoded)
               .axes.size() == 64U);

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

    MotionStatusPayload warning;
    warning.state = MotionStatePayload::Armed;
    warning.queue_depth = 1U;
    warning.queue_capacity = 4U;
    warning.queue_low_watermark = 1U;
    warning.queue_low = true;
    warning.axes.push_back({1});
    const auto decoded_warning =
        remotebsp::protocol::decode_motion_status(
            remotebsp::protocol::encode_motion_status(warning));
    assert(decoded_warning.queue_low);

    warning.queue_depth = 2U;
    try {
        static_cast<void>(
            remotebsp::protocol::encode_motion_status(warning));
        assert(false);
    } catch (const MotionPayloadException&) {
    }
}

void test_group_transaction_payloads() {
    using namespace remotebsp::protocol;
    MotionGroupDigest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        digest[index] = static_cast<std::uint8_t>(index + 1U);
    }
    const MotionGroupIdentityPayload identity{
        0x0102030405060708ULL, 5U, 7U, 9U, 11U, 123456U, digest};
    const MotionSegmentPayload segment{
        3U, 5000000000ULL, 1000000ULL, true,
        {{0x09000001U, -20}}};

    const auto prepare = decode_motion_group_prepare(
        encode_motion_group_prepare({identity, segment}));
    assert(prepare.identity.transaction_id == identity.transaction_id);
    assert(prepare.identity.clock_model_generation == 11U);
    assert(prepare.identity.node_start_tick == 123456U);
    assert(prepare.identity.content_digest == digest);
    assert(prepare.segment.axes[0].steps == -20);

    const auto ready = decode_motion_group_ready(
        encode_motion_group_ready({identity, MotionGroupReadyCode::Ready}));
    assert(ready.code == MotionGroupReadyCode::Ready);
    assert(ready.identity.boot_epoch == 9U);
    const auto commit = decode_motion_group_commit(
        encode_motion_group_commit({identity}));
    assert(commit.identity.node_start_tick == 123456U);
    const auto ack = decode_motion_group_commit_ack(
        encode_motion_group_commit_ack(
            {identity, MotionGroupCommitCode::Armed}));
    assert(ack.code == MotionGroupCommitCode::Armed);
    const auto abort = decode_motion_group_abort(
        encode_motion_group_abort(
            {identity, MotionGroupAbortReason::CommitTimedOut}));
    assert(abort.reason == MotionGroupAbortReason::CommitTimedOut);

    auto corrupt = encode_motion_group_ready(
        {identity, MotionGroupReadyCode::Ready});
    corrupt[81U] = 1U;
    try {
        static_cast<void>(decode_motion_group_ready(corrupt));
        assert(false);
    } catch (const MotionGroupPayloadException&) {
    }
    auto zero_digest = identity;
    zero_digest.content_digest.fill(0U);
    try {
        static_cast<void>(encode_motion_group_commit({zero_digest}));
        assert(false);
    } catch (const MotionGroupPayloadException&) {
    }
}

}

int main() {
    test_segment_round_trip_and_endian();
    test_status_round_trip();
    test_contract_round_trip_and_admission();
    test_single_node_maximum_axis_budget();
    test_rejection();
    test_group_transaction_payloads();
}
