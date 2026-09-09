#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/toolbusd/clock_sync_manager.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

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

protocol::NodeUuid make_uuid(std::uint8_t seed) {
    protocol::NodeUuid uuid{};
    for (std::size_t index = 0U; index < uuid.size(); ++index) {
        uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    return uuid;
}

void register_node(toolbusd::NodeRegistry& nodes, std::uint32_t node_id,
                   std::uint8_t seed,
                   toolbusd::NodeRegistry::TimePoint now) {
    protocol::NodeIdentity identity;
    identity.uuid = make_uuid(seed);
    identity.protocol_version = protocol::kProtocolVersion;

    protocol::Packet discovery;
    discovery.header.message_type = protocol::MessageType::Response;
    discovery.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryResponse);
    discovery.payload = protocol::encode_node_identity(identity);
    CHECK(nodes.accept_discovery_response(discovery, now) ==
          toolbusd::NodeUpdate::Added);
    static_cast<void>(nodes.make_node_assignment(
        identity.uuid, node_id, 0x80000000U + node_id));

    protocol::Packet heartbeat;
    heartbeat.header.message_type = protocol::MessageType::Event;
    heartbeat.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    heartbeat.header.object_id = node_id;
    heartbeat.payload = protocol::encode_heartbeat(
        {identity.uuid, identity.protocol_version});
    CHECK(nodes.accept_heartbeat(heartbeat, now));
}

void test_bounded_periodic_schedule_and_timeout() {
    using Milliseconds = std::chrono::milliseconds;
    const auto start = toolbusd::ClockSyncManager::TimePoint{};
    toolbusd::NodeRegistry nodes;
    register_node(nodes, 1U, 10U, start);
    register_node(nodes, 2U, 30U, start);
    register_node(nodes, 3U, 50U, start);

    toolbusd::RequestManagerConfig request_config;
    request_config.timeout = Milliseconds(10);
    request_config.maximum_retries = 1U;
    toolbusd::RequestManager requests(request_config);

    toolbusd::ClockSyncManagerConfig sync_config;
    sync_config.sync_interval = Milliseconds(100);
    sync_config.maximum_submissions_per_poll = 2U;
    toolbusd::ClockSyncManager sync(sync_config);

    const auto first = sync.poll_schedule(requests, nodes, 77U, start);
    CHECK(first.size() == 2U);
    CHECK(first[0].node_id == 1U);
    CHECK(first[1].node_id == 2U);
    CHECK(sync.pending_count() == 2U);
    CHECK(sync.tracked_node_count() == 3U);

    const auto second = sync.poll_schedule(requests, nodes, 77U, start);
    CHECK(second.size() == 1U);
    CHECK(second[0].node_id == 3U);
    CHECK(sync.pending_count() == 3U);
    CHECK(sync.tracked_node_count() == 3U);

    CHECK(sync.mark_sent(
              first[0].submission.packet.header.session_id,
              first[0].submission.packet.header.request_id, start) ==
          toolbusd::ClockSyncMarkSentResult::Recorded);
    CHECK(sync.mark_sent(
              first[0].submission.packet.header.session_id,
              first[0].submission.packet.header.request_id,
              start + Milliseconds(1)) ==
          toolbusd::ClockSyncMarkSentResult::AlreadyRecorded);

    const auto retries = requests.poll(start + Milliseconds(10));
    CHECK(retries.size() == 3U);
    for (const auto& event : retries) {
        CHECK(event.type == toolbusd::RequestEventType::Retry);
        CHECK(!sync.handle_request_event(requests, event));
    }
    CHECK(sync.pending_count() == 3U);

    const auto timeouts = requests.poll(start + Milliseconds(20));
    CHECK(timeouts.size() == 3U);
    for (const auto& event : timeouts) {
        CHECK(event.type == toolbusd::RequestEventType::TimedOut);
        CHECK(sync.handle_request_event(requests, event));
    }
    CHECK(sync.pending_count() == 0U);
    CHECK(requests.pending_count() == 0U);
    CHECK(sync.poll_schedule(requests, nodes, 77U,
                             start + Milliseconds(99)).empty());
    CHECK(sync.poll_schedule(requests, nodes, 77U,
                             start + Milliseconds(100)).size() == 2U);
}

