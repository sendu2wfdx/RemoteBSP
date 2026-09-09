#include "remotebsp/toolbusd/runtime_control.hpp"
#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <future>
#include <optional>
#include <thread>

namespace {

using namespace remotebsp;

template <typename Callback>
void expect_error(toolbusd::RuntimeControlError expected,
                  Callback callback) {
    try {
        callback();
        assert(false);
    } catch (const toolbusd::RuntimeControlException& error) {
        assert(error.code() == expected);
    }
}

toolbusd::RuntimeGpioWriteRequest request(
    const std::array<std::uint8_t, 16>& daemon,
    const std::array<std::uint8_t, 16>& lease) {
    toolbusd::RuntimeGpioWriteRequest value;
    value.daemon_instance_id = daemon;
    value.lease_id = lease;
    value.owner_key_id = "operator-a";
    value.node_id = 3U;
    value.resource_id = 0x01000005U;
    value.idempotency_key = "gpio-command-1";
    value.value = true;
    return value;
}

toolbusd::RuntimeControlAcquireRequest acquire_request(
    const toolbusd::RuntimeGpioWriteRequest& command,
    std::uint32_t ttl_ms = 1000U) {
    toolbusd::RuntimeControlAcquireRequest value;
    value.daemon_instance_id = command.daemon_instance_id;
    value.lease_id = command.lease_id;
    value.owner_key_id = command.owner_key_id;
    value.permissions = command.permissions;
    value.node_id = command.node_id;
    value.resource_id = command.resource_id;
    value.ttl_ms = ttl_ms;
    return value;
}

protocol::ResourceDescriptor descriptor(
    std::uint32_t resource_id = 0x01000005U) {
    return {resource_id, protocol::ResourceType::Gpio,
            static_cast<std::uint16_t>(resource_id & 0xffffU),
            protocol::kResourceFlagNative, 0U, 0U};
}

protocol::ResourceContract contract(
    std::uint32_t resource_id = 0x01000005U) {
    return {resource_id, protocol::kResourceContractVersion,
            static_cast<std::uint16_t>(
                protocol::kResourceAccessReadable |
                protocol::kResourceAccessWritable |
                protocol::kResourceAccessExclusiveWrite |
                protocol::kResourceAccessLeaseSupported),
            1000U, 100U, 10000U, 0U, 0U, 0U};
}

void check_per_scope_isolation_and_serialization() {
    toolbusd::RuntimeControlGate gate(4U, [] { return 1000000000ULL; });
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> first_lease{};
    std::array<std::uint8_t, 16> second_lease{};
    daemon[0] = 1U;
    first_lease[0] = 2U;
    second_lease[0] = 3U;
    auto first = request(daemon, first_lease);
    gate.acquire(acquire_request(first), daemon, 1U, descriptor(), contract());

    auto other = request(daemon, second_lease);
    other.owner_key_id = "operator-b";
    other.resource_id = 0x01000006U;
    other.idempotency_key = "other-scope";
    gate.acquire(acquire_request(other), daemon, 1U,
                 descriptor(other.resource_id), contract(other.resource_id));

    std::promise<void> first_started;
    std::promise<void> finish_first;
    auto finish = finish_first.get_future().share();
    auto first_future = std::async(std::launch::async, [&] {
        return gate.gpio_write(first, daemon, 1U, descriptor(), contract(),
                               [&](auto) {
                                   first_started.set_value();
                                   finish.wait();
                                   return 91U;
                               });
    });
    first_started.get_future().wait();

    auto same_scope = first;
    same_scope.idempotency_key = "same-scope-next";
    same_scope.value = false;
    auto same_future = std::async(std::launch::async, [&] {
        return gate.gpio_write(same_scope, daemon, 1U, descriptor(),
                               contract(), [](auto object) {
                                   assert(object == 91U);
                                   return 91U;
                               });
    });
    assert(same_future.wait_for(std::chrono::milliseconds(30)) ==
           std::future_status::timeout);

    const auto unrelated = gate.gpio_write(
        other, daemon, 1U, descriptor(other.resource_id),
        contract(other.resource_id), [](auto object) {
            assert(!object.has_value());
            return 92U;
        });
    assert(unrelated.object_id == 92U);
    finish_first.set_value();
    assert(first_future.get().object_id == 91U);
    assert(same_future.get().object_id == 91U);
}

void check_identity_permission_contract_and_idempotency() {
    std::uint64_t now = 1000000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; });
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 2U;
    auto command = request(daemon, lease);
    expect_error(toolbusd::RuntimeControlError::LeaseNotFound, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 7U, descriptor(), contract(),
            [](auto) { return 40U; }));
    });
    gate.acquire(acquire_request(command), daemon, 7U, descriptor(),
                 contract());
    std::uint32_t writes = 0U;
    const auto writer = [&](std::optional<std::uint32_t> object) {
        ++writes;
        assert(!object.has_value());
        return 41U;
    };
    const auto first = gate.gpio_write(
        command, daemon, 7U, descriptor(), contract(), writer);
    assert(first.object_id == 41U && first.value && !first.replayed);
    assert(writes == 1U && gate.active_lease_count() == 1U);
    const auto replay = gate.gpio_write(
        command, daemon, 7U, descriptor(), contract(), writer);
    assert(replay.object_id == 41U && replay.replayed && writes == 1U);

    auto conflict = command;
    conflict.value = false;
    expect_error(toolbusd::RuntimeControlError::IdempotencyConflict, [&] {
        static_cast<void>(gate.gpio_write(
            conflict, daemon, 7U, descriptor(), contract(), writer));
    });
    auto wrong_daemon = daemon;
    wrong_daemon[1] = 9U;
    expect_error(toolbusd::RuntimeControlError::DaemonIdentityMismatch, [&] {
        static_cast<void>(gate.gpio_write(
            command, wrong_daemon, 7U, descriptor(), contract(), writer));
    });
    auto denied = command;
    denied.idempotency_key = "gpio-command-2";
    denied.permissions = 0U;
    expect_error(toolbusd::RuntimeControlError::PermissionDenied, [&] {
        static_cast<void>(gate.gpio_write(
            denied, daemon, 7U, descriptor(), contract(), writer));
    });
    auto readonly = contract();
    readonly.access_flags = protocol::kResourceAccessReadable;
    expect_error(toolbusd::RuntimeControlError::ContractRejected, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 7U, descriptor(), readonly, writer));
    });
}

