#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/motion_executor.hpp"
#include "remotebsp/mock_mcu/time_sync_bsp.hpp"
#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/toolbusd/motion_group_service.hpp"
#include "remotebsp/toolbusd/motion_group_dispatch_gate.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <condition_variable>
#include <memory>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <thread>

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
constexpr std::uint32_t kSessionId = 0x55667788U;
constexpr std::uint32_t kFirstAxis = 0x09000001U;
constexpr std::uint32_t kSecondAxis = 0x09000002U;
constexpr std::uint64_t kHostNowNs = 1012000000ULL;
constexpr std::uint64_t kHostStartNs = 1040000000ULL;

protocol::NodeIdentity identity(std::uint8_t seed) {
    protocol::NodeIdentity result;
    for (std::size_t index = 0U; index < result.uuid.size(); ++index) {
        result.uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    result.protocol_version = protocol::kProtocolVersion;
    result.board_type = 0x2000U + seed;
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

toolbusd::MotionGroupServiceConfig service_config() {
    toolbusd::MotionGroupServiceConfig config;
    config.coordinator.maximum_members = 4U;
    config.coordinator.prepare_timeout = std::chrono::milliseconds(100);
    config.coordinator.commit_timeout = std::chrono::milliseconds(50);
    config.coordinator.abort_settle_timeout = std::chrono::milliseconds(20);
    config.coordinator.minimum_commit_guard_ns = 1000000ULL;
    config.coordinator.maximum_clock_error_ns = 300000ULL;
    config.maximum_pending_routes = 12U;
    config.maximum_completed_routes = 24U;
    return config;
}

protocol::MotionGroupDigest digest() {
    protocol::MotionGroupDigest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(0x30U + index);
    }
    return result;
}

toolbusd::MotionGroupPlan plan() {
    return {7001U,
            91U,
            5U,
            kHostStartNs,
            digest(),
            {{1U, {1U, kHostStartNs, 1000000ULL, true,
                   {{kFirstAxis, 10}}}},
             {2U, {1U, kHostStartNs, 1000000ULL, true,
                   {{kSecondAxis, -12}}}}}};
}

struct Fixture {
    toolbusd::NodeRegistry registry;
    toolbusd::RequestManager requests;
    toolbusd::MotionGroupService service{service_config()};
    std::shared_ptr<mock_mcu::MotionExecutor> first_motion;
    std::shared_ptr<mock_mcu::MotionExecutor> second_motion;
    std::unique_ptr<mock_mcu::MockNode> first_node;
    std::unique_ptr<mock_mcu::MockNode> second_node;
    std::uint16_t first_transfer{1U};
    std::uint16_t second_transfer{1U};

    explicit Fixture(unsigned maximum_retries = 1U)
        : requests(toolbusd::RequestManagerConfig{
              std::chrono::milliseconds(10), maximum_retries,
              std::chrono::milliseconds(100)}) {
        add_node(registry, 1U, 1U, 101U, 500U);
        add_node(registry, 2U, 30U, 202U, 900U);
        first_motion = std::make_shared<mock_mcu::MotionExecutor>(
            std::vector<mock_mcu::MotionAxisConfig>{
                {kFirstAxis, 100000U, 2000U, 2000U, 2000U, 0U, 0U}},
            4U, 1000000ULL, 100000U);
        second_motion = std::make_shared<mock_mcu::MotionExecutor>(
            std::vector<mock_mcu::MotionAxisConfig>{
                {kSecondAxis, 100000U, 2000U, 2000U, 2000U, 0U, 0U}},
            4U, 1000000ULL, 100000U);
        first_node = make_node(1U, 1U, 101U, 500U, first_motion);
        second_node = make_node(2U, 30U, 202U, 900U, second_motion);
    }

