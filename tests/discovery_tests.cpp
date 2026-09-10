#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

mock_mcu::MockNode make_node(std::size_t mtu) {
    mock_mcu::NodeInfo info;
    for (std::size_t index = 0; index < info.uuid.size(); ++index) {
        info.uuid[index] = static_cast<std::uint8_t>(index + 1U);
    }
    info.firmware_major = 2;
    info.firmware_minor = 3;
    info.firmware_patch = 4;
    info.board_type = 0x12345678;
    return mock_mcu::MockNode(mock_mcu::RemoteCore(info, 1), mtu, 0);
}

std::optional<mock_mcu::NodeReply> send_to_node(
    mock_mcu::MockNode& node, std::size_t mtu,
    const protocol::Packet& packet, std::uint16_t transfer_id) {
    const auto frames =
        protocol::Fragmenter(mtu).split(protocol::encode(packet),
                                        transfer_id);
    std::optional<mock_mcu::NodeReply> reply;
    for (const auto& frame : frames) {
        const auto current = node.handle_frame(0x600, frame);
        if (current.has_value()) {
            reply = current;
        }
    }
    return reply;
}

protocol::Packet decode_reply(std::size_t mtu,
                              const mock_mcu::NodeReply& reply) {
    protocol::Reassembler reassembler(mtu, std::chrono::milliseconds(100));
    protocol::ReassemblyResult result;
    for (const auto& frame : reply.frames) {
        result = reassembler.accept(0x580, frame);
    }
    CHECK(result.status == protocol::ReassemblyStatus::Complete);
    CHECK(result.packet.has_value());
    return protocol::decode(*result.packet);
}

void test_payload_codec() {
    const protocol::DiscoveryRequestPayload range{1, 3};
    CHECK(protocol::decode_discovery_request(
              protocol::encode_discovery_request(range))
              .maximum_protocol_version == 3);

    protocol::NodeAssignment assignment;
    assignment.uuid[0] = 9;
    assignment.node_id = 77;
    const auto decoded_assignment = protocol::decode_node_assignment(
        protocol::encode_node_assignment(assignment));
    CHECK(decoded_assignment.uuid == assignment.uuid);
    CHECK(decoded_assignment.node_id == 77);

    try {
        protocol::decode_heartbeat({1, 2});
        CHECK(false);
    } catch (const protocol::DiscoveryPayloadException& error) {
        CHECK(error.code() ==
              protocol::DiscoveryPayloadError::InvalidLength);
    }
}

void test_discovery_assignment_and_heartbeat(std::size_t mtu) {
    auto node = make_node(mtu);
    toolbusd::NodeRegistry registry(std::chrono::milliseconds(2000));
    const auto start = toolbusd::NodeRegistry::TimePoint{};

    const auto discovery_request = registry.make_discovery_request(10);
    const auto discovery_reply =
        send_to_node(node, mtu, discovery_request, 1);
    CHECK(discovery_reply.has_value());
    const auto discovery_response =
        decode_reply(mtu, *discovery_reply);
    CHECK(discovery_response.header.command == static_cast<std::uint16_t>(
                                                   protocol::Command::DiscoveryResponse));
    CHECK(registry.accept_discovery_response(discovery_response, start) ==
          toolbusd::NodeUpdate::Added);
    CHECK(registry.size() == 1);
    CHECK(registry.online_count() == 1);

    const auto identity =
        protocol::decode_node_identity(discovery_response.payload);
    const auto* record = registry.find(identity.uuid);
    CHECK(record != nullptr);
    CHECK(record->identity.firmware_major == 2);
    CHECK(record->identity.board_type == 0x12345678);

    const auto assignment =
        registry.make_node_assignment(identity.uuid, 25, 11);
    CHECK(registry.find(identity.uuid)->node_id == 25);
    CHECK(!registry.find(identity.uuid)->assigned);

    /*
     * 模拟首次 NODE_ASSIGN 丢失：再次收到发现响应后必须保留原 ID，
     * 以便守护进程幂等重发，而不是另行消耗节点 ID。
     */
    CHECK(registry.accept_discovery_response(
              discovery_response,
              start + std::chrono::milliseconds(100)) ==
          toolbusd::NodeUpdate::Updated);
    CHECK(registry.find(identity.uuid)->node_id == 25);
    CHECK(!registry.find(identity.uuid)->assigned);
    const auto retry_assignment =
        registry.make_node_assignment(identity.uuid, 25, 12);
    const auto decoded_retry =
        protocol::decode_node_assignment(retry_assignment.payload);
    CHECK(decoded_retry.node_id == 25);
    CHECK(decoded_retry.uuid == identity.uuid);

    const auto assignment_reply = send_to_node(node, mtu, assignment, 2);
    CHECK(assignment_reply.has_value());
    const auto assignment_response =
        decode_reply(mtu, *assignment_reply);
    CHECK(assignment_response.header.object_id == 25);
    CHECK(assignment_response.payload == std::vector<std::uint8_t>({0}));

    const auto heartbeat = decode_reply(mtu, node.make_heartbeat());
    CHECK(heartbeat.header.object_id == 25);
    CHECK(registry.accept_heartbeat(
        heartbeat, start + std::chrono::milliseconds(500)));
    CHECK(registry.find(identity.uuid)->node_id == 25);
    CHECK(registry.find(identity.uuid)->assigned);

    /* MCU 复位后从临时 CAN ID 响应发现，守护进程应撤销确认并保留原 ID。 */
    CHECK(registry.mark_assignment_unconfirmed(identity.uuid));
    CHECK(registry.find(identity.uuid)->node_id == 25);
    CHECK(!registry.find(identity.uuid)->assigned);
    protocol::NodeUuid unknown_uuid{};
    unknown_uuid[0] = 0xFFU;
    CHECK(!registry.mark_assignment_unconfirmed(unknown_uuid));

    CHECK(registry.accept_heartbeat(
        heartbeat, start + std::chrono::milliseconds(600)));
    CHECK(registry.find(identity.uuid)->assigned);
    CHECK(registry.observe_response(
        25, start + std::chrono::milliseconds(1500)));
    CHECK(registry.expire(
              start + std::chrono::milliseconds(3499))
              .empty());
    const auto offline =
        registry.expire(start + std::chrono::milliseconds(3500));
    CHECK(offline.size() == 1);
    CHECK(offline[0] == identity.uuid);
    CHECK(registry.online_count() == 0);
}

void test_incompatible_and_wrong_uuid() {
    auto node = make_node(64);
    toolbusd::NodeRegistry registry;
    const auto incompatible =
        registry.make_discovery_request(1, 2, 3);
    CHECK(!send_to_node(node, 64, incompatible, 1).has_value());

    auto compatible = registry.make_discovery_request(2);
    const auto response =
        decode_reply(64, *send_to_node(node, 64, compatible, 2));
    registry.accept_discovery_response(response);
    auto identity = protocol::decode_node_identity(response.payload);
    identity.uuid[0] ^= 0xFFU;
    protocol::Packet assignment;
    assignment.header.message_type = protocol::MessageType::Request;
    assignment.header.command =
        static_cast<std::uint16_t>(protocol::Command::NodeAssign);
    assignment.header.request_id = 3;
    assignment.payload =
        protocol::encode_node_assignment({identity.uuid, 9});
    CHECK(!send_to_node(node, 64, assignment, 3).has_value());
}

}

int main() {
    test_payload_codec();
    test_discovery_assignment_and_heartbeat(8);
    test_discovery_assignment_and_heartbeat(64);
    test_incompatible_and_wrong_uuid();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有节点发现和心跳测试通过\n";
    return 0;
}
