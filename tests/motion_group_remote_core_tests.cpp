#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/motion_executor.hpp"
#include "remotebsp/mock_mcu/time_sync_bsp.hpp"
#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/motion_group.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                       \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

constexpr std::size_t kMtu = 64U;
constexpr std::uint32_t kAxisId = 0x09000001U;
constexpr std::uint32_t kSessionId = 0x10203040U;

struct Fixture {
    std::shared_ptr<mock_mcu::MotionExecutor> motion;
    std::unique_ptr<mock_mcu::MockNode> node;
    std::uint32_t next_request_id{1U};
    std::uint16_t next_transfer_id{1U};

    explicit Fixture(bool lease_required = false) {
        motion = std::make_shared<mock_mcu::MotionExecutor>(
            std::vector<mock_mcu::MotionAxisConfig>{
                {kAxisId, 100000U, 2000U, 2000U, 2000U, 0U, 0U}},
            4U, 1000000ULL, 100000U);
        auto clock = std::make_shared<mock_mcu::MockTimeSyncBsp>(
            mock_mcu::MockTimeSyncConfig{
                101U, 1000000ULL, 64U, 500U, 20U},
            mock_mcu::TimeSyncBsp::TimePoint{});
        const std::vector<protocol::ResourceDescriptor> resources{
            {kAxisId, protocol::ResourceType::StepgenAxis, 0U,
             protocol::kResourceFlagNative, 0U, 0U}};
        const auto access_flags = static_cast<std::uint16_t>(
            protocol::kResourceAccessWritable |
            protocol::kResourceAccessExclusiveWrite |
            (lease_required
                 ? protocol::kResourceAccessLeaseSupported |
                       protocol::kResourceAccessLeaseRequired
                 : 0U));
        const std::vector<protocol::ResourceContract> contracts{
            {kAxisId, protocol::kResourceContractVersion,
             access_flags,
             1000U, 100U, 100000U, 4U, 0U, 0U}};
        mock_mcu::NodeInfo info;
        info.uuid[0] = 0x42U;
        node = std::make_unique<mock_mcu::MockNode>(
            mock_mcu::RemoteCore(
                info,
                mock_mcu::capability_mask(mock_mcu::Capability::Motion),
                nullptr, nullptr, resources, contracts, motion, nullptr,
                nullptr, nullptr, clock),
            kMtu, 7U);
    }

    protocol::Packet exchange(protocol::Command command,
                              std::vector<std::uint8_t> payload,
                              std::chrono::nanoseconds now,
                              std::uint32_t session_id = kSessionId) {
        protocol::Packet request;
        request.header.message_type = protocol::MessageType::Request;
        request.header.command = static_cast<std::uint16_t>(command);
        request.header.session_id = session_id;
        request.header.request_id = next_request_id++;
        request.payload = std::move(payload);
        const auto frames = protocol::Fragmenter(kMtu).split(
            protocol::encode(request), next_transfer_id++);
        std::optional<mock_mcu::NodeReply> reply;
        for (const auto& frame : frames) {
            const auto current = node->handle_frame(
                0x607U, frame,
                protocol::Reassembler::TimePoint{} + now);
            if (current.has_value()) {
                reply = current;
            }
        }
        if (!reply.has_value()) {
            throw std::runtime_error("Mock 节点没有返回运动组响应");
        }
        protocol::Reassembler reassembler(
            kMtu, std::chrono::milliseconds(100));
        protocol::ReassemblyResult result;
        for (const auto& frame : reply->frames) {
            result = reassembler.accept(
                0x587U, frame,
                protocol::Reassembler::TimePoint{} + now);
        }
        if (!result.packet.has_value()) {
            throw std::runtime_error("运动组响应重组失败");
        }
        return protocol::decode(*result.packet);
    }
};

protocol::MotionGroupDigest digest() {
    protocol::MotionGroupDigest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(index + 1U);
    }
    return result;
}

protocol::MotionGroupPreparePayload prepare_payload(
    std::uint64_t node_start_tick = 20500U,
    std::uint64_t host_start_time_ns = 20000000ULL) {
    // 初始 tick 为 500，1 MHz 下 20500 对应节点启动后 20 ms。
    return {{9U, 4U, 2U, 101U, 77U, node_start_tick, digest()},
            {1U, host_start_time_ns, 2000000ULL, true,
             {{kAxisId, 4}}}};
}

