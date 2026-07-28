#include "remotebsp/mock_mcu/mock_node.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

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
    info.uuid[0] = 0x42;
    info.firmware_major = 1;
    return mock_mcu::MockNode(
        mock_mcu::RemoteCore(
            info, mock_mcu::capability_mask(mock_mcu::Capability::Gpio)),
        mtu, 7, std::chrono::milliseconds(100));
}

protocol::Packet make_ping() {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command =
        static_cast<std::uint16_t>(protocol::Command::Ping);
    request.header.session_id = 11;
    request.header.request_id = 22;
    request.payload = {1, 2, 3, 4, 5};
    return request;
}

protocol::Packet reassemble_reply(
    std::size_t mtu, const mock_mcu::NodeReply& reply) {
    protocol::Reassembler reassembler(mtu, std::chrono::milliseconds(100));
    protocol::ReassemblyResult result;
    for (const auto& frame : reply.frames) {
        result = reassembler.accept(2, frame);
    }
    CHECK(result.status == protocol::ReassemblyStatus::Complete);
    CHECK(result.packet.has_value());
    return protocol::decode(*result.packet);
}

void test_request_response(std::size_t mtu) {
    auto node = make_node(mtu);
    const auto request_frames =
        protocol::Fragmenter(mtu).split(protocol::encode(make_ping()), 99);

    std::optional<mock_mcu::NodeReply> reply;
    for (const auto& frame : request_frames) {
        const auto current = node.handle_frame(1, frame);
        if (current.has_value()) {
            reply = current;
        }
    }
    CHECK(reply.has_value());
    CHECK(reply->transfer_id == 1);

    const auto response = reassemble_reply(mtu, *reply);
    CHECK(response.header.message_type == protocol::MessageType::Response);
    CHECK(response.header.request_id == 22);
    CHECK(response.payload ==
          std::vector<std::uint8_t>({0, 1, 2, 3, 4, 5}));

    const auto duplicate = node.handle_frame(1, request_frames.back());
    CHECK(duplicate.has_value());
    CHECK(reassemble_reply(mtu, *duplicate).payload == response.payload);
    CHECK(node.executed_request_count() == 1);
    CHECK(node.cached_response_count() == 1);
    CHECK(node.cached_request_count() == 1);

    const auto new_transfer_frames =
        protocol::Fragmenter(mtu).split(protocol::encode(make_ping()), 100);
    std::optional<mock_mcu::NodeReply> retry_reply;
    for (const auto& frame : new_transfer_frames) {
        const auto current = node.handle_frame(1, frame);
        if (current.has_value()) {
            retry_reply = current;
        }
    }
    CHECK(retry_reply.has_value());
    /* 首次响应、重复末片重发和新传输重试分别占用 1、2、3。 */
    CHECK(retry_reply->transfer_id == 3);
    CHECK(node.executed_request_count() == 1);
    CHECK(node.cached_response_count() == 2);
}

void test_heartbeat(std::size_t mtu) {
    auto node = make_node(mtu);
    const auto heartbeat = node.make_heartbeat();
    CHECK(heartbeat.transfer_id == 1);
    const auto packet = reassemble_reply(mtu, heartbeat);
    CHECK(packet.header.message_type == protocol::MessageType::Event);
    CHECK(packet.header.command ==
          static_cast<std::uint16_t>(protocol::Command::Heartbeat));
    CHECK(packet.header.object_id == 7);
    CHECK(packet.payload.size() == 17);
    CHECK(packet.payload[0] == 0x42);
    CHECK(packet.payload[16] == protocol::kProtocolVersion);
}

void test_conflict_eviction_and_session_clear() {
    auto node = make_node(64);
    auto request = make_ping();
    auto frames =
        protocol::Fragmenter(64).split(protocol::encode(request), 1);
    for (const auto& frame : frames) {
        node.handle_frame(1, frame);
    }

    request.payload = {9};
    frames = protocol::Fragmenter(64).split(protocol::encode(request), 2);
    try {
        for (const auto& frame : frames) {
            node.handle_frame(1, frame);
        }
        CHECK(false);
    } catch (const mock_mcu::MockNodeException& error) {
        CHECK(error.code() ==
              mock_mcu::MockNodeError::ConflictingDuplicateRequest);
    }

    CHECK(node.clear_session(11) == 1);
    CHECK(node.cached_request_count() == 0);
}

}

int main() {
    test_request_response(8);
    test_request_response(64);
    test_heartbeat(8);
    test_heartbeat(64);
    test_conflict_eviction_and_session_clear();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有 Mock 节点流水线测试通过\n";
    return 0;
}
