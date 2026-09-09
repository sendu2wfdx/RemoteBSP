#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace remotebsp;

namespace {

toolbusd::IpcRuntimeSnapshot make_snapshot() {
    toolbusd::IpcRuntimeSnapshot snapshot;
    snapshot.sequence = 42U;
    toolbusd::IpcNodeInfo node;
    node.uuid[0] = 0xA5U;
    node.node_id = 3U;
    node.online = true;
    node.ready = true;
    node.protocol_version = protocol::kProtocolVersion;
    snapshot.nodes.push_back(node);
    snapshot.traffic.arbitration_bits_per_second = 500000U;
    snapshot.traffic.data_bits_per_second = 2000000U;
    snapshot.traffic.maximum_utilization_permille = 700U;
    snapshot.resources.push_back({
        3U, true,
        {0x02000001U, protocol::ResourceType::Uart, 1U, 0U, 64U, 32U},
        {0x02000001U, protocol::ResourceHealth::Normal, 0U, 2U, 3U, 4U,
         5U},
    });
    snapshot.resources.push_back({
        3U, false,
        {0x01000002U, protocol::ResourceType::Gpio, 2U, 0U, 0U, 0U},
        {0x01000002U, protocol::ResourceHealth::Normal, 0U, 0U, 0U, 0U,
         0U},
    });
    snapshot.node_issues.push_back(
        {3U, toolbusd::IpcRuntimeNodeError::ResourceInventoryUnavailable});
    return snapshot;
}

void test_request_round_trip() {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_snapshot_request(sockets[0], 64U, 1500U);
    const auto request = toolbusd::read_ipc_request(sockets[1]);
    assert(request.kind == toolbusd::IpcRequestKind::RuntimeSnapshot);
    assert(request.maximum_length == 64U);
    assert(request.timeout_ms == 1500U);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_snapshot_round_trip_and_strict_flags() {
    const auto encoded = toolbusd::encode_ipc_runtime_snapshot(
        make_snapshot());
    const auto decoded = toolbusd::decode_ipc_runtime_snapshot(encoded);
    assert(decoded.version == toolbusd::kRuntimeSnapshotIpcVersion);
    assert(decoded.sequence == 42U);
    assert(decoded.nodes.size() == 1U);
    assert(decoded.resources.size() == 2U);
    assert(decoded.resources[0].status_valid);
    assert(!decoded.resources[1].status_valid);
    assert(decoded.resources[0].descriptor.resource_id == 0x02000001U);
    assert(decoded.resources[0].status.rx_overruns == 4U);
    assert(decoded.node_issues.size() == 1U);

    auto invalid = encoded;
    constexpr std::size_t header_size = 28U;
    constexpr std::size_t node_body_size = 35U;
    constexpr std::size_t traffic_body_size = 268U;
    invalid[header_size + node_body_size + traffic_body_size + 4U] = 2U;
    try {
        static_cast<void>(toolbusd::decode_ipc_runtime_snapshot(invalid));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

void test_invalid_limits_and_duplicates() {
    try {
        auto snapshot = make_snapshot();
        snapshot.sequence = 0U;
        static_cast<void>(toolbusd::encode_ipc_runtime_snapshot(snapshot));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    try {
        auto snapshot = make_snapshot();
        snapshot.resources.push_back(snapshot.resources.front());
        static_cast<void>(toolbusd::encode_ipc_runtime_snapshot(snapshot));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    try {
        int sockets[2]{};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        toolbusd::write_ipc_runtime_snapshot_request(sockets[0], 129U, 1U);
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

}  // namespace

int main() {
    test_request_round_trip();
    test_snapshot_round_trip_and_strict_flags();
    test_invalid_limits_and_duplicates();
    std::cout << "Runtime 单次快照 IPC 测试通过\n";
    return 0;
}