std::vector<std::uint8_t> response_body(
    const protocol::Packet& response,
    mock_mcu::StatusCode expected = mock_mcu::StatusCode::Ok) {
    CHECK(!response.payload.empty());
    CHECK(response.payload[0] == static_cast<std::uint8_t>(expected));
    return response.payload.empty()
               ? std::vector<std::uint8_t>{}
               : std::vector<std::uint8_t>(response.payload.begin() + 1,
                                           response.payload.end());
}

protocol::MotionGroupReadyPayload prepare(Fixture& fixture,
                                          std::chrono::nanoseconds now,
                                          protocol::MotionGroupPreparePayload
                                              payload = prepare_payload()) {
    return protocol::decode_motion_group_ready(response_body(
        fixture.exchange(
            protocol::Command::MotionGroupPrepare,
            protocol::encode_motion_group_prepare(payload), now)));
}

void test_prepare_is_side_effect_free_and_commit_executes_once() {
    Fixture fixture;
    const auto ready = prepare(fixture, std::chrono::milliseconds(1));
    CHECK(ready.code == protocol::MotionGroupReadyCode::Ready);
    const auto before_commit = fixture.motion->status();
    CHECK(before_commit.state == mock_mcu::MotionState::Idle);
    CHECK(before_commit.queue_depth == 0U);
    CHECK(before_commit.last_accepted_sequence == 0U);
    CHECK(before_commit.metrics.accepted_segments == 0U);
    CHECK(fixture.motion->advance_to(10000000ULL).empty());

    const auto duplicate_ready = prepare(
        fixture, std::chrono::milliseconds(2));
    CHECK(duplicate_ready.code == protocol::MotionGroupReadyCode::Ready);
    CHECK(fixture.motion->status().queue_depth == 0U);

    const auto commit_payload = protocol::MotionGroupCommitPayload{
        prepare_payload().identity};
    const auto ack = protocol::decode_motion_group_commit_ack(response_body(
        fixture.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(commit_payload),
            std::chrono::milliseconds(11))));
    CHECK(ack.code == protocol::MotionGroupCommitCode::Armed);
    CHECK(fixture.motion->status().state == mock_mcu::MotionState::Armed);
    CHECK(fixture.motion->status().queue_depth == 1U);
    CHECK(fixture.motion->status().metrics.accepted_segments == 1U);

    const auto duplicate_ack =
        protocol::decode_motion_group_commit_ack(response_body(
            fixture.exchange(
                protocol::Command::MotionGroupCommit,
                protocol::encode_motion_group_commit(commit_payload),
                std::chrono::milliseconds(12))));
    CHECK(duplicate_ack.code == protocol::MotionGroupCommitCode::Armed);
    CHECK(fixture.motion->status().queue_depth == 1U);
    CHECK(fixture.motion->status().metrics.accepted_segments == 1U);

    CHECK(fixture.motion->advance_to(19000000ULL).empty());
    const auto edges = fixture.motion->advance_to(22000000ULL);
    CHECK(!edges.empty());
    CHECK(fixture.motion->status().last_completed_sequence == 1U);
    CHECK(fixture.motion->status().axes[0].emitted_steps == 4U);
}

void test_conflicts_and_wrong_session_never_enqueue() {
    Fixture fixture;
    CHECK(prepare(fixture, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    auto changed = prepare_payload();
    ++changed.segment.axes[0].steps;
    const auto conflict = protocol::decode_motion_group_ready(response_body(
        fixture.exchange(
            protocol::Command::MotionGroupPrepare,
            protocol::encode_motion_group_prepare(changed),
            std::chrono::milliseconds(2))));
    CHECK(conflict.code == protocol::MotionGroupReadyCode::Busy);

    const auto wrong_session = protocol::decode_motion_group_commit_ack(
        response_body(fixture.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(
                {prepare_payload().identity}),
            std::chrono::milliseconds(3), kSessionId + 1U)));
    CHECK(wrong_session.code ==
          protocol::MotionGroupCommitCode::IdentityMismatch);
    CHECK(fixture.motion->status().queue_depth == 0U);

    auto wrong_identity = prepare_payload().identity;
    ++wrong_identity.clock_model_generation;
    const auto mismatch = protocol::decode_motion_group_commit_ack(
        response_body(fixture.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit({wrong_identity}),
            std::chrono::milliseconds(3))));
    CHECK(mismatch.code ==
          protocol::MotionGroupCommitCode::IdentityMismatch);
    CHECK(fixture.motion->status().metrics.accepted_segments == 0U);
}

