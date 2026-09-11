#include "remotebsp/cli_json.hpp"
#include "remotebsp/client.hpp"
#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace toolbusd = remotebsp::toolbusd;
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                      \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <std::size_t Size>
std::array<std::uint8_t, Size> filled(std::uint8_t value) {
    std::array<std::uint8_t, Size> result{};
    result.fill(value);
    return result;
}

void expect_failure(const std::function<void()>& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

toolbusd::RuntimeOperationOutcome committed_gpio(bool replayed = false) {
    toolbusd::RuntimeOperationOutcome outcome;
    outcome.kind = toolbusd::RuntimeOperationKind::GpioWrite;
    outcome.state = toolbusd::RuntimeOperationState::Committed;
    outcome.recovery = toolbusd::RuntimeOperationRecovery::None;
    outcome.replayed = replayed;
    outcome.operation_id = filled<32>(0xa5U);
    outcome.lease_id = filled<16>(0xb1U);
    outcome.expected_node_uuid = filled<16>(0xc2U);
    outcome.resource_id = 0x01020304U;
    outcome.object_id = 0x11223344U;
    outcome.value = true;
    return outcome;
}

void test_query_golden_and_strict_decode() {
    toolbusd::RuntimeOperationQuery query;
    for (std::size_t index = 0; index < query.daemon_instance_id.size();
         ++index) {
        query.daemon_instance_id[index] =
            static_cast<std::uint8_t>(index + 1U);
    }
    for (std::size_t index = 0; index < query.operation_id.size(); ++index) {
        query.operation_id[index] =
            static_cast<std::uint8_t>(index + 0x21U);
    }
    query.owner_key_id = "a";
    const auto encoded = toolbusd::encode_ipc_runtime_operation_query(query);
    const std::vector<std::uint8_t> golden{
        0x01, 0x00, 0x38, 0x00,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
        0x01, 0x00, 0x00, 0x00, 0x61};
    CHECK(encoded == golden);
    const auto decoded =
        toolbusd::decode_ipc_runtime_operation_query(encoded);
    CHECK(decoded.daemon_instance_id == query.daemon_instance_id);
    CHECK(decoded.operation_id == query.operation_id);
    CHECK(decoded.owner_key_id == "a");

    auto malformed = encoded;
    malformed.pop_back();
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
    malformed = encoded;
    malformed.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
    malformed = encoded;
    malformed[54U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
    malformed = encoded;
    malformed[0U] = 2U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
    malformed = encoded;
    malformed[2U] = 0U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
    malformed = encoded;
    std::fill(malformed.begin() + 20U, malformed.begin() + 52U, 0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_query(malformed));
    });
}

void test_lookup_golden_and_selector_rules() {
    toolbusd::RuntimeOperationLookup lookup;
    lookup.daemon_instance_id = filled<16>(1U);
    lookup.lease_id = filled<16>(2U);
    lookup.kind = toolbusd::RuntimeOperationKind::GpioWrite;
    lookup.owner_key_id = "o";
    lookup.idempotency_key = "k";
    const auto encoded = toolbusd::encode_ipc_runtime_operation_lookup(lookup);
    std::vector<std::uint8_t> golden{0x01, 0x00, 0x2c, 0x00};
    golden.insert(golden.end(), 16U, 1U);
    golden.insert(golden.end(), 16U, 2U);
    const std::vector<std::uint8_t> suffix{
        0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 'o', 'k'};
    golden.insert(golden.end(), suffix.begin(), suffix.end());
    CHECK(encoded == golden);
    const auto decoded =
        toolbusd::decode_ipc_runtime_operation_lookup(encoded);
    CHECK(decoded.kind == toolbusd::RuntimeOperationKind::GpioWrite);
    CHECK(decoded.owner_key_id == "o");
    CHECK(decoded.idempotency_key == "k");

    auto malformed = encoded;
    malformed[36U] = 0xffU;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_lookup(malformed));
    });
    malformed = encoded;
    malformed[37U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_lookup(malformed));
    });
    malformed = encoded;
    malformed[42U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_lookup(malformed));
    });
    malformed = encoded;
    malformed[36U] = static_cast<std::uint8_t>(
        toolbusd::RuntimeOperationKind::ControlRelease);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_lookup(malformed));
    });
    malformed = encoded;
    malformed.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_lookup(malformed));
    });

    lookup.kind = toolbusd::RuntimeOperationKind::ControlRelease;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_operation_lookup(lookup));
    });
    lookup.idempotency_key = "release:v1";
    const auto release =
        toolbusd::decode_ipc_runtime_operation_lookup(
            toolbusd::encode_ipc_runtime_operation_lookup(lookup));
    CHECK(release.idempotency_key == "release:v1");
}