void check_scope_expiry_generation_and_reuse() {
    std::uint64_t now = 5000000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; });
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> first_lease{};
    std::array<std::uint8_t, 16> second_lease{};
    daemon[0] = 1U;
    first_lease[0] = 2U;
    second_lease[0] = 3U;
    auto first = request(daemon, first_lease);
    gate.acquire(acquire_request(first), daemon, 11U, descriptor(),
                 contract());
    static_cast<void>(gate.gpio_write(
        first, daemon, 11U, descriptor(), contract(),
        [](auto) { return 51U; }));

    auto second = request(daemon, second_lease);
    second.owner_key_id = "operator-b";
    second.idempotency_key = "gpio-command-b";
    expect_error(toolbusd::RuntimeControlError::LeaseConflict, [&] {
        gate.acquire(acquire_request(second), daemon, 11U, descriptor(),
                     contract());
    });

    auto next_generation = first;
    next_generation.idempotency_key = "gpio-command-generation";
    expect_error(toolbusd::RuntimeControlError::LeaseExpired, [&] {
        static_cast<void>(gate.gpio_write(
            next_generation, daemon, 12U, descriptor(), contract(),
            [](auto) { return 53U; }));
    });

    now += 1000000001ULL;
    gate.acquire(acquire_request(second), daemon, 12U, descriptor(),
                 contract());
    bool reused_old_object = false;
    const auto result = gate.gpio_write(
        second, daemon, 12U, descriptor(), contract(),
        [&](std::optional<std::uint32_t> object) {
            reused_old_object = object.has_value();
            return 61U;
        });
    assert(result.object_id == 61U && !reused_old_object);

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = second_lease;
    release.owner_key_id = "operator-b";
    gate.release(release, daemon);
    assert(gate.active_lease_count() == 0U);
    gate.release(release, daemon);
    expect_error(toolbusd::RuntimeControlError::LeaseNotFound, [&] {
        static_cast<void>(gate.gpio_write(
            second, daemon, 12U, descriptor(), contract(),
            [](auto) { return 62U; }));
    });
}

void check_ipc_round_trip_and_strict_lengths() {
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 2U;
    const auto command = request(daemon, lease);
    const auto acquire = acquire_request(command);
    const auto acquire_body =
        toolbusd::encode_ipc_runtime_control_acquire(acquire);
    const auto acquire_decoded =
        toolbusd::decode_ipc_runtime_control_acquire(acquire_body);
    assert(acquire_decoded.lease_id == lease);
    assert(acquire_decoded.ttl_ms == 1000U);
    const auto body = toolbusd::encode_ipc_runtime_gpio_write(command);
    const auto decoded = toolbusd::decode_ipc_runtime_gpio_write(body);
    assert(decoded.daemon_instance_id == daemon);
    assert(decoded.lease_id == lease);
    assert(decoded.owner_key_id == command.owner_key_id);
    assert(decoded.idempotency_key == command.idempotency_key);
    assert(decoded.node_id == command.node_id);
    assert(decoded.resource_id == command.resource_id);
    assert(decoded.value);

    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    toolbusd::write_ipc_runtime_control_acquire_request(sockets[0], acquire);
    const auto framed = toolbusd::read_ipc_request(sockets[1]);
    assert(framed.kind == toolbusd::IpcRequestKind::RuntimeControlAcquire);
    assert(framed.runtime_control_acquire.lease_id == lease);
    ::close(sockets[0]);
    ::close(sockets[1]);

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = "operator-a";
    const auto release_body =
        toolbusd::encode_ipc_runtime_control_release(release);
    assert(toolbusd::decode_ipc_runtime_control_release(release_body)
               .owner_key_id == "operator-a");

    const toolbusd::RuntimeGpioWriteResult result{
        toolbusd::kRuntimeControlIpcVersion, 81U, true, false};
    const auto result_body =
        toolbusd::encode_ipc_runtime_gpio_write_result(result);
    const auto result_round_trip =
        toolbusd::decode_ipc_runtime_gpio_write_result(result_body);
    assert(result_round_trip.object_id == 81U);
    assert(result_round_trip.value && !result_round_trip.replayed);

    auto malformed = body;
    malformed[44U] = 2U;
    try {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_gpio_write(malformed));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

}  // namespace

int main() {
    check_identity_permission_contract_and_idempotency();
    check_scope_expiry_generation_and_reuse();
    check_ipc_round_trip_and_strict_lengths();
    check_per_scope_isolation_and_serialization();
}
