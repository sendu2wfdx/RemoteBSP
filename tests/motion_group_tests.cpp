#include "remotebsp/mock_mcu/motion_group_participant.hpp"
#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/motion_group.hpp"
#include "remotebsp/toolbusd/motion_group_coordinator.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

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

protocol::NodeIdentity identity(std::uint8_t seed) {
    protocol::NodeIdentity result;
    for (std::size_t index = 0U; index < result.uuid.size(); ++index) {
        result.uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    result.protocol_version = protocol::kProtocolVersion;
    result.board_type = 0x1000U + seed;
    return result;
}

protocol::Packet discovery(const protocol::NodeIdentity& source) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Response;
    packet.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryResponse);
    packet.payload = protocol::encode_node_identity(source);
    return packet;
}

protocol::Packet heartbeat(const protocol::NodeIdentity& source,
                           std::uint32_t node_id) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Event;
    packet.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    packet.header.object_id = node_id;
    packet.payload = protocol::encode_heartbeat(
        {source.uuid, source.protocol_version});
    return packet;
}

toolbusd::ClockModelConfig clock_config() {
    toolbusd::ClockModelConfig config;
    config.window_size = 8U;
    config.low_rtt_sample_count = 4U;
    config.minimum_samples = 4U;
    config.minimum_fit_span_ns = 5000000ULL;
    config.maximum_round_trip_ns = 1000000ULL;
    config.synchronized_max_age_ns = 100000000ULL;
    config.model_expiry_ns = 200000000ULL;
    config.maximum_error_bound_ns = 300000ULL;
    config.minimum_drift_uncertainty_ppm = 1U;
    return config;
}

toolbusd::FourTimestampSample sample(std::size_t index,
                                     std::uint64_t offset_ticks) {
    constexpr std::uint64_t base = 1000000000ULL;
    constexpr std::uint64_t spacing = 2000000ULL;
    const auto send = base + static_cast<std::uint64_t>(index) * spacing;
    const auto receive_tick = (send + 100000ULL) / 1000ULL + offset_ticks;
    return {send, receive_tick, receive_tick + 20ULL, send + 220000ULL};
}

void add_node(toolbusd::NodeRegistry& registry, std::uint32_t node_id,
              std::uint8_t seed, std::uint64_t boot_epoch,
              std::uint64_t offset_ticks) {
    const auto info = identity(seed);
    CHECK(registry.accept_discovery_response(discovery(info)) ==
          toolbusd::NodeUpdate::Added);
    static_cast<void>(registry.make_node_assignment(
        info.uuid, node_id, node_id));
    CHECK(registry.accept_heartbeat(heartbeat(info, node_id)));
    CHECK(registry.register_clock_model(node_id, boot_epoch, clock_config()) ==
          toolbusd::NodeClockRegistrationResult::Registered);
    for (std::size_t index = 0U; index < 6U; ++index) {
        CHECK(registry.add_clock_sample(
                  node_id, boot_epoch, sample(index, offset_ticks)).status ==
              toolbusd::NodeClockAccessStatus::Ready);
    }
}

toolbusd::MotionGroupCoordinatorConfig coordinator_config() {
    toolbusd::MotionGroupCoordinatorConfig config;
    config.maximum_members = 4U;
    config.prepare_timeout = std::chrono::milliseconds(100);
    config.commit_timeout = std::chrono::milliseconds(50);
    config.abort_settle_timeout = std::chrono::milliseconds(20);
    config.minimum_commit_guard_ns = 1000000ULL;
    config.maximum_clock_error_ns = 300000ULL;
    return config;
}

protocol::MotionGroupDigest digest() {
    protocol::MotionGroupDigest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(0x80U + index);
    }
    return result;
}

toolbusd::MotionGroupPlan plan() {
    return {99U,
            42U,
            3U,
            1020000000ULL,
            digest(),
            {{1U, {1U, 1020000000ULL, 1000000ULL, true,
                   {{0x09000001U, 10}}}},
             {2U, {1U, 1020000000ULL, 1000000ULL, true,
                   {{0x09000002U, -12}}}}}};
}

struct Fixture {
    toolbusd::NodeRegistry registry;
    toolbusd::MotionGroupCoordinator coordinator{coordinator_config()};
    mock_mcu::MockMotionGroupParticipant first;
    mock_mcu::MockMotionGroupParticipant second;

