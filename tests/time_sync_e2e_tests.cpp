#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/time_sync_bsp.hpp"
#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/toolbusd/clock_sync_manager.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

constexpr std::size_t kMtu = 64U;
constexpr std::uint32_t kNodeId = 12U;
constexpr std::uint32_t kSessionId = 0x12345678U;

mock_mcu::NodeInfo make_node_info() {
    mock_mcu::NodeInfo info;
    for (std::size_t index = 0U; index < info.uuid.size(); ++index) {
        info.uuid[index] = static_cast<std::uint8_t>(index + 10U);
    }
    info.firmware_major = 1U;
    info.board_type = 0x11223344U;
    return info;
}

std::unique_ptr<mock_mcu::MockNode> make_node(
    const mock_mcu::NodeInfo& info, std::uint64_t boot_epoch,
    std::uint64_t tick_rate_hz = 1000000ULL,
    std::uint64_t initial_tick = 500ULL) {
    auto clock = std::make_shared<mock_mcu::MockTimeSyncBsp>(
        mock_mcu::MockTimeSyncConfig{
            boot_epoch, tick_rate_hz, 64U, initial_tick, 20U},
        mock_mcu::TimeSyncBsp::TimePoint{});
    return std::make_unique<mock_mcu::MockNode>(
        mock_mcu::RemoteCore(info, 0U, nullptr, nullptr, {}, {}, nullptr,
                             nullptr, nullptr, nullptr, clock),
        kMtu, kNodeId);
}

void register_node(toolbusd::NodeRegistry& registry,
                   const mock_mcu::NodeInfo& info) {
    protocol::Packet discovery;
    discovery.header.message_type = protocol::MessageType::Response;
    discovery.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryResponse);
    discovery.payload = protocol::encode_node_identity(
        {info.uuid, info.firmware_major, info.firmware_minor,
         info.firmware_patch, info.board_type, info.protocol_version});
    CHECK(registry.accept_discovery_response(
              discovery, toolbusd::NodeRegistry::TimePoint{}) ==
          toolbusd::NodeUpdate::Added);
    static_cast<void>(
        registry.make_node_assignment(info.uuid, kNodeId, 1U));

    protocol::Packet heartbeat;
    heartbeat.header.message_type = protocol::MessageType::Event;
    heartbeat.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    heartbeat.header.object_id = kNodeId;
    heartbeat.payload =
        protocol::encode_heartbeat({info.uuid, info.protocol_version});
    CHECK(registry.accept_heartbeat(
        heartbeat, toolbusd::NodeRegistry::TimePoint{}));
}

protocol::Packet send_request(
    mock_mcu::MockNode& node, const toolbusd::Submission& submission,
    std::uint16_t transfer_id,
    toolbusd::ClockSyncManager::TimePoint node_receive_time,
    toolbusd::ClockSyncManager::TimePoint host_receive_time) {
    const auto request_frames = protocol::Fragmenter(kMtu).split(
        protocol::encode(submission.packet), transfer_id);
    std::optional<mock_mcu::NodeReply> reply;
    for (const auto& frame : request_frames) {
        const auto current = node.handle_frame(
            0x600U + kNodeId, frame, node_receive_time);
        if (current.has_value()) {
            reply = current;
        }
    }
    CHECK(reply.has_value());
    if (!reply.has_value()) {
        throw std::runtime_error("Mock 节点未返回时钟同步响应");
    }

    protocol::Reassembler reassembler(
        kMtu, std::chrono::milliseconds(100));
    protocol::ReassemblyResult result;
    for (const auto& frame : reply->frames) {
        result = reassembler.accept(
            0x580U + kNodeId, frame, host_receive_time);
    }
    CHECK(result.status == protocol::ReassemblyStatus::Complete);
    CHECK(result.packet.has_value());
    if (!result.packet.has_value()) {
        throw std::runtime_error("主机未完成时钟同步响应重组");
    }
    return protocol::decode(*result.packet);
}