void test_outcome_golden_and_state_matrix() {
    const auto outcome = committed_gpio();
    const auto encoded =
        toolbusd::encode_ipc_runtime_operation_outcome(outcome);
    std::vector<std::uint8_t> golden{
        0x01, 0x00, 0x58, 0x00, 0x01, 0x02, 0x00, 0x02};
    golden.insert(golden.end(), 32U, 0xa5U);
    golden.insert(golden.end(), 16U, 0xb1U);
    golden.insert(golden.end(), 16U, 0xc2U);
    const std::vector<std::uint8_t> suffix{
        0x04, 0x03, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00};
    golden.insert(golden.end(), suffix.begin(), suffix.end());
    CHECK(encoded == golden);
    const auto decoded =
        toolbusd::decode_ipc_runtime_operation_outcome(encoded);
    CHECK(decoded.object_id == 0x11223344U);
    CHECK(decoded.lease_id == filled<16>(0xb1U));
    CHECK(decoded.expected_node_uuid == filled<16>(0xc2U));
    CHECK(decoded.resource_id == 0x01020304U);
    CHECK(decoded.value);
    CHECK(!decoded.replayed);

    toolbusd::RuntimeOperationOutcome pending_without_scope;
    pending_without_scope.kind = toolbusd::RuntimeOperationKind::GpioWrite;
    pending_without_scope.state = toolbusd::RuntimeOperationState::Pending;
    pending_without_scope.operation_id = filled<32>(2U);
    expect_failure([&] {
        static_cast<void>(toolbusd::encode_ipc_runtime_operation_outcome(
            pending_without_scope));
    });

    toolbusd::RuntimeOperationOutcome unknown;
    unknown.kind = toolbusd::RuntimeOperationKind::ControlRelease;
    unknown.state = toolbusd::RuntimeOperationState::Unknown;
    unknown.operation_id = filled<32>(3U);
    unknown.lease_id = filled<16>(4U);
    unknown.expected_node_uuid = filled<16>(5U);
    unknown.resource_id = 6U;
    unknown.error = toolbusd::RuntimeOperationError::Backend;
    for (const auto recovery : {
             toolbusd::RuntimeOperationRecovery::SafeClosed,
             toolbusd::RuntimeOperationRecovery::ScopeBlocked,
             toolbusd::RuntimeOperationRecovery::AwaitingReboot,
             toolbusd::RuntimeOperationRecovery::NodeRebootConfirmed}) {
        unknown.recovery = recovery;
        CHECK(toolbusd::decode_ipc_runtime_operation_outcome(
                  toolbusd::encode_ipc_runtime_operation_outcome(unknown))
                  .recovery == recovery);
    }

    toolbusd::RuntimeOperationOutcome expired;
    expired.kind = toolbusd::RuntimeOperationKind::Unknown;
    expired.state = toolbusd::RuntimeOperationState::ExpiredUnknown;
    expired.operation_id = filled<32>(4U);
    expired.error = toolbusd::RuntimeOperationError::HistoryExpired;
    expired.recovery = toolbusd::RuntimeOperationRecovery::None;
    CHECK(toolbusd::encode_ipc_runtime_operation_outcome(expired).size() ==
          88U);
    expired.lease_id = filled<16>(1U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_operation_outcome(expired));
    });
    expired.lease_id.fill(0U);
    expired.recovery = toolbusd::RuntimeOperationRecovery::ScopeBlocked;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_operation_outcome(expired));
    });
    expired.recovery = toolbusd::RuntimeOperationRecovery::None;
    expired.kind = static_cast<toolbusd::RuntimeOperationKind>(0xfeU);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_operation_outcome(expired));
    });
    expired.kind = toolbusd::RuntimeOperationKind::Unknown;
    expired.state = toolbusd::RuntimeOperationState::Unknown;
    expired.error = toolbusd::RuntimeOperationError::Backend;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_operation_outcome(expired));
    });

    auto malformed = encoded;
    malformed[4U] = 0xffU;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed[5U] = 0xffU;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed[6U] = 0xffU;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed[7U] |= 0x80U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed[80U] = 0xffU;
    malformed[81U] = 0xffU;
    malformed[7U] |= 0x04U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed[83U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed.pop_back();
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
    malformed = encoded;
    malformed.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_operation_outcome(malformed));
    });
}

std::vector<std::uint8_t> receive_exact(int socket, std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    std::size_t offset = 0U;
    while (offset < size) {
        const auto received = ::recv(socket, bytes.data() + offset,
                                     size - offset, 0);
        if (received <= 0) break;
        offset += static_cast<std::size_t>(received);
    }
    bytes.resize(offset);
    return bytes;
}

std::vector<std::uint8_t> receive_frame(int socket) {
    const auto header = receive_exact(socket, 4U);
    CHECK(header.size() == 4U);
    if (header.size() != 4U) return {};
    const std::size_t size = static_cast<std::size_t>(header[0U]) |
                             (static_cast<std::size_t>(header[1U]) << 8U) |
                             (static_cast<std::size_t>(header[2U]) << 16U) |
                             (static_cast<std::size_t>(header[3U]) << 24U);
    return receive_exact(socket, size);
}