    Fixture() {
        add_node(registry, 1U, 1U, 101U, 500U);
        add_node(registry, 2U, 30U, 202U, 900U);
    }
};

void test_all_ready_before_atomic_commit_batch_and_frozen_clock() {
    Fixture fixture;
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    const auto prepare_actions =
        fixture.coordinator.begin(plan(), fixture.registry,
                                  1011000000ULL, start);
    CHECK(prepare_actions.size() == 2U);
    CHECK(fixture.coordinator.state() ==
          toolbusd::MotionGroupState::Preparing);
    const auto first_generation =
        fixture.coordinator.frozen_members()[0].clock_model_generation;
    const auto first_tick =
        fixture.coordinator.frozen_members()[0].node_start_tick;

    auto first_prepare = protocol::decode_motion_group_prepare(
        prepare_actions[0].payload);
    auto second_prepare = protocol::decode_motion_group_prepare(
        prepare_actions[1].payload);
    const auto first_ready = fixture.first.prepare(first_prepare);
    const auto first_outcome = fixture.coordinator.accept_ready(
        prepare_actions[0].node_id, first_ready,
        start + std::chrono::milliseconds(10));
    CHECK(first_outcome.status == toolbusd::MotionGroupEventStatus::Accepted);
    CHECK(first_outcome.actions.empty());
    CHECK(fixture.coordinator.commit(
              fixture.registry, 1012000000ULL,
              start + std::chrono::milliseconds(11)).status ==
          toolbusd::MotionGroupEventStatus::InvalidState);

    const auto second_ready = fixture.second.prepare(second_prepare);
    CHECK(fixture.coordinator.accept_ready(
              prepare_actions[1].node_id, second_ready,
              start + std::chrono::milliseconds(12)).status ==
          toolbusd::MotionGroupEventStatus::AllReady);
    CHECK(fixture.coordinator.accept_ready(
              prepare_actions[0].node_id, first_ready,
              start + std::chrono::milliseconds(12)).status ==
          toolbusd::MotionGroupEventStatus::Duplicate);

    // 后续同步样本推进活动模型代次；事务必须继续使用冻结代次和 tick。
    CHECK(fixture.registry.add_clock_sample(
              1U, 101U, sample(6U, 500U)).status ==
          toolbusd::NodeClockAccessStatus::Ready);
    CHECK(fixture.registry.clock_model_generation(1U).has_value());
    CHECK(*fixture.registry.clock_model_generation(1U) != first_generation);

    const auto commits = fixture.coordinator.commit(
        fixture.registry, 1013000000ULL,
        start + std::chrono::milliseconds(13));
    CHECK(commits.status == toolbusd::MotionGroupEventStatus::Accepted);
    CHECK(commits.actions.size() == 2U);
    CHECK(commits.actions[0].phase ==
          toolbusd::MotionGroupActionPhase::Commit);
    const auto first_commit = protocol::decode_motion_group_commit(
        commits.actions[0].payload);
    CHECK(first_commit.identity.clock_model_generation == first_generation);
    CHECK(first_commit.identity.node_start_tick == first_tick);

    const auto first_ack = fixture.first.commit(first_commit);
    CHECK(fixture.coordinator.accept_commit_ack(
              commits.actions[0].node_id, first_ack,
              start + std::chrono::milliseconds(14)).status ==
          toolbusd::MotionGroupEventStatus::Accepted);
    const auto second_commit = protocol::decode_motion_group_commit(
        commits.actions[1].payload);
    const auto second_ack = fixture.second.commit(second_commit);
    CHECK(fixture.coordinator.accept_commit_ack(
              commits.actions[1].node_id, second_ack,
              start + std::chrono::milliseconds(15)).status ==
          toolbusd::MotionGroupEventStatus::AllCommitted);
    CHECK(fixture.coordinator.accept_commit_ack(
              commits.actions[0].node_id, first_ack,
              start + std::chrono::milliseconds(16)).status ==
          toolbusd::MotionGroupEventStatus::Duplicate);
    CHECK(fixture.coordinator.state() ==
          toolbusd::MotionGroupState::Committed);
}