toolbusd::ClockSyncResponseOutcome exchange(
    mock_mcu::MockNode& node, toolbusd::ClockSyncManager& sync,
    toolbusd::RequestManager& requests, toolbusd::NodeRegistry& registry,
    std::uint64_t host_send_ns, std::uint16_t transfer_id) {
    using Nanoseconds = std::chrono::nanoseconds;
    const auto host_send =
        toolbusd::ClockSyncManager::TimePoint{} + Nanoseconds(host_send_ns);
    const auto node_receive = host_send + Nanoseconds(100000ULL);
    const auto host_receive = node_receive + Nanoseconds(120000ULL);
    const auto submission = sync.submit(
        requests, kNodeId, kSessionId,
        host_send - Nanoseconds(1000ULL));
    CHECK(sync.mark_sent(submission.packet.header.session_id,
                         submission.packet.header.request_id,
                         host_send) ==
          toolbusd::ClockSyncMarkSentResult::Recorded);
    CHECK(sync.mark_sent(submission.packet.header.session_id,
                         submission.packet.header.request_id,
                         host_send + Nanoseconds(50000ULL)) ==
          toolbusd::ClockSyncMarkSentResult::AlreadyRecorded);
    const auto response = send_request(node, submission, transfer_id,
                                       node_receive, host_receive);
    return sync.accept_response(requests, registry, response, kNodeId,
                                host_receive);
}

void test_deterministic_mock_clock() {
    const mock_mcu::MockTimeSyncConfig config{
        44U, 1000000ULL, 32U, 0xFFFFFFF0ULL, 32U};
    const auto origin = mock_mcu::TimeSyncBsp::TimePoint{};
    const mock_mcu::MockTimeSyncBsp first(config, origin);
    const mock_mcu::MockTimeSyncBsp second(config, origin);
    const auto first_capture = first.capture(origin);
    const auto second_capture = second.capture(origin);
    CHECK(first_capture.boot_epoch == 44U);
    CHECK(first_capture.node_receive_tick == 0xFFFFFFF0ULL);
    CHECK(first_capture.node_send_tick == 0x10ULL);
    CHECK(first_capture.node_receive_tick ==
          second_capture.node_receive_tick);
    CHECK(first_capture.node_send_tick == second_capture.node_send_tick);

    const auto later = first.capture(origin + std::chrono::milliseconds(1));
    CHECK(later.node_receive_tick == 0x3D8ULL);
    CHECK(later.node_send_tick == 0x3F8ULL);

    const mock_mcu::MockTimeSyncBsp generated_first;
    const mock_mcu::MockTimeSyncBsp generated_second;
    CHECK(generated_first.boot_epoch() != 0U);
    CHECK(generated_second.boot_epoch() != 0U);
    CHECK(generated_first.boot_epoch() != generated_second.boot_epoch());
}