void send_frame(int socket, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> wire(4U);
    const auto size = static_cast<std::uint32_t>(body.size());
    for (unsigned index = 0U; index < 4U; ++index) {
        wire[index] = static_cast<std::uint8_t>(size >> (index * 8U));
    }
    wire.insert(wire.end(), body.begin(), body.end());
    std::size_t offset = 0U;
    while (offset < wire.size()) {
        const auto sent =
            ::send(socket, wire.data() + offset, wire.size() - offset, 0);
        CHECK(sent > 0);
        if (sent <= 0) return;
        offset += static_cast<std::size_t>(sent);
    }
}

void expect_tagged_reader_failure(
    toolbusd::IpcRequestKind kind,
    const std::vector<std::uint8_t>& payload) {
    int sockets[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(kind)};
    body.insert(body.end(), payload.begin(), payload.end());
    send_frame(sockets[0], body);
    bool rejected = false;
    try {
        static_cast<void>(toolbusd::read_ipc_request(sockets[1]));
    } catch (const toolbusd::IpcException& error) {
        rejected = error.has_request_kind() && error.request_kind() == kind;
    }
    CHECK(rejected);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void test_request_kinds_and_fragmented_response() {
    int sockets[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::RuntimeOperationQuery query;
    query.daemon_instance_id = filled<16>(1U);
    query.operation_id = filled<32>(2U);
    query.owner_key_id = "owner";
    toolbusd::write_ipc_runtime_operation_query_request(sockets[0], query);
    auto request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind == toolbusd::IpcRequestKind::RuntimeOperationQuery);
    CHECK(request.runtime_operation_query.owner_key_id == "owner");
    ::close(sockets[0]);
    ::close(sockets[1]);

    toolbusd::RuntimeGpioWriteRequest write;
    write.daemon_instance_id = filled<16>(1U);
    write.lease_id = filled<16>(2U);
    write.expected_node_uuid = filled<16>(3U);
    write.owner_key_id = "owner";
    write.node_id = 1U;
    write.resource_id = 2U;
    write.idempotency_key = "write:1";
    write.value = true;
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_gpio_write_operation_request(
        sockets[0], write);
    request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind ==
          toolbusd::IpcRequestKind::RuntimeGpioWriteOperation);
    CHECK(request.runtime_gpio_write.resource_id == 2U);
    ::close(sockets[0]);
    ::close(sockets[1]);

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = filled<16>(1U);
    release.lease_id = filled<16>(2U);
    release.owner_key_id = "owner";
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_control_release_operation_request(
        sockets[0], release);
    request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind ==
          toolbusd::IpcRequestKind::RuntimeControlReleaseOperation);
    CHECK(request.runtime_control_release.owner_key_id == "owner");
    ::close(sockets[0]);
    ::close(sockets[1]);

    toolbusd::RuntimeOperationLookup lookup;
    lookup.daemon_instance_id = filled<16>(1U);
    lookup.lease_id = filled<16>(2U);
    lookup.owner_key_id = "owner";
    lookup.idempotency_key = "release:v1";
    lookup.kind = toolbusd::RuntimeOperationKind::ControlRelease;
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_operation_lookup_request(sockets[0], lookup);
    request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind == toolbusd::IpcRequestKind::RuntimeOperationLookup);
    CHECK(request.runtime_operation_lookup.kind ==
          toolbusd::RuntimeOperationKind::ControlRelease);
    ::close(sockets[0]);
    ::close(sockets[1]);

    auto malformed_query =
        toolbusd::encode_ipc_runtime_operation_query(query);
    malformed_query[54U] = 1U;
    expect_tagged_reader_failure(
        toolbusd::IpcRequestKind::RuntimeOperationQuery, malformed_query);
    auto malformed_lookup =
        toolbusd::encode_ipc_runtime_operation_lookup(lookup);
    malformed_lookup.push_back(0U);
    expect_tagged_reader_failure(
        toolbusd::IpcRequestKind::RuntimeOperationLookup,
        malformed_lookup);
    auto malformed_write = toolbusd::encode_ipc_runtime_gpio_write(write);
    malformed_write.pop_back();
    expect_tagged_reader_failure(
        toolbusd::IpcRequestKind::RuntimeGpioWriteOperation,
        malformed_write);
    auto malformed_release =
        toolbusd::encode_ipc_runtime_control_release(release);
    malformed_release.push_back(0U);
    expect_tagged_reader_failure(
        toolbusd::IpcRequestKind::RuntimeControlReleaseOperation,
        malformed_release);

    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto outcome = committed_gpio(true);
    const auto body =
        toolbusd::encode_ipc_runtime_operation_outcome(outcome);
    std::vector<std::uint8_t> wire{static_cast<std::uint8_t>(
        toolbusd::IpcStatus::Ok)};
    const auto size = static_cast<std::uint32_t>(body.size());
    for (unsigned index = 0; index < 4U; ++index) {
        wire.push_back(static_cast<std::uint8_t>(size >> (index * 8U)));
    }
    wire.insert(wire.end(), body.begin(), body.end());
    std::atomic<bool> writer_ok{true};
    std::thread writer([&] {
        for (const auto byte : wire) {
            if (::send(sockets[0], &byte, 1U, 0) != 1) {
                writer_ok.store(false);
                break;
            }
        }
        ::close(sockets[0]);
    });
    const auto response = toolbusd::read_ipc_response(sockets[1]);
    writer.join();
    CHECK(writer_ok.load());
    CHECK(response.status == toolbusd::IpcStatus::Ok);
    CHECK(toolbusd::decode_ipc_runtime_operation_outcome(response.body)
              .replayed);
    ::close(sockets[1]);
}