    static std::unique_ptr<mock_mcu::MockNode> make_node(
        std::uint32_t node_id, std::uint8_t seed,
        std::uint64_t boot_epoch, std::uint64_t initial_tick,
        const std::shared_ptr<mock_mcu::MotionExecutor>& motion) {
        const auto axis = node_id == 1U ? kFirstAxis : kSecondAxis;
        auto clock = std::make_shared<mock_mcu::MockTimeSyncBsp>(
            mock_mcu::MockTimeSyncConfig{
                boot_epoch, 1000000ULL, 64U, initial_tick, 20U},
            mock_mcu::TimeSyncBsp::TimePoint{});
        const std::vector<protocol::ResourceDescriptor> resources{
            {axis, protocol::ResourceType::StepgenAxis, 0U,
             protocol::kResourceFlagNative, 0U, 0U}};
        const std::vector<protocol::ResourceContract> contracts{
            {axis, protocol::kResourceContractVersion,
             protocol::kResourceAccessWritable |
                 protocol::kResourceAccessExclusiveWrite,
             1000U, 100U, 100000U, 4U, 0U, 0U}};
        mock_mcu::NodeInfo info;
        info.uuid = identity(seed).uuid;
        return std::make_unique<mock_mcu::MockNode>(
            mock_mcu::RemoteCore(
                info,
                mock_mcu::capability_mask(mock_mcu::Capability::Motion),
                nullptr, nullptr, resources, contracts, motion, nullptr,
                nullptr, nullptr, clock),
            kMtu, node_id);
    }

    protocol::Packet exchange(const toolbusd::MotionGroupDispatch& dispatch,
                              std::uint64_t host_time_ns) {
        auto& node = dispatch.node_id == 1U ? *first_node : *second_node;
        auto& transfer = dispatch.node_id == 1U
                             ? first_transfer
                             : second_transfer;
        const auto frames = protocol::Fragmenter(kMtu).split(
            protocol::encode(dispatch.submission.packet), transfer++);
        std::optional<mock_mcu::NodeReply> reply;
        const auto now = protocol::Reassembler::TimePoint{} +
                         std::chrono::nanoseconds(host_time_ns);
        for (const auto& frame : frames) {
            const auto current = node.handle_frame(
                0x600U + dispatch.node_id, frame, now);
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
                0x580U + dispatch.node_id, frame, now);
        }
        if (!result.packet.has_value()) {
            throw std::runtime_error("Mock 运动组响应重组失败");
        }
        return protocol::decode(*result.packet);
    }
};

const toolbusd::MotionGroupDispatch& dispatch_for(
    const std::vector<toolbusd::MotionGroupDispatch>& dispatches,
    std::uint32_t node_id) {
    for (const auto& dispatch : dispatches) {
        if (dispatch.node_id == node_id) {
            return dispatch;
        }
    }
    throw std::runtime_error("找不到目标节点的运动组动作");
}