void test_prepare_rejection_never_emits_commit() {
    Fixture fixture;
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    const auto actions = fixture.coordinator.begin(
        plan(), fixture.registry, 1011000000ULL, start);
    const auto first_prepare = protocol::decode_motion_group_prepare(
        actions[0].payload);
    const auto rejected = fixture.first.prepare(
        first_prepare, protocol::MotionGroupReadyCode::ResourceUnavailable);
    const auto outcome = fixture.coordinator.accept_ready(
        actions[0].node_id, rejected, start + std::chrono::milliseconds(1));
    CHECK(outcome.status == toolbusd::MotionGroupEventStatus::Rejected);
    CHECK(outcome.actions.size() == 2U);
    for (const auto& action : outcome.actions) {
        CHECK(action.phase == toolbusd::MotionGroupActionPhase::Abort);
        CHECK(action.command == protocol::Command::MotionGroupAbort);
    }
    CHECK(fixture.coordinator.state() ==
          toolbusd::MotionGroupState::Aborting);
}

void test_identity_mismatch_cancel_and_prepare_timeout() {
    Fixture fixture;
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    const auto actions = fixture.coordinator.begin(
        plan(), fixture.registry, 1011000000ULL, start);
    auto ready = fixture.first.prepare(
        protocol::decode_motion_group_prepare(actions[0].payload));
    ++ready.identity.clock_model_generation;
    const auto mismatch = fixture.coordinator.accept_ready(
        actions[0].node_id, ready, start + std::chrono::milliseconds(1));
    CHECK(mismatch.status ==
          toolbusd::MotionGroupEventStatus::IdentityMismatch);
    CHECK(mismatch.actions.size() == 2U);
    CHECK(fixture.coordinator.poll(
              start + std::chrono::milliseconds(22)).status ==
          toolbusd::MotionGroupEventStatus::Settled);
    CHECK(fixture.coordinator.state() == toolbusd::MotionGroupState::Aborted);

    fixture.coordinator.reset();
    static_cast<void>(fixture.coordinator.begin(
        plan(), fixture.registry, 1011000000ULL, start));
    const auto cancelled = fixture.coordinator.cancel(
        start + std::chrono::milliseconds(2));
    CHECK(cancelled.status == toolbusd::MotionGroupEventStatus::Cancelled);
    CHECK(cancelled.actions.size() == 2U);

    Fixture timeout_fixture;
    static_cast<void>(timeout_fixture.coordinator.begin(
        plan(), timeout_fixture.registry, 1011000000ULL, start));
    const auto timed_out = timeout_fixture.coordinator.poll(
        start + std::chrono::milliseconds(100));
    CHECK(timed_out.status == toolbusd::MotionGroupEventStatus::TimedOut);
    CHECK(timed_out.actions.size() == 2U);
    CHECK(timeout_fixture.coordinator.abort_reason() ==
          protocol::MotionGroupAbortReason::PrepareTimedOut);
}

void make_all_ready(Fixture& fixture,
                    toolbusd::MotionGroupCoordinator::TimePoint start) {
    const auto actions = fixture.coordinator.begin(
        plan(), fixture.registry, 1011000000ULL, start);
    CHECK(fixture.coordinator.accept_ready(
              actions[0].node_id,
              fixture.first.prepare(protocol::decode_motion_group_prepare(
                  actions[0].payload)),
              start + std::chrono::milliseconds(1)).status ==
          toolbusd::MotionGroupEventStatus::Accepted);
    CHECK(fixture.coordinator.accept_ready(
              actions[1].node_id,
              fixture.second.prepare(protocol::decode_motion_group_prepare(
                  actions[1].payload)),
              start + std::chrono::milliseconds(2)).status ==
          toolbusd::MotionGroupEventStatus::AllReady);
}

void test_restart_and_commit_failure_abort_whole_group() {
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    Fixture restarted;
    make_all_ready(restarted, start);
    CHECK(restarted.registry.register_clock_model(
              2U, 203U, clock_config()) ==
          toolbusd::NodeClockRegistrationResult::ReplacedBootEpoch);
    const auto restart_outcome = restarted.coordinator.commit(
        restarted.registry, 1012000000ULL,
        start + std::chrono::milliseconds(3));
    CHECK(restart_outcome.status == toolbusd::MotionGroupEventStatus::Rejected);
    CHECK(restart_outcome.actions.size() == 2U);
    CHECK(restarted.coordinator.abort_reason() ==
          protocol::MotionGroupAbortReason::NodeRestarted);

    Fixture rejected;
    make_all_ready(rejected, start);
    const auto commits = rejected.coordinator.commit(
        rejected.registry, 1012000000ULL,
        start + std::chrono::milliseconds(3));
    CHECK(commits.actions.size() == 2U);
    const auto first_commit = protocol::decode_motion_group_commit(
        commits.actions[0].payload);
    const auto negative_ack = rejected.first.commit(
        first_commit, protocol::MotionGroupCommitCode::Rejected);
    const auto failure = rejected.coordinator.accept_commit_ack(
        commits.actions[0].node_id, negative_ack,
        start + std::chrono::milliseconds(4));
    CHECK(failure.status == toolbusd::MotionGroupEventStatus::Rejected);
    CHECK(failure.actions.size() == 2U);
    CHECK(rejected.coordinator.abort_reason() ==
          protocol::MotionGroupAbortReason::CommitRejected);
}