void test_json_is_exact() {
    remotebsp::RuntimeOperationOutcome outcome;
    outcome.operation_id.fill(0xabU);
    outcome.lease_id = filled<16>(0x11U);
    outcome.expected_node_uuid = filled<16>(0x22U);
    outcome.resource_id = 3U;
    outcome.kind = remotebsp::RuntimeOperationKind::GpioWrite;
    outcome.state = remotebsp::RuntimeOperationState::Committed;
    outcome.recovery = remotebsp::RuntimeOperationRecovery::None;
    outcome.object_id = 7U;
    outcome.value = false;
    std::ostringstream output;
    remotebsp::cli_json::write_runtime_operation_outcome(
        output, "runtime-operation-status", outcome);
    CHECK(output.str() ==
          "{\"schema_version\":1,\"command\":\"runtime-operation-status\","
          "\"data\":{\"operation_id\":\""
          "abababababababababababababababababababababababababababababababab"
          "\",\"lease_id\":\"11111111111111111111111111111111\""
          ",\"expected_node_uuid\":\"22222222222222222222222222222222\""
          ",\"resource_id\":3"
          ",\"kind\":\"gpio_write\",\"state\":\"committed\","
          "\"replayed\":false,\"recovery\":\"none\",\"object_id\":7,"
          "\"value\":false,\"error_code\":null}}\n");

    outcome.kind = remotebsp::RuntimeOperationKind::Unknown;
    outcome.state = remotebsp::RuntimeOperationState::ExpiredUnknown;
    outcome.recovery = remotebsp::RuntimeOperationRecovery::None;
    outcome.object_id.reset();
    outcome.value.reset();
    outcome.lease_id.reset();
    outcome.expected_node_uuid.reset();
    outcome.resource_id.reset();
    outcome.error = remotebsp::RuntimeOperationError::HistoryExpired;
    outcome.replayed = true;
    output.str({});
    output.clear();
    remotebsp::cli_json::write_runtime_operation_outcome(
        output, "runtime-operation-status", outcome);
    CHECK(output.str().find("\"kind\":null") != std::string::npos);
    CHECK(output.str().find("\"lease_id\":null") != std::string::npos);
    CHECK(output.str().find("\"expected_node_uuid\":null") !=
          std::string::npos);
    CHECK(output.str().find("\"resource_id\":null") != std::string::npos);
    CHECK(output.str().find("\"error_code\":\"history_expired\"") !=
          std::string::npos);
}

remotebsp::RuntimeOperationOutcome invoke_client_once(
    toolbusd::IpcRequestKind expected_kind,
    const toolbusd::RuntimeOperationOutcome& response_outcome,
    const std::function<remotebsp::RuntimeOperationOutcome(
        const remotebsp::Client&)>& call) {
    static unsigned sequence = 0U;
    const std::string path = "/tmp/remotebsp-operation-contract-" +
                             std::to_string(::getpid()) + "-" +
                             std::to_string(++sequence) + ".sock";
    static_cast<void>(::unlink(path.c_str()));
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    CHECK(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    CHECK(::bind(listener, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)) == 0);
    CHECK(::listen(listener, 1) == 0);
    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        const int peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0) {
            server_ok.store(false);
            return;
        }
        const auto request = receive_frame(peer);
        if (request.empty() ||
            request[0U] != static_cast<std::uint8_t>(expected_kind)) {
            server_ok.store(false);
        } else {
            try {
                toolbusd::write_ipc_response(
                    peer, toolbusd::IpcStatus::Ok,
                    toolbusd::encode_ipc_runtime_operation_outcome(
                        response_outcome));
            } catch (const std::exception&) {
                server_ok.store(false);
            }
        }
        ::close(peer);
    });
    remotebsp::RuntimeOperationOutcome result;
    try {
        const remotebsp::Client client(path, 1U);
        result = call(client);
    } catch (...) {
        server.join();
        ::close(listener);
        static_cast<void>(::unlink(path.c_str()));
        throw;
    }
    server.join();
    CHECK(server_ok.load());
    ::close(listener);
    static_cast<void>(::unlink(path.c_str()));
    return result;
}