void test_two_remote_cores_complete_prepare_and_commit() {
    Fixture fixture;
    const auto time_zero = toolbusd::RequestManager::TimePoint{};
    const auto started = fixture.service.start(
        fixture.requests, plan(), fixture.registry, kSessionId,
        kHostNowNs, time_zero);
    CHECK(started.status == toolbusd::MotionGroupServiceStatus::Started);
    CHECK(started.dispatches.size() == 2U);
    CHECK(fixture.requests.pending_count() == 2U);
    CHECK(!fixture.service.snapshot().commit_dispatched);

    const auto first_ready = fixture.exchange(
        dispatch_for(started.dispatches, 1U), kHostNowNs + 1000000ULL);
    const auto first = fixture.service.accept_response(
        fixture.requests, fixture.registry, 1U, first_ready,
        kHostNowNs + 1000000ULL, time_zero + std::chrono::milliseconds(1));
    CHECK(first.status == toolbusd::MotionGroupServiceStatus::Accepted);
    CHECK(first.dispatches.empty());
    CHECK(fixture.first_motion->status().queue_depth == 0U);
    CHECK(!fixture.service.snapshot().commit_dispatched);

    const auto second_ready = fixture.exchange(
        dispatch_for(started.dispatches, 2U), kHostNowNs + 2000000ULL);
    const auto all_ready = fixture.service.accept_response(
        fixture.requests, fixture.registry, 2U, second_ready,
        kHostNowNs + 2000000ULL, time_zero + std::chrono::milliseconds(2));
    CHECK(all_ready.status == toolbusd::MotionGroupServiceStatus::AllReady);
    CHECK(all_ready.dispatches.size() == 2U);
    CHECK(fixture.service.snapshot().commit_dispatched);
    for (const auto& dispatch : all_ready.dispatches) {
        CHECK(dispatch.phase == toolbusd::MotionGroupActionPhase::Commit);
        CHECK(dispatch.submission.packet.header.command ==
              static_cast<std::uint16_t>(
                  protocol::Command::MotionGroupCommit));
    }

    const auto first_ack = fixture.exchange(
        dispatch_for(all_ready.dispatches, 1U), kHostNowNs + 3000000ULL);
    const auto first_commit = fixture.service.accept_response(
        fixture.requests, fixture.registry, 1U, first_ack,
        kHostNowNs + 3000000ULL, time_zero + std::chrono::milliseconds(3));
    CHECK(first_commit.status ==
          toolbusd::MotionGroupServiceStatus::Accepted);
    const auto second_ack = fixture.exchange(
        dispatch_for(all_ready.dispatches, 2U), kHostNowNs + 4000000ULL);
    const auto completed = fixture.service.accept_response(
        fixture.requests, fixture.registry, 2U, second_ack,
        kHostNowNs + 4000000ULL, time_zero + std::chrono::milliseconds(4));
    CHECK(completed.status ==
          toolbusd::MotionGroupServiceStatus::AllCommitted);
    CHECK(fixture.service.state() == toolbusd::MotionGroupState::Committed);
    CHECK(fixture.service.pending_route_count() == 0U);
    CHECK(fixture.requests.pending_count() == 0U);
    CHECK(fixture.first_motion->status().queue_depth == 1U);
    CHECK(fixture.second_motion->status().queue_depth == 1U);

    const auto late = fixture.service.accept_response(
        fixture.requests, fixture.registry, 1U, first_ready,
        kHostNowNs + 5000000ULL, time_zero + std::chrono::milliseconds(5));
    CHECK(late.status == toolbusd::MotionGroupServiceStatus::Duplicate);
}

void test_commit_failure_reports_best_effort_abort_boundary() {
    Fixture fixture;
    const auto time_zero = toolbusd::RequestManager::TimePoint{};
    const auto started = fixture.service.start(
        fixture.requests, plan(), fixture.registry, kSessionId,
        kHostNowNs, time_zero);
    for (std::uint32_t node_id = 1U; node_id <= 2U; ++node_id) {
        const auto response = fixture.exchange(
            dispatch_for(started.dispatches, node_id),
            kHostNowNs + node_id * 1000000ULL);
        const auto ready = fixture.service.accept_response(
            fixture.requests, fixture.registry, node_id, response,
            kHostNowNs + node_id * 1000000ULL,
            time_zero + std::chrono::milliseconds(node_id));
        if (node_id == 2U) {
            CHECK(ready.dispatches.size() == 2U);
            const auto first_ack = fixture.exchange(
                dispatch_for(ready.dispatches, 1U),
                kHostNowNs + 3000000ULL);
            static_cast<void>(fixture.service.accept_response(
                fixture.requests, fixture.registry, 1U, first_ack,
                kHostNowNs + 3000000ULL,
                time_zero + std::chrono::milliseconds(3)));
            auto second_ack = fixture.exchange(
                dispatch_for(ready.dispatches, 2U),
                kHostNowNs + 4000000ULL);
            second_ack.header.flags |= protocol::kErrorResponseFlag;
            second_ack.payload = {
                static_cast<std::uint8_t>(
                    mock_mcu::StatusCode::ResourceFailed)};
            const auto failed = fixture.service.accept_response(
                fixture.requests, fixture.registry, 2U, second_ack,
                kHostNowNs + 4000000ULL,
                time_zero + std::chrono::milliseconds(4));
            CHECK(failed.status ==
                  toolbusd::MotionGroupServiceStatus::Aborting);
            const auto snapshot = fixture.service.snapshot();
            CHECK(snapshot.commit_dispatched);
            CHECK(snapshot.abort_is_best_effort);
            CHECK(snapshot.committed_count == 1U);
        }
    }
}