void test_abort_prepared_and_armed_before_start() {
    Fixture prepared_fixture;
    CHECK(prepare(prepared_fixture, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    const auto abort_payload = protocol::MotionGroupAbortPayload{
        prepare_payload().identity,
        protocol::MotionGroupAbortReason::Cancelled};
    response_body(prepared_fixture.exchange(
        protocol::Command::MotionGroupAbort,
        protocol::encode_motion_group_abort(abort_payload),
        std::chrono::milliseconds(2)));
    CHECK(prepared_fixture.motion->status().state ==
          mock_mcu::MotionState::Idle);
    CHECK(prepared_fixture.motion->status().queue_depth == 0U);
    CHECK(prepare(prepared_fixture, std::chrono::milliseconds(3)).code ==
          protocol::MotionGroupReadyCode::Busy);
    auto next_plan = prepare_payload();
    ++next_plan.identity.transaction_id;
    ++next_plan.identity.plan_generation;
    CHECK(prepare(prepared_fixture, std::chrono::milliseconds(4),
                  next_plan)
              .code == protocol::MotionGroupReadyCode::Ready);

    Fixture armed_fixture;
    CHECK(prepare(armed_fixture, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    const auto commit = protocol::MotionGroupCommitPayload{
        prepare_payload().identity};
    CHECK(protocol::decode_motion_group_commit_ack(response_body(
              armed_fixture.exchange(
                  protocol::Command::MotionGroupCommit,
                  protocol::encode_motion_group_commit(commit),
                  std::chrono::milliseconds(2))))
              .code == protocol::MotionGroupCommitCode::Armed);
    response_body(armed_fixture.exchange(
        protocol::Command::MotionGroupAbort,
        protocol::encode_motion_group_abort(abort_payload),
        std::chrono::milliseconds(3)));
    const auto stopped = armed_fixture.motion->status();
    CHECK(stopped.state == mock_mcu::MotionState::Faulted);
    CHECK(stopped.fault == mock_mcu::MotionFault::Aborted);
    CHECK(stopped.queue_depth == 0U);
    CHECK(stopped.metrics.emitted_steps == 0U);

    // 完全相同的重复 ABORT 不重复触发执行器停止。
    response_body(armed_fixture.exchange(
        protocol::Command::MotionGroupAbort,
        protocol::encode_motion_group_abort(abort_payload),
        std::chrono::milliseconds(4)));
    CHECK(armed_fixture.motion->status().metrics.safety_stops == 1U);
}

void test_invalid_boot_epoch_and_prepare_blocks_direct_enqueue() {
    Fixture fixture;
    auto wrong_epoch = prepare_payload();
    ++wrong_epoch.identity.boot_epoch;
    const auto rejected = protocol::decode_motion_group_ready(response_body(
        fixture.exchange(
            protocol::Command::MotionGroupPrepare,
            protocol::encode_motion_group_prepare(wrong_epoch),
            std::chrono::milliseconds(1))));
    CHECK(rejected.code == protocol::MotionGroupReadyCode::ClockMismatch);
    CHECK(fixture.motion->status().queue_depth == 0U);

    CHECK(prepare(fixture, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    protocol::MotionSegmentPayload direct{
        1U, 20000000ULL, 2000000ULL, true, {{kAxisId, 4}}};
    const auto busy = fixture.exchange(
        protocol::Command::MotionEnqueue,
        protocol::encode_motion_segment(direct),
        std::chrono::milliseconds(2));
    static_cast<void>(response_body(
        busy, mock_mcu::StatusCode::ResourceBusy));
    CHECK(fixture.motion->status().queue_depth == 0U);
}

void test_ordinary_abort_and_session_release_invalidate_prepare() {
    Fixture aborted;
    CHECK(prepare(aborted, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    static_cast<void>(response_body(aborted.exchange(
        protocol::Command::MotionAbort, {},
        std::chrono::milliseconds(2))));
    const auto rejected = protocol::decode_motion_group_commit_ack(
        response_body(aborted.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(
                {prepare_payload().identity}),
            std::chrono::milliseconds(3))));
    CHECK(rejected.code == protocol::MotionGroupCommitCode::NotPrepared);
    CHECK(aborted.motion->status().queue_depth == 0U);
    CHECK(aborted.motion->status().state == mock_mcu::MotionState::Faulted);
    CHECK(aborted.motion->status().fault == mock_mcu::MotionFault::Aborted);
    static_cast<void>(response_body(aborted.exchange(
        protocol::Command::MotionClearFault, {},
        std::chrono::milliseconds(4))));
    CHECK(aborted.motion->status().state == mock_mcu::MotionState::Idle);
    CHECK(aborted.motion->status().fault == mock_mcu::MotionFault::None);
    CHECK(prepare(aborted, std::chrono::milliseconds(5)).code ==
          protocol::MotionGroupReadyCode::Ready);

    Fixture released;
    CHECK(prepare(released, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    static_cast<void>(released.node->clear_session(kSessionId));
    const auto after_release = protocol::decode_motion_group_commit_ack(
        response_body(released.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(
                {prepare_payload().identity}),
            std::chrono::milliseconds(3))));
    CHECK(after_release.code == protocol::MotionGroupCommitCode::NotPrepared);
    CHECK(released.motion->status().queue_depth == 0U);
}

void test_stepgen_lease_release_and_expiry_invalidate_prepare() {
    Fixture released(true);
    const auto lease = protocol::decode_resource_lease_info(response_body(
        released.exchange(
            protocol::Command::ResourceAcquire,
            protocol::encode_resource_lease_request(
                {kAxisId, 100U,
                 protocol::ResourceLeaseMode::Exclusive}),
            std::chrono::milliseconds(0))));
    CHECK(lease.lease_id != 0U);
    CHECK(prepare(released, std::chrono::milliseconds(1)).code ==
          protocol::MotionGroupReadyCode::Ready);
    static_cast<void>(response_body(released.exchange(
        protocol::Command::ResourceRelease,
        protocol::encode_resource_lease_token_request(
            {kAxisId, lease.lease_id, 0U}),
        std::chrono::milliseconds(2))));
    const auto after_release = protocol::decode_motion_group_commit_ack(
        response_body(released.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(
                {prepare_payload().identity}),
            std::chrono::milliseconds(3))));
    CHECK(after_release.code == protocol::MotionGroupCommitCode::Rejected);
    CHECK(released.motion->status().queue_depth == 0U);

    Fixture expired(true);
    const auto expiry_lease = protocol::decode_resource_lease_info(
        response_body(expired.exchange(
            protocol::Command::ResourceAcquire,
            protocol::encode_resource_lease_request(
                {kAxisId, 100U,
                 protocol::ResourceLeaseMode::Exclusive}),
            std::chrono::milliseconds(0))));
    CHECK(expiry_lease.lease_id != 0U);
    const auto late_prepare = prepare_payload(200500U, 200000000ULL);
    CHECK(prepare(expired, std::chrono::milliseconds(1), late_prepare).code ==
          protocol::MotionGroupReadyCode::Ready);
    CHECK(expired.node->expire(
              protocol::Reassembler::TimePoint{} +
              std::chrono::milliseconds(101)) >= 1U);
    const auto after_expiry = protocol::decode_motion_group_commit_ack(
        response_body(expired.exchange(
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit(
                {late_prepare.identity}),
            std::chrono::milliseconds(102))));
    CHECK(after_expiry.code == protocol::MotionGroupCommitCode::Rejected);
    CHECK(expired.motion->status().queue_depth == 0U);
}

}

int main() {
    test_prepare_is_side_effect_free_and_commit_executes_once();
    test_conflicts_and_wrong_session_never_enqueue();
    test_abort_prepared_and_armed_before_start();
    test_invalid_boot_epoch_and_prepare_blocks_direct_enqueue();
    test_ordinary_abort_and_session_release_invalidate_prepare();
    test_stepgen_lease_release_and_expiry_invalidate_prepare();
    if (failures != 0) {
        std::cerr << failures << " 个 Mock 运动组 RemoteCore 测试失败\n";
        return 1;
    }
    std::cout << "所有 Mock 运动组 RemoteCore 测试通过\n";
    return 0;
}