void test_client_api_and_query_replay_contract() {
    const auto daemon = filled<16>(1U);
    const auto lease = filled<16>(2U);
    const auto node = filled<16>(3U);

    auto write_response = committed_gpio(false);
    const auto submitted = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimeGpioWriteOperation, write_response,
        [&](const remotebsp::Client& client) {
            return client.runtime_gpio_write_operation(
                daemon, lease, node, "owner", 7U, "write:1", true);
        });
    CHECK(submitted.object_id == 0x11223344U);
    CHECK(!submitted.replayed);

    write_response.replayed = true;
    const auto status = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimeOperationQuery, write_response,
        [&](const remotebsp::Client& client) {
            return client.runtime_operation_status(
                daemon, "owner", write_response.operation_id);
        });
    CHECK(status.replayed);

    const auto lookup = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimeOperationLookup, write_response,
        [&](const remotebsp::Client& client) {
            return client.runtime_operation_lookup(
                daemon, "owner", remotebsp::RuntimeOperationKind::GpioWrite,
                lease, "write:1");
        });
    CHECK(lookup.replayed);

    write_response.replayed = false;
    expect_failure([&] {
        static_cast<void>(invoke_client_once(
            toolbusd::IpcRequestKind::RuntimeOperationQuery, write_response,
            [&](const remotebsp::Client& client) {
                return client.runtime_operation_status(
                    daemon, "owner", write_response.operation_id);
            }));
    });

    toolbusd::RuntimeOperationOutcome release_response;
    release_response.kind = toolbusd::RuntimeOperationKind::ControlRelease;
    release_response.state = toolbusd::RuntimeOperationState::Committed;
    release_response.recovery = toolbusd::RuntimeOperationRecovery::SafeClosed;
    release_response.operation_id = filled<32>(9U);
    release_response.lease_id = lease;
    release_response.expected_node_uuid = node;
    release_response.resource_id = 7U;
    const auto released = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimeControlReleaseOperation,
        release_response, [&](const remotebsp::Client& client) {
            return client.runtime_control_release_operation(
                daemon, lease, "owner");
        });
    CHECK(released.kind == remotebsp::RuntimeOperationKind::ControlRelease);
    CHECK(released.state == remotebsp::RuntimeOperationState::Committed);

    toolbusd::RuntimeOperationOutcome pwm_response;
    pwm_response.kind = toolbusd::RuntimeOperationKind::PwmConfigure;
    pwm_response.state = toolbusd::RuntimeOperationState::Committed;
    pwm_response.operation_id = filled<32>(0x31U);
    pwm_response.lease_id = lease;
    pwm_response.expected_node_uuid = node;
    pwm_response.resource_id = 0x06000000U;
    pwm_response.object_id = 23U;
    pwm_response.frequency_hz = 20000U;
    pwm_response.duty = 4200U;
    pwm_response.active_low = true;
    const auto pwm = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimePwmConfigureOperation, pwm_response,
        [&](const remotebsp::Client& client) {
            return client.runtime_pwm_configure_operation(
                daemon, lease, node, "owner", 0x06000000U,
                "pwm:configure:1", 20000U, 4200U, true);
        });
    CHECK(pwm.kind == remotebsp::RuntimeOperationKind::PwmConfigure);
    CHECK(pwm.frequency_hz == 20000U);
    CHECK(pwm.duty == 4200U);
    CHECK(pwm.active_low == true);

    pwm_response.kind = toolbusd::RuntimeOperationKind::PwmStop;
    pwm_response.recovery = toolbusd::RuntimeOperationRecovery::SafeClosed;
    pwm_response.frequency_hz = 0U;
    pwm_response.duty = 0U;
    pwm_response.active_low = false;
    const auto stopped = invoke_client_once(
        toolbusd::IpcRequestKind::RuntimePwmStopOperation, pwm_response,
        [&](const remotebsp::Client& client) {
            return client.runtime_pwm_stop_operation(
                daemon, lease, node, "owner", 0x06000000U, "pwm:stop:1");
        });
    CHECK(stopped.kind == remotebsp::RuntimeOperationKind::PwmStop);
    CHECK(stopped.recovery == remotebsp::RuntimeOperationRecovery::SafeClosed);
}