void test_one_prepare_failure_never_dispatches_commit() {
    Fixture fixture;
    static_cast<void>(fixture.second_motion->abort(0U));
    const auto time_zero = toolbusd::RequestManager::TimePoint{};
    const auto started = fixture.service.start(
        fixture.requests, plan(), fixture.registry, kSessionId,
        kHostNowNs, time_zero);
    const auto first_ready = fixture.exchange(
        dispatch_for(started.dispatches, 1U), kHostNowNs + 1000000ULL);
    static_cast<void>(fixture.service.accept_response(
        fixture.requests, fixture.registry, 1U, first_ready,
        kHostNowNs + 1000000ULL, time_zero + std::chrono::milliseconds(1)));
    const auto rejected = fixture.exchange(
        dispatch_for(started.dispatches, 2U), kHostNowNs + 2000000ULL);
    const auto aborted = fixture.service.accept_response(
        fixture.requests, fixture.registry, 2U, rejected,
        kHostNowNs + 2000000ULL, time_zero + std::chrono::milliseconds(2));
    CHECK(aborted.status == toolbusd::MotionGroupServiceStatus::Aborting);
    CHECK(aborted.dispatches.size() == 2U);
    for (const auto& dispatch : aborted.dispatches) {
        CHECK(dispatch.phase == toolbusd::MotionGroupActionPhase::Abort);
        CHECK(dispatch.submission.packet.header.command ==
              static_cast<std::uint16_t>(
                  protocol::Command::MotionGroupAbort));
    }
    CHECK(fixture.service.abort_reason() ==
          protocol::MotionGroupAbortReason::PrepareRejected);
    CHECK(fixture.first_motion->status().queue_depth == 0U);
    CHECK(fixture.second_motion->status().queue_depth == 0U);
}

void test_request_timeout_preserves_unrelated_events_and_aborts_group() {
    Fixture fixture(0U);
    const auto time_zero = toolbusd::RequestManager::TimePoint{};
    const auto started = fixture.service.start(
        fixture.requests, plan(), fixture.registry, kSessionId,
        kHostNowNs, time_zero);
    CHECK(started.dispatches.size() == 2U);
    protocol::Packet ping;
    ping.header.message_type = protocol::MessageType::Request;
    ping.header.command = static_cast<std::uint16_t>(protocol::Command::Ping);
    ping.header.session_id = 0x11112222U;
    static_cast<void>(fixture.requests.submit(ping, time_zero));

    const auto events = fixture.requests.poll(
        time_zero + std::chrono::milliseconds(11));
    const auto outcome = fixture.service.poll(
        fixture.requests, fixture.registry, events,
        kHostNowNs + 11000000ULL,
        time_zero + std::chrono::milliseconds(11));
    CHECK(outcome.status == toolbusd::MotionGroupServiceStatus::Aborting);
    CHECK(outcome.unhandled_events.size() == 1U);
    CHECK(outcome.unhandled_events.front().session_id == 0x11112222U);
    CHECK(outcome.dispatches.size() == 2U);
    CHECK(fixture.service.abort_reason() ==
          protocol::MotionGroupAbortReason::PrepareTimedOut);
    CHECK(fixture.service.pending_route_count() == 2U);
}

void test_request_retry_reuses_exact_request_identity() {
    Fixture fixture(1U);
    const auto time_zero = toolbusd::RequestManager::TimePoint{};
    const auto started = fixture.service.start(
        fixture.requests, plan(), fixture.registry, kSessionId,
        kHostNowNs, time_zero);
    const auto events = fixture.requests.poll(
        time_zero + std::chrono::milliseconds(11));
    const auto retried = fixture.service.poll(
        fixture.requests, fixture.registry, events,
        kHostNowNs + 11000000ULL,
        time_zero + std::chrono::milliseconds(11));
    CHECK(retried.status == toolbusd::MotionGroupServiceStatus::Retrying);
    CHECK(retried.dispatches.size() == 2U);
    CHECK(retried.unhandled_events.empty());
    CHECK(fixture.service.state() == toolbusd::MotionGroupState::Preparing);
    CHECK(fixture.service.pending_route_count() == 2U);
    CHECK(fixture.requests.pending_count() == 2U);
    for (const auto& dispatch : retried.dispatches) {
        const auto& original = dispatch_for(started.dispatches,
                                            dispatch.node_id);
        CHECK(dispatch.retry);
        CHECK(dispatch.submission.request_id ==
              original.submission.request_id);
        CHECK(dispatch.submission.packet.header.session_id ==
              original.submission.packet.header.session_id);
        CHECK(dispatch.submission.packet.header.command ==
              original.submission.packet.header.command);
        CHECK(dispatch.submission.packet.payload ==
              original.submission.packet.payload);
    }
}