void test_offline_and_restart_cancel_lifecycle() {
    using Milliseconds = std::chrono::milliseconds;
    const auto start = toolbusd::ClockSyncManager::TimePoint{};
    toolbusd::NodeRegistry nodes(Milliseconds(2000));
    register_node(nodes, 5U, 70U, start);
    toolbusd::RequestManager requests;
    toolbusd::ClockSyncManager sync;

    CHECK(sync.poll_schedule(requests, nodes, 88U, start).size() == 1U);
    CHECK(sync.pending_count() == 1U);
    CHECK(nodes.expire(start + Milliseconds(2000)).size() == 1U);
    CHECK(sync.poll_schedule(requests, nodes, 88U,
                             start + Milliseconds(2000)).empty());
    CHECK(sync.pending_count() == 0U);
    CHECK(sync.tracked_node_count() == 0U);
    CHECK(requests.pending_count() == 0U);

    const auto uuid = make_uuid(70U);
    protocol::Packet heartbeat;
    heartbeat.header.message_type = protocol::MessageType::Event;
    heartbeat.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    heartbeat.header.object_id = 5U;
    heartbeat.payload = protocol::encode_heartbeat(
        {uuid, protocol::kProtocolVersion});
    CHECK(nodes.accept_heartbeat(heartbeat, start + Milliseconds(2100)));
    CHECK(sync.poll_schedule(requests, nodes, 88U,
                             start + Milliseconds(2100)).size() == 1U);

    CHECK(sync.cancel_node(requests, 5U) == 1U);
    CHECK(nodes.mark_assignment_unconfirmed(uuid));
    CHECK(sync.pending_count() == 0U);
    CHECK(sync.tracked_node_count() == 0U);
    CHECK(sync.poll_schedule(requests, nodes, 88U,
                             start + Milliseconds(2200)).empty());

    CHECK(nodes.accept_heartbeat(heartbeat, start + Milliseconds(2300)));
    CHECK(sync.poll_schedule(requests, nodes, 88U,
                             start + Milliseconds(2300)).size() == 1U);
}

void test_invalid_scheduler_configuration() {
    toolbusd::ClockSyncManagerConfig config;
    config.maximum_submissions_per_poll = 0U;
    try {
        static_cast<void>(toolbusd::ClockSyncManager(config));
        CHECK(false);
    } catch (const toolbusd::ClockSyncManagerException& error) {
        CHECK(error.code() ==
              toolbusd::ClockSyncManagerError::InvalidConfiguration);
    }
}

void test_tracking_and_pending_capacity() {
    const auto start = toolbusd::ClockSyncManager::TimePoint{};
    toolbusd::NodeRegistry nodes;
    register_node(nodes, 1U, 90U, start);
    register_node(nodes, 2U, 110U, start);
    register_node(nodes, 3U, 130U, start);
    toolbusd::RequestManager requests;
    toolbusd::ClockSyncManagerConfig config;
    config.maximum_pending_requests = 1U;
    config.maximum_tracked_nodes = 2U;
    config.maximum_submissions_per_poll = 4U;
    toolbusd::ClockSyncManager sync(config);

    CHECK(sync.poll_schedule(requests, nodes, 99U, start).size() == 1U);
    CHECK(sync.pending_count() == 1U);
    CHECK(sync.tracked_node_count() == 2U);
    CHECK(sync.poll_schedule(requests, nodes, 99U, start).empty());
    CHECK(sync.pending_count() == 1U);
    CHECK(sync.tracked_node_count() == 2U);
}

}  // namespace

int main() {
    test_bounded_periodic_schedule_and_timeout();
    test_offline_and_restart_cancel_lifecycle();
    test_invalid_scheduler_configuration();
    test_tracking_and_pending_capacity();
    if (failures != 0) {
        std::cerr << failures << " 个时钟同步调度测试失败\n";
        return 1;
    }
    std::cout << "所有时钟同步调度测试通过\n";
    return 0;
}