void test_pwm_request_roundtrip_and_strict_decode() {
    toolbusd::RuntimePwmConfigureRequest configure;
    configure.daemon_instance_id = filled<16>(0x11U);
    configure.lease_id = filled<16>(0x22U);
    configure.expected_node_uuid = filled<16>(0x33U);
    configure.owner_key_id = "owner";
    configure.node_id = 7U;
    configure.resource_id = 0x06000000U;
    configure.idempotency_key = "pwm:configure:1";
    configure.frequency_hz = 25000U;
    configure.duty = 3750U;
    configure.active_low = true;
    const auto encoded = toolbusd::encode_ipc_runtime_pwm_request(configure);
    const auto decoded = toolbusd::decode_ipc_runtime_pwm_request(encoded);
    CHECK(decoded.daemon_instance_id == configure.daemon_instance_id);
    CHECK(decoded.lease_id == configure.lease_id);
    CHECK(decoded.expected_node_uuid == configure.expected_node_uuid);
    CHECK(decoded.permissions == toolbusd::kRuntimePermissionPwmWrite);
    CHECK(decoded.node_id == configure.node_id);
    CHECK(decoded.resource_id == configure.resource_id);
    CHECK(decoded.owner_key_id == configure.owner_key_id);
    CHECK(decoded.idempotency_key == configure.idempotency_key);
    CHECK(decoded.frequency_hz == 25000U);
    CHECK(decoded.duty == 3750U);
    CHECK(decoded.active_low);

    auto malformed = encoded;
    malformed.pop_back();
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });
    malformed = encoded;
    malformed.push_back(0U);
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });
    malformed = encoded;
    malformed[60U] = malformed[61U] = malformed[62U] = malformed[63U] = 0U;
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });
    malformed = encoded;
    malformed[64U] = 0x11U; malformed[65U] = 0x27U;
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });
    malformed = encoded;
    malformed[66U] = 2U;
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });
    malformed = encoded;
    malformed[69U] = 1U;
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_request(malformed)); });

    toolbusd::RuntimePwmStopRequest stop;
    stop.daemon_instance_id = configure.daemon_instance_id;
    stop.lease_id = configure.lease_id;
    stop.expected_node_uuid = configure.expected_node_uuid;
    stop.owner_key_id = configure.owner_key_id;
    stop.node_id = configure.node_id;
    stop.resource_id = configure.resource_id;
    stop.idempotency_key = "pwm:stop:1";
    const auto stop_encoded = toolbusd::encode_ipc_runtime_pwm_stop_request(stop);
    const auto stop_decoded = toolbusd::decode_ipc_runtime_pwm_stop_request(stop_encoded);
    CHECK(stop_decoded.owner_key_id == stop.owner_key_id);
    CHECK(stop_decoded.idempotency_key == stop.idempotency_key);
    CHECK(stop_decoded.resource_id == stop.resource_id);
    malformed = stop_encoded;
    malformed[62U] = 1U;
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_stop_request(malformed)); });
    malformed = stop_encoded;
    malformed.pop_back();
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_stop_request(malformed)); });
    malformed = stop_encoded;
    malformed.push_back(0U);
    expect_failure([&] { static_cast<void>(toolbusd::decode_ipc_runtime_pwm_stop_request(malformed)); });

    configure.owner_key_id.assign(toolbusd::kMaximumRuntimeControlIdentityBytes + 1U, 'a');
    expect_failure([&] { static_cast<void>(toolbusd::encode_ipc_runtime_pwm_request(configure)); });
    stop.idempotency_key.assign(toolbusd::kMaximumRuntimeControlIdempotencyBytes + 1U, 'b');
    expect_failure([&] { static_cast<void>(toolbusd::encode_ipc_runtime_pwm_stop_request(stop)); });
}

void test_timed_bitstream_request_roundtrip_and_bounds() {
    toolbusd::RuntimeTimedBitstreamConfigureRequest configure;
    configure.daemon_instance_id=filled<16>(1U);configure.lease_id=filled<16>(2U);
    configure.expected_node_uuid=filled<16>(3U);configure.owner_key_id="owner";
    configure.node_id=7U;configure.resource_id=0x0A000000U;configure.idempotency_key="cfg";
    configure.bit_period_ns=1250U;configure.zero_high_ns=350U;
    configure.one_high_ns=700U;configure.reset_time_us=80U;
    auto encoded=toolbusd::encode_ipc_runtime_timed_bitstream_configure(configure);
    auto decoded=toolbusd::decode_ipc_runtime_timed_bitstream_configure(encoded);
    CHECK(decoded.bit_period_ns==1250U&&decoded.owner_key_id=="owner");
    auto malformed=encoded;malformed[78U]=1U;
    expect_failure([&]{static_cast<void>(toolbusd::decode_ipc_runtime_timed_bitstream_configure(malformed));});

    toolbusd::RuntimeTimedBitstreamFrameRequest frame;
    frame.daemon_instance_id=configure.daemon_instance_id;frame.lease_id=configure.lease_id;
    frame.expected_node_uuid=configure.expected_node_uuid;frame.owner_key_id="owner";
    frame.node_id=7U;frame.resource_id=0x0A000000U;frame.idempotency_key="frame";
    frame.bit_count=16176U;frame.data.assign(2022U,0xA5U);
    encoded=toolbusd::encode_ipc_runtime_timed_bitstream_frame(frame);
    const auto frame_decoded=toolbusd::decode_ipc_runtime_timed_bitstream_frame(encoded);
    CHECK(frame_decoded.data==frame.data&&frame_decoded.bit_count==frame.bit_count);
    malformed=encoded;malformed[66U]=1U;
    expect_failure([&]{static_cast<void>(toolbusd::decode_ipc_runtime_timed_bitstream_frame(malformed));});
    frame.bit_count=16177U;frame.data.push_back(0U);
    expect_failure([&]{static_cast<void>(toolbusd::encode_ipc_runtime_timed_bitstream_frame(frame));});

    toolbusd::RuntimeTimedBitstreamStopRequest stop;
    stop.daemon_instance_id=configure.daemon_instance_id;stop.lease_id=configure.lease_id;
    stop.expected_node_uuid=configure.expected_node_uuid;stop.owner_key_id="owner";
    stop.node_id=7U;stop.resource_id=0x0A000000U;stop.idempotency_key="stop";
    encoded=toolbusd::encode_ipc_runtime_timed_bitstream_stop(stop);
    CHECK(toolbusd::decode_ipc_runtime_timed_bitstream_stop(encoded).idempotency_key=="stop");
}