void test_end_to_end_sync_and_restart() {
    const auto info = make_node_info();
    auto node = make_node(info, 100U);
    toolbusd::NodeRegistry registry;
    register_node(registry, info);
    toolbusd::RequestManager requests;
    toolbusd::ClockSyncManager sync;

    for (std::size_t index = 0U; index < 8U; ++index) {
        const auto outcome = exchange(
            *node, sync, requests, registry,
            1000000000ULL + static_cast<std::uint64_t>(index) *
                                20000000ULL,
            static_cast<std::uint16_t>(index + 1U));
        CHECK(outcome.status ==
              toolbusd::ClockSyncResponseStatus::Accepted);
        CHECK(outcome.payload.has_value());
        CHECK(outcome.payload->boot_epoch == 100U);
    }
    CHECK(sync.pending_count() == 0U);
    CHECK(registry.clock_boot_epoch(kNodeId) == 100U);
    const auto estimate = registry.clock_estimate(
        kNodeId, 100U, 1141000000ULL);
    CHECK(estimate.has_value());
    CHECK(estimate->state == toolbusd::ClockSyncState::Synced);

    const auto converted = registry.host_to_node_time(
        kNodeId, 100U, 1145000000ULL, 1141000000ULL, 200000ULL);
    CHECK(converted.status == toolbusd::NodeClockAccessStatus::Ready);
    CHECK(converted.node_tick == 1145500ULL);

    node = make_node(info, 101U, 1000000ULL, 9000ULL);
    const auto restarted = exchange(*node, sync, requests, registry,
                                    1200000000ULL, 20U);
    CHECK(restarted.status ==
          toolbusd::ClockSyncResponseStatus::Accepted);
    CHECK(restarted.registration_result ==
          toolbusd::NodeClockRegistrationResult::ReplacedBootEpoch);
    CHECK(!registry.clock_estimate(kNodeId, 100U, 1201000000ULL)
               .has_value());
    const auto new_estimate = registry.clock_estimate(
        kNodeId, 101U, 1201000000ULL);
    CHECK(new_estimate.has_value());
    CHECK(new_estimate->state == toolbusd::ClockSyncState::Unsynced);

    for (std::size_t index = 1U; index < 8U; ++index) {
        const auto outcome = exchange(
            *node, sync, requests, registry,
            1200000000ULL + static_cast<std::uint64_t>(index) *
                                20000000ULL,
            static_cast<std::uint16_t>(20U + index));
        CHECK(outcome.status ==
              toolbusd::ClockSyncResponseStatus::Accepted);
    }
    const auto restarted_estimate = registry.clock_estimate(
        kNodeId, 101U, 1341000000ULL);
    CHECK(restarted_estimate.has_value());
    CHECK(restarted_estimate->state ==
          toolbusd::ClockSyncState::Synced);

    auto inconsistent = make_node(info, 101U, 2000000ULL, 9000ULL);
    const auto mismatch = exchange(*inconsistent, sync, requests, registry,
                                   1400000000ULL, 40U);
    CHECK(mismatch.status ==
          toolbusd::ClockSyncResponseStatus::RegistrationRejected);
    CHECK(mismatch.registration_result ==
          toolbusd::NodeClockRegistrationResult::ConfigurationMismatch);
    CHECK(registry.clock_boot_epoch(kNodeId) == 101U);
}

void test_missing_send_boundary_and_cancel() {
    const auto info = make_node_info();
    auto node = make_node(info, 200U);
    toolbusd::NodeRegistry registry;
    register_node(registry, info);
    toolbusd::RequestManager requests;
    toolbusd::ClockSyncManager sync;
    using Nanoseconds = std::chrono::nanoseconds;
    const auto send_time =
        toolbusd::ClockSyncManager::TimePoint{} + Nanoseconds(1000000000ULL);
    const auto submission =
        sync.submit(requests, kNodeId, kSessionId, send_time);
    const auto response = send_request(
        *node, submission, 1U, send_time + Nanoseconds(100000ULL),
        send_time + Nanoseconds(220000ULL));
    const auto missing = sync.accept_response(
        requests, registry, response, kNodeId,
        send_time + Nanoseconds(220000ULL));
    CHECK(missing.status ==
          toolbusd::ClockSyncResponseStatus::SendTimeMissing);
    CHECK(registry.clock_model_count() == 0U);

    const auto cancelled =
        sync.submit(requests, kNodeId, kSessionId, send_time);
    CHECK(sync.cancel(requests, cancelled.packet.header.session_id,
                      cancelled.packet.header.request_id));
    CHECK(sync.pending_count() == 0U);
    CHECK(requests.pending_count() == 0U);
}

}

int main() {
    test_deterministic_mock_clock();
    test_end_to_end_sync_and_restart();
    test_missing_send_boundary_and_cancel();
    if (failures != 0) {
        std::cerr << failures << " 个时钟同步端到端测试失败\n";
        return 1;
    }
    std::cout << "所有时钟同步端到端测试通过\n";
    return 0;
}