void test_remote_error_and_response_mismatch_abort_and_ignore_late_reply() {
    {
        Fixture fixture;
        const auto time_zero = toolbusd::RequestManager::TimePoint{};
        const auto started = fixture.service.start(
            fixture.requests, plan(), fixture.registry, kSessionId,
            kHostNowNs, time_zero);
        auto response = fixture.exchange(
            dispatch_for(started.dispatches, 1U), kHostNowNs + 1000000ULL);
        const auto valid_response = response;
        response.header.flags |= protocol::kErrorResponseFlag;
        response.payload = {
            static_cast<std::uint8_t>(mock_mcu::StatusCode::AccessDenied)};
        const auto failed = fixture.service.accept_response(
            fixture.requests, fixture.registry, 1U, response,
            kHostNowNs + 1000000ULL,
            time_zero + std::chrono::milliseconds(1));
        CHECK(failed.status ==
              toolbusd::MotionGroupServiceStatus::Aborting);
        CHECK(failed.dispatches.size() == 2U);
        const auto late = fixture.service.accept_response(
            fixture.requests, fixture.registry, 1U, valid_response,
            kHostNowNs + 2000000ULL,
            time_zero + std::chrono::milliseconds(2));
        CHECK(late.status == toolbusd::MotionGroupServiceStatus::Duplicate);
    }
    {
        Fixture fixture;
        const auto time_zero = toolbusd::RequestManager::TimePoint{};
        const auto started = fixture.service.start(
            fixture.requests, plan(), fixture.registry, kSessionId,
            kHostNowNs, time_zero);
        const auto response = fixture.exchange(
            dispatch_for(started.dispatches, 1U), kHostNowNs + 1000000ULL);
        const auto mismatch = fixture.service.accept_response(
            fixture.requests, fixture.registry, 2U, response,
            kHostNowNs + 1000000ULL,
            time_zero + std::chrono::milliseconds(1));
        CHECK(mismatch.status ==
              toolbusd::MotionGroupServiceStatus::Aborting);
        CHECK(mismatch.dispatches.size() == 2U);
        CHECK(fixture.requests.pending_count() == 2U);
    }
    for (unsigned variant = 0U; variant < 3U; ++variant) {
        Fixture fixture;
        const auto time_zero = toolbusd::RequestManager::TimePoint{};
        const auto started = fixture.service.start(
            fixture.requests, plan(), fixture.registry, kSessionId,
            kHostNowNs, time_zero);
        auto response = fixture.exchange(
            dispatch_for(started.dispatches, 1U), kHostNowNs + 1000000ULL);
        if (variant == 0U) {
            response.header.command =
                static_cast<std::uint16_t>(protocol::Command::Ping);
        } else if (variant == 1U) {
            response.header.object_id = 77U;
        } else {
            // 保持载荷可解码，仅改变事务 ID，验证完整冻结身份关联。
            response.payload[5U] ^= 1U;
        }
        const auto mismatch = fixture.service.accept_response(
            fixture.requests, fixture.registry, 1U, response,
            kHostNowNs + 1000000ULL,
            time_zero + std::chrono::milliseconds(1));
        CHECK(mismatch.status ==
              toolbusd::MotionGroupServiceStatus::Aborting);
        CHECK(mismatch.dispatches.size() == 2U);
        CHECK(fixture.requests.pending_count() == 2U);
    }
}