void test_bus_resource_reset_request_roundtrip_and_dispatch() {
    static_assert(static_cast<std::uint8_t>(
                      toolbusd::IpcRequestKind::RuntimeBusResourceResetOperation) ==
                      27U,
                  "总线资源复位IPC kind必须保持稳定");
    toolbusd::RuntimeBusResourceResetRequest reset;
    reset.daemon_instance_id = filled<16>(1U);
    reset.lease_id = filled<16>(2U);
    reset.expected_node_uuid = filled<16>(3U);
    reset.owner_key_id = "owner";
    reset.permissions = toolbusd::kRuntimePermissionBusReset;
    reset.node_id = 7U;
    reset.resource_id = 0x02000001U;
    reset.idempotency_key = "bus-reset:1";

    const auto encoded =
        toolbusd::encode_ipc_runtime_bus_resource_reset(reset);
    const auto decoded =
        toolbusd::decode_ipc_runtime_bus_resource_reset(encoded);
    CHECK(decoded.daemon_instance_id == reset.daemon_instance_id);
    CHECK(decoded.lease_id == reset.lease_id);
    CHECK(decoded.expected_node_uuid == reset.expected_node_uuid);
    CHECK(decoded.owner_key_id == reset.owner_key_id);
    CHECK(decoded.permissions == reset.permissions);
    CHECK(decoded.node_id == reset.node_id);
    CHECK(decoded.resource_id == reset.resource_id);
    CHECK(decoded.idempotency_key == reset.idempotency_key);

    auto malformed = encoded;
    malformed[62U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_bus_resource_reset(malformed));
    });
    malformed = encoded;
    malformed[0U] = 0xffU;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_bus_resource_reset(malformed));
    });
    malformed = encoded;
    malformed.pop_back();
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_bus_resource_reset(malformed));
    });
    malformed = encoded;
    malformed.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_bus_resource_reset(malformed));
    });

    int sockets[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_bus_resource_reset_operation_request(
        sockets[0], reset);
    const auto request = toolbusd::read_ipc_request(sockets[1]);
    CHECK(request.kind ==
          toolbusd::IpcRequestKind::RuntimeBusResourceResetOperation);
    CHECK(request.runtime_bus_resource_reset.resource_id == reset.resource_id);
    CHECK(request.runtime_bus_resource_reset.idempotency_key ==
          reset.idempotency_key);
    ::close(sockets[0]);
    ::close(sockets[1]);

    malformed = encoded;
    malformed[62U] = 1U;
    expect_tagged_reader_failure(
        toolbusd::IpcRequestKind::RuntimeBusResourceResetOperation,
        malformed);

    reset.owner_key_id.clear();
    expect_failure([&] {
        static_cast<void>(
            toolbusd::encode_ipc_runtime_bus_resource_reset(reset));
    });
}

