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
    snapshot.clocks.push_back({
        3U, true, true, toolbusd::ClockSyncState::Synced,
        11U, 7U, 8U, 6U, 25U, -80, 100U, 200U, 300U, 400U});
    snapshot.bus_health.push_back({
        3U, 0x0C000001U, true, protocol::BusTransactionStatus::Timeout,
        2U, 5U, 123456U});
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
    assert(decoded.clocks.size() == 1U);
    assert(decoded.clocks[0].registered);
    assert(decoded.clocks[0].estimate_valid);
    assert(decoded.clocks[0].state == toolbusd::ClockSyncState::Synced);
    assert(decoded.clocks[0].boot_epoch == 11U);
    assert(decoded.clocks[0].model_generation == 7U);
    assert(decoded.clocks[0].rate_deviation_ppb == -80);
    assert(decoded.clocks[0].error_bound_ns == 200U);
    assert(decoded.bus_health.size() == 1U);
    assert(decoded.bus_health[0].last_status ==
           protocol::BusTransactionStatus::Timeout);
    assert(decoded.bus_health[0].consecutive_failures == 2U);
    assert(decoded.bus_health[0].peak_consecutive_failures == 5U);

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

    constexpr std::size_t resources_size = 2U * 50U;
    constexpr std::size_t issue_size = 8U;
    constexpr std::size_t clock_offset = header_size + node_body_size +
        traffic_body_size + resources_size + issue_size;
    auto invalid_clock = encoded;
    invalid_clock[clock_offset + 4U] = 0x80U;
    try {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_snapshot(invalid_clock));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }

    auto old_version = encoded;
    old_version[0] = 1U;
    try {
        static_cast<void>(toolbusd::decode_ipc_runtime_snapshot(old_version));
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
        auto snapshot = make_snapshot();
        snapshot.clocks.clear();
        static_cast<void>(toolbusd::encode_ipc_runtime_snapshot(snapshot));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    try {
        auto snapshot = make_snapshot();
        snapshot.clocks[0].estimate_valid = false;
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

void test_daemon_identity_round_trip_and_validation() {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_daemon_identity_request(sockets[0]);
    const auto request = toolbusd::read_ipc_request(sockets[1]);
    assert(request.kind == toolbusd::IpcRequestKind::DaemonIdentity);
    ::close(sockets[0]);
    ::close(sockets[1]);

    toolbusd::IpcDaemonIdentity identity;
    identity.instance_id[0] = 0x42U;
    identity.instance_id[15] = 0xA5U;
    const auto encoded = toolbusd::encode_ipc_daemon_identity(identity);
    const auto decoded = toolbusd::decode_ipc_daemon_identity(encoded);
    assert(decoded.version == toolbusd::kDaemonIdentityIpcVersion);
    assert(decoded.instance_id == identity.instance_id);

    try {
        toolbusd::IpcDaemonIdentity zero;
        static_cast<void>(toolbusd::encode_ipc_daemon_identity(zero));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    auto wrong_version = encoded;
    wrong_version[0] = 2U;
    try {
        static_cast<void>(
            toolbusd::decode_ipc_daemon_identity(wrong_version));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    auto nonzero_reserved = encoded;
    nonzero_reserved[2] = 1U;
    try {
        static_cast<void>(
            toolbusd::decode_ipc_daemon_identity(nonzero_reserved));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    auto truncated = encoded;
    truncated.pop_back();
    try {
        static_cast<void>(toolbusd::decode_ipc_daemon_identity(truncated));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
    auto trailing = encoded;
    trailing.push_back(0U);
    try {
        static_cast<void>(toolbusd::decode_ipc_daemon_identity(trailing));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

}  // namespace

int main() {
    test_request_round_trip();
    test_snapshot_round_trip_and_strict_flags();
    test_invalid_limits_and_duplicates();
    test_daemon_identity_round_trip_and_validation();
    std::cout << "Runtime 单次快照 IPC 测试通过\n";
    return 0;
}