void test_node_restart_and_active_cancel_clear_old_routes() {
    {
        Fixture fixture;
        const auto time_zero = toolbusd::RequestManager::TimePoint{};
        static_cast<void>(fixture.service.start(
            fixture.requests, plan(), fixture.registry, kSessionId,
            kHostNowNs, time_zero));
        CHECK(fixture.registry.register_clock_model(
                  2U, 303U, clock_config()) ==
              toolbusd::NodeClockRegistrationResult::ReplacedBootEpoch);
        const auto restarted = fixture.service.poll(
            fixture.requests, fixture.registry, {},
            kHostNowNs + 1000000ULL,
            time_zero + std::chrono::milliseconds(1));
        CHECK(restarted.status ==
              toolbusd::MotionGroupServiceStatus::Aborting);
        CHECK(restarted.dispatches.size() == 2U);
        CHECK(fixture.service.abort_reason() ==
              protocol::MotionGroupAbortReason::NodeRestarted);
        CHECK(fixture.requests.pending_count() == 2U);
    }
    {
        Fixture fixture;
        const auto time_zero = toolbusd::RequestManager::TimePoint{};
        const auto started = fixture.service.start(
            fixture.requests, plan(), fixture.registry, kSessionId,
            kHostNowNs, time_zero);
        const auto old_request_id = started.dispatches.front().submission
                                        .request_id;
        const auto cancelled = fixture.service.cancel(
            fixture.requests, time_zero + std::chrono::milliseconds(1));
        CHECK(cancelled.status ==
              toolbusd::MotionGroupServiceStatus::Aborting);
        CHECK(cancelled.dispatches.size() == 2U);
        CHECK(fixture.requests.pending_count() == 2U);
        protocol::Packet late;
        late.header.message_type = protocol::MessageType::Response;
        late.header.command = static_cast<std::uint16_t>(
            protocol::Command::MotionGroupPrepare);
        late.header.session_id = kSessionId;
        late.header.request_id = old_request_id;
        const auto ignored = fixture.service.accept_response(
            fixture.requests, fixture.registry,
            started.dispatches.front().node_id, late,
            kHostNowNs + 2000000ULL,
            time_zero + std::chrono::milliseconds(2));
        CHECK(ignored.status ==
              toolbusd::MotionGroupServiceStatus::Duplicate);
    }
}

void test_dispatch_gate_orders_cancel_after_existing_prepare_batch() {
    toolbusd::MotionGroupDispatchGate gate;
    std::mutex mutex;
    std::condition_variable changed;
    bool submit_entered = false;
    bool cancel_attempted = false;
    bool finish_submit = false;
    std::vector<toolbusd::MotionGroupActionPhase> order;

    std::thread submit([&] {
        auto guard = gate.lock();
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(toolbusd::MotionGroupActionPhase::Prepare);
            submit_entered = true;
        }
        changed.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return finish_submit; });
        order.push_back(toolbusd::MotionGroupActionPhase::Prepare);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return submit_entered; });
    }
    std::thread cancel([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel_attempted = true;
        }
        changed.notify_all();
        auto guard = gate.lock();
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(toolbusd::MotionGroupActionPhase::Abort);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return cancel_attempted; });
        CHECK(order.size() == 1U);
        finish_submit = true;
    }
    changed.notify_all();
    submit.join();
    cancel.join();
    CHECK(order.size() == 3U);
    CHECK(order[0U] == toolbusd::MotionGroupActionPhase::Prepare);
    CHECK(order[1U] == toolbusd::MotionGroupActionPhase::Prepare);
    CHECK(order[2U] == toolbusd::MotionGroupActionPhase::Abort);
}

}

int main() {
    test_two_remote_cores_complete_prepare_and_commit();
    test_commit_failure_reports_best_effort_abort_boundary();
    test_one_prepare_failure_never_dispatches_commit();
    test_request_timeout_preserves_unrelated_events_and_aborts_group();
    test_request_retry_reuses_exact_request_identity();
    test_remote_error_and_response_mismatch_abort_and_ignore_late_reply();
    test_node_restart_and_active_cancel_clear_old_routes();
    test_dispatch_gate_orders_cancel_after_existing_prepare_batch();
    if (failures != 0) {
        std::cerr << failures << " 项运动组服务测试失败\n";
        return 1;
    }
    std::cout << "运动组服务测试通过\n";
    return 0;
}