void test_motion_group_cancel_payload_is_strict_and_lossless() {
    toolbusd::RuntimeMotionGroupCancelRequest request;
    request.daemon_instance_id = filled<16>(0x11U);
    request.lease_id = filled<16>(0x22U);
    request.owner_key_id = "runtime-owner";
    request.idempotency_key = "motion-stop-7";
    request.transaction_id = 0x1122334455667788ULL;
    request.group_id = 7U;
    request.plan_generation = 3U;
    request.deadline_ms = 1800U;
    const auto encoded = toolbusd::encode_ipc_runtime_motion_group_cancel(request);
    const auto decoded = toolbusd::decode_ipc_runtime_motion_group_cancel(encoded);
    CHECK(decoded.daemon_instance_id == request.daemon_instance_id);
    CHECK(decoded.lease_id == request.lease_id);
    CHECK(decoded.owner_key_id == request.owner_key_id);
    CHECK(decoded.idempotency_key == request.idempotency_key);
    CHECK(decoded.transaction_id == request.transaction_id);
    CHECK(decoded.group_id == request.group_id);
    CHECK(decoded.plan_generation == request.plan_generation);
    CHECK(decoded.deadline_ms == request.deadline_ms);

    int sockets[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_motion_group_cancel_operation_request(
        sockets[0], request);
    const auto dispatched = toolbusd::read_ipc_request(sockets[1]);
    CHECK(dispatched.kind ==
          toolbusd::IpcRequestKind::RuntimeMotionGroupCancelOperation);
    CHECK(dispatched.runtime_motion_group_cancel.transaction_id ==
          request.transaction_id);
    ::close(sockets[0]);
    ::close(sockets[1]);

    auto malformed = encoded;
    malformed[58U] = 1U;
    expect_failure([&] {
        static_cast<void>(toolbusd::decode_ipc_runtime_motion_group_cancel(malformed));
    });
    malformed = encoded;
    std::fill_n(malformed.begin() + 20U, 16U, 0U);
    expect_failure([&] {
        static_cast<void>(toolbusd::decode_ipc_runtime_motion_group_cancel(malformed));
    });
    auto invalid = request;
    invalid.deadline_ms = toolbusd::kMaximumRuntimeControlTtlMs + 1U;
    expect_failure([&] {
        static_cast<void>(toolbusd::encode_ipc_runtime_motion_group_cancel(invalid));
    });

    toolbusd::RuntimeMotionGroupLeaseAcquireRequest acquire;
    acquire.daemon_instance_id = request.daemon_instance_id;
    acquire.lease_id = request.lease_id;
    acquire.owner_key_id = request.owner_key_id;
    acquire.transaction_id = request.transaction_id;
    acquire.group_id = request.group_id;
    acquire.plan_generation = request.plan_generation;
    acquire.ttl_ms = 5000U;
    const auto encoded_acquire =
        toolbusd::encode_ipc_runtime_motion_group_lease_acquire(acquire);
    const auto decoded_acquire =
        toolbusd::decode_ipc_runtime_motion_group_lease_acquire(
            encoded_acquire);
    CHECK(decoded_acquire.transaction_id==acquire.transaction_id);
    CHECK(decoded_acquire.ttl_ms==acquire.ttl_ms);

    toolbusd::RuntimeMotionGroupLeaseReleaseRequest release;
    release.daemon_instance_id = request.daemon_instance_id;
    release.lease_id = request.lease_id;
    release.owner_key_id = request.owner_key_id;
    CHECK(toolbusd::decode_ipc_runtime_motion_group_lease_release(
        toolbusd::encode_ipc_runtime_motion_group_lease_release(release)).owner_key_id==release.owner_key_id);

    toolbusd::RuntimeMotionGroupOperationOutcome outcome;
    outcome.operation.kind = toolbusd::RuntimeOperationKind::MotionGroupCancel;
    outcome.operation.state = toolbusd::RuntimeOperationState::Committed;
    outcome.operation.recovery = toolbusd::RuntimeOperationRecovery::SafeClosed;
    outcome.operation.operation_id = filled<32>(0x33U);
    outcome.operation.lease_id = request.lease_id;
    outcome.transaction_id = request.transaction_id;
    outcome.group_id = request.group_id;
    outcome.plan_generation = request.plan_generation;
    const auto encoded_outcome =
        toolbusd::encode_ipc_runtime_motion_group_operation_outcome(outcome);
    const auto decoded_outcome =
        toolbusd::decode_ipc_runtime_motion_group_operation_outcome(
            encoded_outcome);
    CHECK(decoded_outcome.transaction_id==request.transaction_id);
    CHECK(decoded_outcome.group_id==request.group_id);

    auto bad_acquire = encoded_acquire;
    bad_acquire[57U] = 1U;
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_motion_group_lease_acquire(
                bad_acquire));
    });
    bad_acquire = encoded_acquire;
    bad_acquire.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_motion_group_lease_acquire(
                bad_acquire));
    });
    auto bad_outcome = encoded_outcome;
    bad_outcome.push_back(0U);
    expect_failure([&] {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_motion_group_operation_outcome(
                bad_outcome));
    });
}

}  // namespace

int main() {
    test_query_golden_and_strict_decode();
    test_lookup_golden_and_selector_rules();
    test_outcome_golden_and_state_matrix();
    test_request_kinds_and_fragmented_response();
    test_json_is_exact();
    test_client_api_and_query_replay_contract();
    test_pwm_request_roundtrip_and_strict_decode();
    test_timed_bitstream_request_roundtrip_and_bounds();
    test_bus_resource_reset_request_roundtrip_and_dispatch();
    test_motion_group_cancel_payload_is_strict_and_lossless();
    if (failures != 0) {
        std::cerr << failures << " 项 Runtime operation IPC 测试失败\n";
        return 1;
    }
    std::cout << "Runtime operation IPC 测试通过\n";
    return 0;
}
