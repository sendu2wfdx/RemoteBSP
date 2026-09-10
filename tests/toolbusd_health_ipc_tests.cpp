#include "remotebsp/protocol/health.hpp"
#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <unistd.h>

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

protocol::HealthSnapshot health() {
    protocol::HealthSnapshot value;
    value.source = protocol::HealthSource::Toolbusd;
    value.sample_sequence = 7U;
    value.sample_time_ms = 12U;
    value.producer_generation = 99U;
    value.metrics = {{
        static_cast<std::uint16_t>(
            protocol::HealthMetricId::SessionGeneration),
        protocol::MetricAvailability::Available,
        protocol::HealthMetricUnit::Generation, 99U}};
    return value;
}

void expect_decode_failure(std::vector<std::uint8_t> body) {
    bool rejected = false;
    try {
        static_cast<void>(toolbusd::decode_ipc_health_snapshot(body));
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_versioned_request() {
    int sockets[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_health_snapshot_request(sockets[0]);
    const auto request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind == toolbusd::IpcRequestKind::HealthSnapshot);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_atomic_identity_and_health_round_trip() {
    toolbusd::IpcToolbusdHealthSnapshot input;
    input.daemon_instance_id.fill(0xA5U);
    input.health = health();
    input.ipc_active_clients = 3U;
    input.ipc_maximum_clients = 64U;
    input.ipc_peak_clients = 9U;
    input.ipc_accepted_total = 101U;
    input.ipc_capacity_rejected_total = 7U;
    input.ipc_oversized_frame_total = 5U;
    input.ipc_timeout_total = 4U;
    input.ipc_thread_creation_failed_total = 2U;
    const auto encoded = toolbusd::encode_ipc_health_snapshot(input);
    const auto decoded = toolbusd::decode_ipc_health_snapshot(encoded);
    CHECK(decoded.version == toolbusd::kHealthSnapshotIpcVersion);
    CHECK(decoded.daemon_instance_id == input.daemon_instance_id);
    CHECK(decoded.health.source == protocol::HealthSource::Toolbusd);
    CHECK(decoded.health.node_id == 0U);
    CHECK(decoded.health.producer_generation == 99U);
    CHECK(decoded.health.sample_sequence == 7U);
    CHECK(decoded.ipc_active_clients == 3U);
    CHECK(decoded.ipc_maximum_clients == 64U);
    CHECK(decoded.ipc_peak_clients == 9U);
    CHECK(decoded.ipc_accepted_total == 101U);
    CHECK(decoded.ipc_capacity_rejected_total == 7U);
    CHECK(decoded.ipc_oversized_frame_total == 5U);
    CHECK(decoded.ipc_timeout_total == 4U);
    CHECK(decoded.ipc_thread_creation_failed_total == 2U);

    auto malformed = encoded;
    malformed[2] = 1U;
    expect_decode_failure(malformed);
    malformed = encoded;
    malformed[84] = 0U;
    malformed[85] = 0U;
    expect_decode_failure(malformed);
    malformed = encoded;
    malformed.pop_back();
    expect_decode_failure(malformed);
}

void test_wrong_source_and_zero_identity_rejected() {
    toolbusd::IpcToolbusdHealthSnapshot input;
    input.daemon_instance_id.fill(1U);
    input.health = health();
    input.health.source = protocol::HealthSource::RemoteCore;
    bool rejected = false;
    try {
        static_cast<void>(toolbusd::encode_ipc_health_snapshot(input));
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
    input.health.source = protocol::HealthSource::Toolbusd;
    input.daemon_instance_id.fill(0U);
    rejected = false;
    try {
        static_cast<void>(toolbusd::encode_ipc_health_snapshot(input));
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

}  // namespace

int main() {
    test_versioned_request();
    test_atomic_identity_and_health_round_trip();
    test_wrong_source_and_zero_identity_rejected();
    if (failures != 0) {
        std::cerr << failures << " 项 toolbusd 健康 IPC 测试失败\n";
        return 1;
    }
    std::cout << "toolbusd 健康 IPC 测试通过\n";
    return 0;
}