void test_commit_timeout_aborts_every_member() {
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    Fixture fixture;
    make_all_ready(fixture, start);
    const auto commits = fixture.coordinator.commit(
        fixture.registry, 1012000000ULL,
        start + std::chrono::milliseconds(3));
    CHECK(commits.actions.size() == 2U);
    const auto timed_out = fixture.coordinator.poll(
        start + std::chrono::milliseconds(53));
    CHECK(timed_out.status == toolbusd::MotionGroupEventStatus::TimedOut);
    CHECK(timed_out.actions.size() == 2U);
    CHECK(fixture.coordinator.abort_reason() ==
          protocol::MotionGroupAbortReason::CommitTimedOut);
}

void test_mock_idempotency_and_begin_failure_is_atomic() {
    Fixture fixture;
    const auto start = toolbusd::MotionGroupCoordinator::TimePoint{};
    auto invalid = plan();
    invalid.members.push_back(invalid.members.front());
    try {
        static_cast<void>(fixture.coordinator.begin(
            invalid, fixture.registry, 1011000000ULL, start));
        CHECK(false);
    } catch (const toolbusd::MotionGroupCoordinatorException& error) {
        CHECK(error.code() == toolbusd::MotionGroupCoordinatorError::InvalidPlan);
    }
    CHECK(fixture.coordinator.state() == toolbusd::MotionGroupState::Idle);
    CHECK(fixture.coordinator.frozen_members().empty());

    const auto actions = fixture.coordinator.begin(
        plan(), fixture.registry, 1011000000ULL, start);
    const auto prepare = protocol::decode_motion_group_prepare(
        actions[0].payload);
    CHECK(fixture.first.prepare(prepare).code ==
          protocol::MotionGroupReadyCode::Ready);
    CHECK(fixture.first.prepare(prepare).code ==
          protocol::MotionGroupReadyCode::Ready);
    auto changed_content = prepare;
    ++changed_content.segment.axes[0].steps;
    CHECK(fixture.first.prepare(changed_content).code ==
          protocol::MotionGroupReadyCode::Busy);
    auto wrong = prepare;
    ++wrong.identity.plan_generation;
    CHECK(fixture.first.prepare(wrong).code ==
          protocol::MotionGroupReadyCode::Busy);
}

void test_group_traffic_classes() {
    protocol::Packet packet;
    packet.header.command = static_cast<std::uint16_t>(
        protocol::Command::MotionGroupPrepare);
    CHECK(toolbusd::classify_traffic(packet) ==
          toolbusd::TrafficClass::Motion);
    packet.header.command = static_cast<std::uint16_t>(
        protocol::Command::MotionGroupCommit);
    CHECK(toolbusd::classify_traffic(packet) ==
          toolbusd::TrafficClass::Motion);
    packet.header.command = static_cast<std::uint16_t>(
        protocol::Command::MotionGroupAbort);
    CHECK(toolbusd::classify_traffic(packet) ==
          toolbusd::TrafficClass::Safety);
}

}

int main() {
    test_all_ready_before_atomic_commit_batch_and_frozen_clock();
    test_prepare_rejection_never_emits_commit();
    test_identity_mismatch_cancel_and_prepare_timeout();
    test_restart_and_commit_failure_abort_whole_group();
    test_commit_timeout_aborts_every_member();
    test_mock_idempotency_and_begin_failure_is_atomic();
    test_group_traffic_classes();
    if (failures != 0) {
        std::cerr << failures << " 个跨板运动组测试失败\n";
        return 1;
    }
    std::cout << "所有跨板运动组测试通过\n";
    return 0;
}
