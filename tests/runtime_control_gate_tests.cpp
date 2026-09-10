#include "remotebsp/toolbusd/runtime_control.hpp"
#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace remotebsp;

// 不能使用标准 assert：Release/NDEBUG 构建也必须真正执行每个断言。
#undef assert
#define assert(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("测试断言失败: " #condition);           \
        }                                                                     \
    } while (false)

const toolbusd::RuntimeControlGate::GpioSafeStopper noop_stop =
    [](std::uint32_t, std::uint64_t,
       const std::array<std::uint8_t, 16>&, std::uint32_t) {};
const toolbusd::RuntimeControlGate::GpioCloser noop_close =
    [](std::uint32_t, std::uint64_t,
       const std::array<std::uint8_t, 16>&, std::uint32_t) {};

void check_always(bool condition, const char* expression) {
    if (!condition) {
        throw std::runtime_error(
            std::string("测试检查失败: ") + expression);
    }
}

struct ThrowingCopyState {
    bool throw_on_copy{};
    std::uint32_t calls{};
};

// std::function 会复制其目标；用可控复制异常确定性验证“先准备、后占位”
// 的事务边界，而不依赖不可移植的全局 operator new 故障注入。
struct ThrowingCopyStopper {
    std::shared_ptr<ThrowingCopyState> state;

    explicit ThrowingCopyStopper(std::shared_ptr<ThrowingCopyState> value)
        : state(std::move(value)) {}

    ThrowingCopyStopper(const ThrowingCopyStopper& other)
        : state(other.state) {
        if (state->throw_on_copy) {
            throw std::runtime_error("模拟 stopper 复制失败");
        }
    }

    ThrowingCopyStopper(ThrowingCopyStopper&&) noexcept = default;
    ThrowingCopyStopper& operator=(const ThrowingCopyStopper&) = default;
    ThrowingCopyStopper& operator=(ThrowingCopyStopper&&) noexcept = default;

    void operator()(std::uint32_t, std::uint64_t,
                    const std::array<std::uint8_t, 16>&,
                    std::uint32_t) const {
        ++state->calls;
    }
};

template <typename Writer>
toolbusd::RuntimeControlGate::GpioIo test_io(
    Writer writer,
    toolbusd::RuntimeControlGate::GpioSafeStopper stopper = noop_stop,
    toolbusd::RuntimeControlGate::GpioCloser closer = noop_close) {
    auto created_this_call = std::make_shared<bool>(false);
    return {
        [writer, created_this_call] {
            *created_this_call = true;
            return writer(std::optional<std::uint32_t>{});
        },
        [writer, created_this_call](std::uint32_t object_id, bool) {
            if (!*created_this_call) {
                const auto returned = writer(
                    std::optional<std::uint32_t>{object_id});
                assert(returned == object_id);
            }
        },
        std::move(stopper),
        std::move(closer),
    };
}

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
    value.expected_node_uuid[0] = 0x31U;
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
    value.expected_node_uuid = command.expected_node_uuid;
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
    toolbusd::RuntimeControlGate gate(
        4U, [] { return 1000000000ULL; }, false);
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
                               test_io([&](auto) {
                                   first_started.set_value();
                                   finish.wait();
                                   return 91U;
                               }));
    });
    first_started.get_future().wait();

    auto same_scope = first;
    same_scope.idempotency_key = "same-scope-next";
    same_scope.value = false;
    auto same_future = std::async(std::launch::async, [&] {
        return gate.gpio_write(same_scope, daemon, 1U, descriptor(),
                               contract(), test_io([](auto object) {
                                   assert(object == 91U);
                                   return 91U;
                               }));
    });
    assert(same_future.wait_for(std::chrono::milliseconds(30)) ==
           std::future_status::timeout);

    const auto unrelated = gate.gpio_write(
        other, daemon, 1U, descriptor(other.resource_id),
        contract(other.resource_id), test_io([](auto object) {
            assert(!object.has_value());
            return 92U;
        }));
    assert(unrelated.object_id == 92U);
    finish_first.set_value();
    assert(first_future.get().object_id == 91U);
    assert(same_future.get().object_id == 91U);
}

void check_identity_permission_contract_and_idempotency() {
    std::uint64_t now = 1000000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 2U;
    auto command = request(daemon, lease);
    expect_error(toolbusd::RuntimeControlError::LeaseNotFound, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 7U, descriptor(), contract(),
            test_io([](auto) { return 40U; })));
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
        command, daemon, 7U, descriptor(), contract(), test_io(writer));
    assert(first.object_id == 41U && first.value && !first.replayed);
    assert(writes == 1U && gate.active_lease_count() == 1U);
    const auto replay = gate.gpio_write(
        command, daemon, 7U, descriptor(), contract(), test_io(writer));
    assert(replay.object_id == 41U && replay.replayed && writes == 1U);

    auto conflict = command;
    conflict.value = false;
    expect_error(toolbusd::RuntimeControlError::IdempotencyConflict, [&] {
        static_cast<void>(gate.gpio_write(
            conflict, daemon, 7U, descriptor(), contract(), test_io(writer)));
    });
    auto wrong_daemon = daemon;
    wrong_daemon[1] = 9U;
    expect_error(toolbusd::RuntimeControlError::DaemonIdentityMismatch, [&] {
        static_cast<void>(gate.gpio_write(
            command, wrong_daemon, 7U, descriptor(), contract(),
            test_io(writer)));
    });
    auto denied = command;
    denied.idempotency_key = "gpio-command-2";
    denied.permissions = 0U;
    expect_error(toolbusd::RuntimeControlError::PermissionDenied, [&] {
        static_cast<void>(gate.gpio_write(
            denied, daemon, 7U, descriptor(), contract(), test_io(writer)));
    });
    auto wrong_node_identity = command;
    wrong_node_identity.idempotency_key = "gpio-command-wrong-node";
    wrong_node_identity.expected_node_uuid[1] = 1U;
    expect_error(toolbusd::RuntimeControlError::LeaseConflict, [&] {
        static_cast<void>(gate.gpio_write(
            wrong_node_identity, daemon, 7U, descriptor(), contract(),
            test_io(writer)));
    });
    auto readonly = contract();
    readonly.access_flags = protocol::kResourceAccessReadable;
    expect_error(toolbusd::RuntimeControlError::ContractRejected, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 7U, descriptor(), readonly, test_io(writer)));
    });
}

void check_scope_expiry_generation_and_reuse() {
    std::uint64_t now = 5000000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
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
        test_io([](auto) { return 51U; })));

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
            test_io([](auto) { return 53U; })));
    });

    now += 1000000001ULL;
    assert(gate.reap_expired() == 0U);
    gate.acquire(acquire_request(second), daemon, 12U, descriptor(),
                 contract());
    bool reused_old_object = false;
    const auto result = gate.gpio_write(
        second, daemon, 12U, descriptor(), contract(),
        test_io([&](std::optional<std::uint32_t> object) {
            reused_old_object = object.has_value();
            return 61U;
        }));
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
            test_io([](auto) { return 62U; })));
    });
}

void check_release_stops_low_closes_and_reuses_scope() {
    std::uint64_t now = 7000000000ULL;
    std::uint32_t stop_count = 0U;
    std::uint32_t stopped_object = 0U;
    std::uint32_t close_count = 0U;
    std::uint32_t closed_object = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 4U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 21U, descriptor(),
                 contract());
    const auto stopper = [&](std::uint32_t node_id,
                             std::uint64_t generation,
                             const std::array<std::uint8_t, 16>& uuid,
                             std::uint32_t object_id) {
        ++stop_count;
        stopped_object = object_id;
        assert(node_id == command.node_id);
        assert(generation == 21U);
        assert(uuid == command.expected_node_uuid);
    };
    static_cast<void>(gate.gpio_write(
        command, daemon, 21U, descriptor(), contract(),
        test_io([](auto object) {
            assert(!object.has_value());
            return 71U;
        }, stopper,
        [&](std::uint32_t node_id, std::uint64_t generation,
            const std::array<std::uint8_t, 16>& uuid,
            std::uint32_t object_id) {
            ++close_count;
            closed_object = object_id;
            assert(node_id == command.node_id && generation == 21U);
            assert(uuid == command.expected_node_uuid);
        })));

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    gate.release(release, daemon);
    assert(stop_count == 1U && stopped_object == 71U);
    assert(close_count == 1U && closed_object == 71U);
    assert(gate.active_lease_count() == 0U);

    std::array<std::uint8_t, 16> next_lease{};
    next_lease[0] = 5U;
    auto next = request(daemon, next_lease);
    next.idempotency_key = "release-next-owner";
    // Close 已获得确定成功，同一节点代次即可重新登记并创建新对象。
    gate.acquire(acquire_request(next), daemon, 21U, descriptor(),
                 contract());
    bool reused = false;
    static_cast<void>(gate.gpio_write(
        next, daemon, 21U, descriptor(), contract(),
        test_io([&](auto object) {
            reused = object.has_value();
            return 72U;
        })));
    assert(!reused);
    assert(gate.shutdown() == 0U);
}

void check_close_uncertain_retries_close_only() {
    std::uint64_t now = 8000000000ULL;
    bool close_must_fail = true;
    std::uint32_t stop_count = 0U;
    std::uint32_t close_count = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 40U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 23U, descriptor(),
                 contract());
    static_cast<void>(gate.gpio_write(
        command, daemon, 23U, descriptor(), contract(),
        test_io(
            [](auto object) {
                assert(!object.has_value());
                return 73U;
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&,
                std::uint32_t object_id) {
                assert(object_id == 73U);
                ++stop_count;
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&,
                std::uint32_t object_id) {
                assert(object_id == 73U);
                ++close_count;
                if (close_must_fail) {
                    throw std::runtime_error("模拟 GPIO_CLOSE 响应丢失");
                }
            })));

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        gate.release(release, daemon);
    });
    assert(stop_count == 1U && close_count == 1U);

    std::array<std::uint8_t, 16> blocked_lease{};
    blocked_lease[0] = 41U;
    auto blocked = request(daemon, blocked_lease);
    blocked.idempotency_key = "close-uncertain-blocked";
    expect_error(toolbusd::RuntimeControlError::LeaseConflict, [&] {
        gate.acquire(acquire_request(blocked), daemon, 23U,
                     descriptor(), contract());
    });

    close_must_fail = false;
    gate.release(release, daemon);
    // 第二次只重试幂等 Close，不再向可能已经不存在的对象写低。
    assert(stop_count == 1U && close_count == 2U);
    gate.acquire(acquire_request(blocked), daemon, 23U,
                 descriptor(), contract());
    assert(gate.shutdown() == 0U);
}

void check_stop_failure_poison_and_other_scope_isolation() {
    std::uint64_t now = 9000000000ULL;
    bool first_stop_must_fail = true;
    std::uint32_t first_stop_count = 0U;
    std::uint32_t second_stop_count = 0U;
    toolbusd::RuntimeControlGate gate(6U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> first_lease{};
    std::array<std::uint8_t, 16> second_lease{};
    daemon[0] = 1U;
    first_lease[0] = 6U;
    second_lease[0] = 7U;
    auto first = request(daemon, first_lease);
    auto second = request(daemon, second_lease);
    second.owner_key_id = "operator-b";
    second.resource_id = 0x01000006U;
    second.idempotency_key = "isolation-second";
    gate.acquire(acquire_request(first, 100U), daemon, 31U,
                 descriptor(), contract());
    gate.acquire(acquire_request(second, 100U), daemon, 31U,
                 descriptor(second.resource_id), contract(second.resource_id));
    static_cast<void>(gate.gpio_write(
        first, daemon, 31U, descriptor(), contract(),
        test_io([](auto) { return 81U; },
        [&](std::uint32_t, std::uint64_t,
            const std::array<std::uint8_t, 16>&, std::uint32_t) {
            ++first_stop_count;
            if (first_stop_must_fail) {
                throw std::runtime_error("模拟单资源安全写低失败");
            }
        })));
    static_cast<void>(gate.gpio_write(
        second, daemon, 31U, descriptor(second.resource_id),
        contract(second.resource_id), test_io([](auto) { return 82U; },
        [&](std::uint32_t, std::uint64_t,
            const std::array<std::uint8_t, 16>&, std::uint32_t object_id) {
            assert(object_id == 82U);
            ++second_stop_count;
        })));

    now += 100000001ULL;
    assert(gate.reap_expired() == 1U);
    assert(first_stop_count == 1U);
    assert(second_stop_count == 1U);
    assert(gate.active_lease_count() == 0U);

    std::array<std::uint8_t, 16> replacement_lease{};
    replacement_lease[0] = 8U;
    auto replacement = request(daemon, replacement_lease);
    replacement.owner_key_id = "operator-c";
    replacement.idempotency_key = "failed-scope-replacement";
    expect_error(toolbusd::RuntimeControlError::LeaseConflict, [&] {
        gate.acquire(acquire_request(replacement), daemon, 31U,
                     descriptor(), contract());
    });

    // 失败 scope 可由原所有者显式重试；成功前绝不交给新所有者。
    first_stop_must_fail = false;
    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = first_lease;
    release.owner_key_id = first.owner_key_id;
    gate.release(release, daemon);
    assert(first_stop_count == 2U);
    gate.acquire(acquire_request(replacement), daemon, 31U,
                 descriptor(), contract());

    // 第二个资源已经独立完成安全停机；换代后可重新使用，不受首资源故障影响。
    std::array<std::uint8_t, 16> other_lease{};
    other_lease[0] = 9U;
    auto other = request(daemon, other_lease);
    other.owner_key_id = "operator-d";
    other.resource_id = second.resource_id;
    other.idempotency_key = "isolated-new-generation";
    gate.acquire(acquire_request(other), daemon, 32U,
                 descriptor(other.resource_id), contract(other.resource_id));
    assert(gate.shutdown() == 0U);
}

void check_shutdown_attempts_every_scope() {
    std::uint64_t now = 11000000000ULL;
    std::uint32_t successful_stops = 0U;
    std::uint32_t failed_stops = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    daemon[0] = 1U;
    for (std::uint8_t index = 1U; index <= 2U; ++index) {
        std::array<std::uint8_t, 16> lease{};
        lease[0] = static_cast<std::uint8_t>(10U + index);
        auto command = request(daemon, lease);
        command.resource_id += index;
        command.idempotency_key = "shutdown-" + std::to_string(index);
        gate.acquire(acquire_request(command), daemon, 41U,
                     descriptor(command.resource_id),
                     contract(command.resource_id));
        static_cast<void>(gate.gpio_write(
            command, daemon, 41U, descriptor(command.resource_id),
            contract(command.resource_id),
            test_io(
                [index](auto) {
                    return static_cast<std::uint32_t>(90U + index);
                },
            [&, index](std::uint32_t, std::uint64_t,
                       const std::array<std::uint8_t, 16>&,
                       std::uint32_t) {
                if (index == 1U) {
                    ++failed_stops;
                    throw std::runtime_error("模拟会话结束停机失败");
                }
                ++successful_stops;
            })));
    }
    assert(gate.shutdown() == 1U);
    assert(failed_stops == 1U && successful_stops == 1U);
    // shutdown 幂等且记忆失败，不会把尚未清理的资源误报为成功。
    assert(gate.shutdown() == 1U);
    assert(failed_stops == 1U && successful_stops == 1U);

    std::array<std::uint8_t, 16> after_shutdown_lease{};
    after_shutdown_lease[0] = 20U;
    auto after_shutdown = request(daemon, after_shutdown_lease);
    after_shutdown.idempotency_key = "after-shutdown";
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        gate.acquire(acquire_request(after_shutdown), daemon, 41U,
                     descriptor(), contract());
    });
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        static_cast<void>(gate.gpio_write(
            after_shutdown, daemon, 41U, descriptor(), contract(),
            test_io([](auto) { return 99U; })));
    });
}

void check_shutdown_closes_concurrent_admission_window() {
    std::uint64_t now = 13000000000ULL;
    std::uint32_t stop_count = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 21U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 51U, descriptor(),
                 contract());

    std::promise<void> writer_started;
    std::promise<void> finish_writer;
    auto finish = finish_writer.get_future().share();
    auto write = std::async(std::launch::async, [&] {
        return gate.gpio_write(
            command, daemon, 51U, descriptor(), contract(),
            test_io([&](auto) {
                writer_started.set_value();
                finish.wait();
                return 101U;
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&,
                std::uint32_t object_id) {
                assert(object_id == 101U);
                ++stop_count;
            }));
    });
    writer_started.get_future().wait();
    auto shutdown = std::async(std::launch::async, [&] {
        return gate.shutdown();
    });

    std::array<std::uint8_t, 16> late_lease{};
    late_lease[0] = 22U;
    auto late = request(daemon, late_lease);
    late.resource_id = 0x01000006U;
    late.idempotency_key = "shutdown-race";
    const auto admission_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(1);
    bool rejected_after_stop = false;
    while (std::chrono::steady_clock::now() < admission_deadline) {
        try {
            gate.acquire(acquire_request(late), daemon, 51U,
                         descriptor(late.resource_id),
                         contract(late.resource_id));
        } catch (const toolbusd::RuntimeControlException& error) {
            if (error.code() == toolbusd::RuntimeControlError::SafeStopFailed) {
                rejected_after_stop = true;
                break;
            }
            throw;
        }
        std::this_thread::yield();
    }
    assert(rejected_after_stop);
    finish_writer.set_value();
    try {
        static_cast<void>(write.get());
        assert(false);
    } catch (const toolbusd::RuntimeControlException& error) {
        assert(error.code() == toolbusd::RuntimeControlError::LeaseExpired);
    }
    assert(shutdown.get() == 0U);
    assert(stop_count == 1U);
}

void check_background_expiry_stops_without_new_request() {
    std::promise<std::uint32_t> stopped_object;
    toolbusd::RuntimeControlGate gate;
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 23U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command, 500U), daemon, 61U,
                 descriptor(), contract());
    static_cast<void>(gate.gpio_write(
        command, daemon, 61U, descriptor(), contract(),
        test_io([](auto) { return 111U; },
        [&](std::uint32_t, std::uint64_t,
            const std::array<std::uint8_t, 16>&, std::uint32_t object_id) {
            stopped_object.set_value(object_id);
        })));
    auto stopped = stopped_object.get_future();
    assert(stopped.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready);
    assert(stopped.get() == 111U);
    assert(gate.active_lease_count() == 0U);
    assert(gate.shutdown() == 0U);
}

void check_first_create_crosses_ttl_without_orphan() {
    std::atomic<std::uint64_t> now{15000000000ULL};
    std::uint32_t stop_count = 0U;
    toolbusd::RuntimeControlGate gate(
        4U, [&] { return now.load(); }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 24U;
    auto command = request(daemon, lease);
    command.value = true;
    gate.acquire(acquire_request(command, 100U), daemon, 71U,
                 descriptor(), contract());

    std::promise<void> create_started;
    std::promise<void> finish_create;
    auto finish = finish_create.get_future().share();
    auto write = std::async(std::launch::async, [&] {
        return gate.gpio_write(
            command, daemon, 71U, descriptor(), contract(),
            toolbusd::RuntimeControlGate::GpioIo{
                [&] {
                    create_started.set_value();
                    finish.wait();
                    return 121U;
                },
                // 租约在 CREATE 阻塞期间到期后，绝不能再进入第二阶段高
                // 电平写入；只能对已经登记的低电平对象执行安全停机。
                [](std::uint32_t, bool) { assert(false); },
                [&](std::uint32_t, std::uint64_t,
                    const std::array<std::uint8_t, 16>&,
                    std::uint32_t object_id) {
                    assert(object_id == 121U);
                    ++stop_count;
                }, noop_close});
    });
    create_started.get_future().wait();
    now.fetch_add(100000001ULL);
    // CREATE in-flight 且对象尚未登记时，reaper 必须保留租约。
    assert(gate.reap_expired() == 0U);
    finish_create.set_value();
    try {
        static_cast<void>(write.get());
        assert(false);
    } catch (const toolbusd::RuntimeControlException& error) {
        assert(error.code() == toolbusd::RuntimeControlError::LeaseExpired);
    }
    assert(stop_count == 1U);
    assert(gate.active_lease_count() == 0U);

    std::array<std::uint8_t, 16> replacement_lease{};
    replacement_lease[0] = 25U;
    auto replacement = request(daemon, replacement_lease);
    replacement.idempotency_key = "cross-ttl-replacement";
    gate.acquire(acquire_request(replacement), daemon, 71U,
                 descriptor(), contract());
    toolbusd::RuntimeControlReleaseRequest replacement_release;
    replacement_release.daemon_instance_id = daemon;
    replacement_release.lease_id = replacement_lease;
    replacement_release.owner_key_id = replacement.owner_key_id;
    gate.release(replacement_release, daemon);

    // 节点换代后复用同一租约/幂等键，必须重新执行而不能读到失败路径
    // 遗留的 pending 结果。
    std::uint32_t retry_creates = 0U;
    std::uint32_t retry_writes = 0U;
    gate.acquire(acquire_request(command, 100U), daemon, 72U,
                 descriptor(), contract());
    const auto retried = gate.gpio_write(
        command, daemon, 72U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [&] {
                ++retry_creates;
                return 122U;
            },
            [&](std::uint32_t object_id, bool value) {
                assert(object_id == 122U && value);
                ++retry_writes;
            },
            noop_stop, noop_close});
    assert(retried.object_id == 122U && !retried.replayed);
    assert(retry_creates == 1U && retry_writes == 1U);
    assert(gate.shutdown() == 0U);
}

void check_create_response_loss_poison_until_generation_change() {
    std::uint64_t now = 17000000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 26U;
    auto command = request(daemon, lease);
    std::uint32_t create_attempts = 0U;
    std::uint32_t unsafe_writes = 0U;
    std::uint32_t stop_attempts = 0U;
    gate.acquire(acquire_request(command), daemon, 81U, descriptor(),
                 contract());
    try {
        static_cast<void>(gate.gpio_write(
            command, daemon, 81U, descriptor(), contract(),
            toolbusd::RuntimeControlGate::GpioIo{
                [&]() -> std::uint32_t {
                    ++create_attempts;
                    throw std::runtime_error("模拟 CREATE 低电平响应丢失");
                },
                [&](std::uint32_t, bool) { ++unsafe_writes; },
                [&](std::uint32_t, std::uint64_t,
                    const std::array<std::uint8_t, 16>&, std::uint32_t) {
                    ++stop_attempts;
                }, noop_close}));
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(create_attempts == 1U && unsafe_writes == 0U &&
           stop_attempts == 0U);
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 81U, descriptor(), contract(),
            test_io([](auto) { return 131U; })));
    });
    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        gate.release(release, daemon);
    });
    assert(gate.shutdown() == 1U);

    // 独立门模拟节点换代：新 generation 才能解除 unknown-object poison。
    toolbusd::RuntimeControlGate next_gate(4U, [&] { return now; }, false);
    next_gate.acquire(acquire_request(command), daemon, 81U, descriptor(),
                      contract());
    try {
        static_cast<void>(next_gate.gpio_write(
            command, daemon, 81U, descriptor(), contract(),
            toolbusd::RuntimeControlGate::GpioIo{
                []() -> std::uint32_t {
                    throw std::runtime_error("模拟 CREATE 响应丢失");
                },
                [](std::uint32_t, bool) {}, noop_stop, noop_close}));
    } catch (const std::runtime_error&) {
    }
    std::array<std::uint8_t, 16> new_lease{};
    new_lease[0] = 27U;
    auto replacement = request(daemon, new_lease);
    replacement.idempotency_key = "unknown-object-new-generation";
    next_gate.acquire(acquire_request(replacement), daemon, 82U,
                      descriptor(), contract());
    assert(next_gate.shutdown() == 0U);
}

void check_second_stage_uncertain_write_is_stopped_or_poisoned() {
    std::uint64_t now = 19000000000ULL;
    bool stop_must_fail = false;
    std::uint32_t high_attempts = 0U;
    std::uint32_t stop_attempts = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 28U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 91U, descriptor(),
                 contract());
    const auto io = toolbusd::RuntimeControlGate::GpioIo{
        [] { return 141U; },
        [&](std::uint32_t object_id, bool value) {
            assert(object_id == 141U && value);
            ++high_attempts;
            throw std::runtime_error("模拟高电平写响应丢失");
        },
        [&](std::uint32_t, std::uint64_t,
            const std::array<std::uint8_t, 16>&, std::uint32_t object_id) {
            assert(object_id == 141U);
            ++stop_attempts;
            if (stop_must_fail) {
                throw std::runtime_error("模拟安全写低失败");
            }
        }, noop_close};
    try {
        static_cast<void>(gate.gpio_write(
            command, daemon, 91U, descriptor(), contract(), io));
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(high_attempts == 1U && stop_attempts == 1U);
    assert(gate.active_lease_count() == 0U);
    std::array<std::uint8_t, 16> reusable_lease{};
    reusable_lease[0] = 29U;
    auto reusable = request(daemon, reusable_lease);
    reusable.idempotency_key = "uncertain-close-reusable";
    gate.acquire(acquire_request(reusable), daemon, 91U,
                 descriptor(), contract());
    assert(gate.shutdown() == 0U);

    // 安全写低也失败时，已登记的 object_id 必须留下 poison，供 release 重试。
    toolbusd::RuntimeControlGate poison_gate(
        4U, [&] { return now; }, false);
    stop_must_fail = true;
    poison_gate.acquire(acquire_request(command), daemon, 92U,
                        descriptor(), contract());
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        static_cast<void>(poison_gate.gpio_write(
            command, daemon, 92U, descriptor(), contract(), io));
    });
    expect_error(toolbusd::RuntimeControlError::SafeStopFailed, [&] {
        static_cast<void>(poison_gate.gpio_write(
            command, daemon, 92U, descriptor(), contract(), io));
    });
    stop_must_fail = false;
    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    poison_gate.release(release, daemon);
    assert(poison_gate.shutdown() == 0U);
}

void check_expiry_clock_failure_is_reported_without_terminate() {
    std::promise<void> clock_called;
    bool first = true;
    toolbusd::RuntimeControlGate gate(
        4U,
        [&]() -> std::uint64_t {
            if (first) {
                first = false;
                clock_called.set_value();
            }
            throw std::runtime_error("模拟后台单调时钟失败");
        });
    clock_called.get_future().wait();
    assert(gate.shutdown() == 1U);
}

void check_concurrent_idempotency_publishes_once() {
    std::uint64_t now = 20500000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 30U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 100U, descriptor(),
                 contract());

    std::promise<void> write_started;
    std::promise<void> finish_write;
    auto finish = finish_write.get_future().share();
    std::atomic<std::uint32_t> creates{0U};
    std::atomic<std::uint32_t> writes{0U};
    const auto io = toolbusd::RuntimeControlGate::GpioIo{
        [&] {
            ++creates;
            return 145U;
        },
        [&](std::uint32_t object_id, bool value) {
            assert(object_id == 145U && value);
            if (++writes == 1U) {
                write_started.set_value();
                finish.wait();
            }
        },
        noop_stop, noop_close};
    auto first = std::async(std::launch::async, [&] {
        return gate.gpio_write(command, daemon, 100U, descriptor(),
                               contract(), io);
    });
    write_started.get_future().wait();
    auto replay = std::async(std::launch::async, [&] {
        return gate.gpio_write(command, daemon, 100U, descriptor(),
                               contract(), io);
    });
    assert(replay.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
    finish_write.set_value();
    const auto first_result = first.get();
    const auto replay_result = replay.get();
    assert(!first_result.replayed && replay_result.replayed);
    assert(first_result.object_id == 145U && replay_result.object_id == 145U);
    assert(creates == 1U && writes == 1U);
    assert(gate.shutdown() == 0U);
}

void check_concurrent_shutdown_has_single_owner() {
    std::uint64_t now = 20700000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 34U;
    auto command = request(daemon, lease);
    command.value = false;
    gate.acquire(acquire_request(command), daemon, 104U, descriptor(),
                 contract());
    std::promise<void> stop_started;
    std::promise<void> finish_stop;
    auto finish = finish_stop.get_future().share();
    std::atomic<std::uint32_t> stops{0U};
    static_cast<void>(gate.gpio_write(
        command, daemon, 104U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [] { return 181U; },
            [](std::uint32_t, bool) { assert(false); },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&, std::uint32_t) {
                assert(++stops == 1U);
                stop_started.set_value();
                finish.wait();
            }, noop_close}));
    auto first = std::async(std::launch::async,
                            [&] { return gate.shutdown(); });
    stop_started.get_future().wait();
    auto second = std::async(std::launch::async,
                             [&] { return gate.shutdown(); });
    assert(second.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
    finish_stop.set_value();
    assert(first.get() == 0U);
    assert(second.get() == 0U);
    assert(stops == 1U);
}

void check_concurrent_release_and_shutdown_close_once() {
    std::uint64_t now = 20800000000ULL;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 35U;
    auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 105U, descriptor(),
                 contract());

    std::promise<void> stop_started;
    std::promise<void> finish_stop;
    auto finish = finish_stop.get_future().share();
    std::atomic<std::uint32_t> stops{0U};
    std::atomic<std::uint32_t> closes{0U};
    static_cast<void>(gate.gpio_write(
        command, daemon, 105U, descriptor(), contract(),
        test_io(
            [](auto) { return 182U; },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&,
                std::uint32_t object_id) {
                assert(object_id == 182U);
                assert(++stops == 1U);
                stop_started.set_value();
                finish.wait();
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&,
                std::uint32_t object_id) {
                assert(object_id == 182U);
                assert(++closes == 1U);
            })));

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    auto releasing = std::async(std::launch::async,
                                [&] { gate.release(release, daemon); });
    stop_started.get_future().wait();
    auto shutting = std::async(std::launch::async,
                               [&] { return gate.shutdown(); });
    assert(shutting.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
    finish_stop.set_value();
    releasing.get();
    assert(shutting.get() == 0U);
    assert(stops == 1U && closes == 1U);
}

void check_create_registration_copy_failure_has_no_remote_side_effect() {
    std::uint64_t now = 21000000000ULL;
    auto copy_state = std::make_shared<ThrowingCopyState>();
    toolbusd::RuntimeControlGate::GpioSafeStopper stopper{
        ThrowingCopyStopper{copy_state}};
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 31U;
    auto command = request(daemon, lease);
    command.value = false;
    gate.acquire(acquire_request(command), daemon, 101U, descriptor(),
                 contract());
    std::uint32_t creates = 0U;
    toolbusd::RuntimeControlGate::GpioIo io{
        [&] {
            ++creates;
            return 151U;
        },
        [](std::uint32_t, bool) { assert(false); },
        stopper, noop_close};

    copy_state->throw_on_copy = true;
    try {
        static_cast<void>(gate.gpio_write(
            command, daemon, 101U, descriptor(), contract(), io));
        assert(false);
    } catch (const std::runtime_error&) {
    }
    // stopper 无法复制时必须发生在远端 CREATE 之前，也不能遗留 in-flight。
    assert(creates == 0U);
    copy_state->throw_on_copy = false;
    const auto result = gate.gpio_write(
        command, daemon, 101U, descriptor(), contract(), io);
    assert(result.object_id == 151U && creates == 1U);

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    copy_state->throw_on_copy = true;
    try {
        gate.release(release, daemon);
        assert(false);
    } catch (const std::runtime_error&) {
    }
    copy_state->throw_on_copy = false;
    // release 的任务复制失败也不能发布永久 in-flight，占位应可重试。
    gate.release(release, daemon);
    assert(copy_state->calls == 1U);
    assert(gate.shutdown() == 0U);
}

void check_cleanup_task_copy_failure_rolls_back_in_flight() {
    std::uint64_t now = 23000000000ULL;
    auto copy_state = std::make_shared<ThrowingCopyState>();
    toolbusd::RuntimeControlGate::GpioSafeStopper stopper{
        ThrowingCopyStopper{copy_state}};
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 32U;
    auto command = request(daemon, lease);
    command.value = false;
    gate.acquire(acquire_request(command, 100U), daemon, 102U,
                 descriptor(), contract());
    const auto result = gate.gpio_write(
        command, daemon, 102U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [] { return 161U; },
            [](std::uint32_t, bool) { assert(false); }, stopper,
            noop_close});
    assert(result.object_id == 161U);

    now += 100000001ULL;
    copy_state->throw_on_copy = true;
    try {
        static_cast<void>(gate.reap_expired());
        assert(false);
    } catch (const std::runtime_error&) {
    }
    copy_state->throw_on_copy = false;
    // 第二次 reaper 能正常取得作用域，证明第一次异常没有遗留占位。
    assert(gate.reap_expired() == 0U);
    assert(copy_state->calls == 1U);
    assert(gate.shutdown() == 0U);
}

void check_shutdown_task_copy_failure_is_memoized() {
    std::uint64_t now = 25000000000ULL;
    auto copy_state = std::make_shared<ThrowingCopyState>();
    toolbusd::RuntimeControlGate::GpioSafeStopper stopper{
        ThrowingCopyStopper{copy_state}};
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 33U;
    auto command = request(daemon, lease);
    command.value = false;
    gate.acquire(acquire_request(command), daemon, 103U, descriptor(),
                 contract());
    static_cast<void>(gate.gpio_write(
        command, daemon, 103U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [] { return 171U; },
            [](std::uint32_t, bool) { assert(false); }, stopper,
            noop_close}));

    copy_state->throw_on_copy = true;
    const auto first = gate.shutdown();
    const auto second = gate.shutdown();
    assert(first == 1U);
    assert(second == first);
    assert(copy_state->calls == 0U);
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
    assert(decoded.expected_node_uuid == command.expected_node_uuid);
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
    malformed[60U] = 2U;
    try {
        static_cast<void>(
            toolbusd::decode_ipc_runtime_gpio_write(malformed));
        assert(false);
    } catch (const toolbusd::IpcException&) {
    }
}

void check_ipc_error_envelope_is_strict_and_bounded() {
    using Category = toolbusd::IpcErrorCategory;
    using Code = toolbusd::IpcErrorCode;
    const toolbusd::IpcErrorEnvelope uncertain{
        toolbusd::kIpcErrorEnvelopeVersion, Code::SafeStopFailed,
        Category::Internal, false, true, "安全停机响应状态未知"};
    const auto body = toolbusd::encode_ipc_error_envelope(uncertain);
    const auto decoded = toolbusd::decode_ipc_error_envelope(body);
    check_always(decoded.version == toolbusd::kIpcErrorEnvelopeVersion,
                 "错误信封版本应保持一致");
    check_always(decoded.code == Code::SafeStopFailed,
                 "错误信封代码应保持一致");
    check_always(decoded.category == Category::Internal,
                 "错误信封类别应保持一致");
    check_always(!decoded.retryable, "不确定提交不得建议直接重试");
    check_always(decoded.possibly_committed, "不确定提交标志不得丢失");
    check_always(decoded.message == "安全停机响应状态未知",
                 "错误信封消息应保持一致");

    int sockets[2]{};
    const auto socket_result = ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    check_always(socket_result == 0, "socketpair必须成功执行");
    toolbusd::write_ipc_response(
        sockets[0], toolbusd::IpcStatus::Error, body);
    const auto framed = toolbusd::read_ipc_response(sockets[1]);
    check_always(framed.status == toolbusd::IpcStatus::Error,
                 "IPC响应状态必须为Error");
    check_always(
        toolbusd::decode_ipc_error_envelope(framed.body).code ==
            Code::SafeStopFailed,
        "IPC帧内错误信封必须可严格解码");
    ::close(sockets[0]);
    ::close(sockets[1]);

    // Release 的确定拒绝不允许声明“可能已提交”，也不建议盲重放。
    const toolbusd::IpcErrorEnvelope release_not_found{
        toolbusd::kIpcErrorEnvelopeVersion, Code::LeaseNotFound,
        Category::Conflict, false, false, "Runtime 控制租约不存在"};
    const auto release_decoded = toolbusd::decode_ipc_error_envelope(
        toolbusd::encode_ipc_error_envelope(release_not_found));
    check_always(release_decoded.code == Code::LeaseNotFound,
                 "确定拒绝代码应保持一致");
    check_always(!release_decoded.retryable,
                 "LeaseNotFound不得建议重试");
    check_always(!release_decoded.possibly_committed,
                 "LeaseNotFound不得声明可能提交");

    const auto must_reject_body = [](std::vector<std::uint8_t> malformed) {
        try {
            static_cast<void>(
                toolbusd::decode_ipc_error_envelope(malformed));
            check_always(false, "畸形错误信封必须被解码器拒绝");
        } catch (const toolbusd::IpcException&) {
        }
    };
    auto malformed = body;
    malformed[0U] = 2U;
    must_reject_body(malformed);
    malformed = body;
    malformed[2U] = 11U;
    must_reject_body(malformed);
    malformed = body;
    malformed[4U] = 0xfeU;
    malformed[5U] = 0xffU;
    must_reject_body(malformed);
    malformed = body;
    malformed[6U] = static_cast<std::uint8_t>(Category::Conflict);
    must_reject_body(malformed);
    malformed = body;
    malformed[7U] = 0x80U;
    must_reject_body(malformed);
    malformed = body;
    malformed[7U] = 0x03U;
    must_reject_body(malformed);
    malformed = toolbusd::encode_ipc_error_envelope(
        {toolbusd::kIpcErrorEnvelopeVersion, Code::InvalidRequest,
         Category::Request, false, false, "x"});
    malformed[7U] = 0x01U;
    must_reject_body(malformed);
    malformed = toolbusd::encode_ipc_error_envelope(
        {toolbusd::kIpcErrorEnvelopeVersion, Code::DeadlineExceeded,
         Category::Timeout, true, false, "超时"});
    malformed[7U] = 0x00U;
    must_reject_body(malformed);
    malformed = body;
    malformed[10U] = 1U;
    must_reject_body(malformed);
    malformed = body;
    malformed.push_back(0U);
    must_reject_body(malformed);
    malformed = toolbusd::encode_ipc_error_envelope(
        {toolbusd::kIpcErrorEnvelopeVersion, Code::InvalidRequest,
         Category::Request, false, false, "x"});
    malformed[12U] = 0xc0U;
    must_reject_body(malformed);
    malformed[12U] = 0x7fU;
    must_reject_body(malformed);

    const auto must_reject_value = [](toolbusd::IpcErrorEnvelope invalid) {
        try {
            static_cast<void>(toolbusd::encode_ipc_error_envelope(invalid));
            check_always(false, "非法错误字段必须被编码器拒绝");
        } catch (const toolbusd::IpcException&) {
        }
    };
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::LeaseConflict, Category::Conflict,
                       true, true, "冲突"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::LeaseNotFound, Category::Conflict,
                       false, true, "不存在"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::LeaseNotFound, Category::Conflict,
                       true, false, "不得重试"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::InvalidRequest, Category::Request,
                       true, false, "不得重试"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::DeadlineExceeded, Category::Timeout,
                       false, false, "标志不完整"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::LeaseNotFound, Category::Unavailable,
                       false, false, "类别不匹配"});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::InvalidRequest, Category::Request,
                       false, false, std::string(257U, 'x')});
    must_reject_value({toolbusd::kIpcErrorEnvelopeVersion,
                       Code::InvalidRequest, Category::Request,
                       false, false, std::string("x\n")});
}

void check_gpio_durability_orders_disk_boundaries_around_io() {
    std::uint64_t now = 30000000000ULL;
    std::vector<std::string> events;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 41U;
    const auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 201U, descriptor(),
                 contract());

    const auto result = gate.gpio_write(
        command, daemon, 201U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [&] {
                events.emplace_back("create");
                return 301U;
            },
            [&](std::uint32_t object_id, bool value) {
                assert(object_id == 301U && value);
                events.emplace_back("write");
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&, std::uint32_t) {
                events.emplace_back("safe");
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&, std::uint32_t) {
                events.emplace_back("close");
            }},
        toolbusd::RuntimeControlGate::GpioDurability{
            [&] { events.emplace_back("pending"); },
            [&](const toolbusd::RuntimeGpioWriteResult& durable) {
                assert(durable.object_id == 301U && durable.value);
                events.emplace_back("committed");
            },
            [&](auto, auto) { events.emplace_back("failed"); }});
    assert(result.object_id == 301U);
    assert((events == std::vector<std::string>{
                          "pending", "create", "write", "committed"}));
}

void check_gpio_pending_failure_rolls_back_without_remote_io() {
    std::uint64_t now = 31000000000ULL;
    std::uint32_t io_calls = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 42U;
    const auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 202U, descriptor(),
                 contract());
    try {
        static_cast<void>(gate.gpio_write(
            command, daemon, 202U, descriptor(), contract(),
            test_io([&](std::optional<std::uint32_t>) {
                ++io_calls;
                return 302U;
            }),
            toolbusd::RuntimeControlGate::GpioDurability{
                [] { throw std::runtime_error("pending sync failure"); },
                [](const auto&) {}, [](auto, auto) { assert(false); }}));
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(io_calls == 0U);
    const auto retried = gate.gpio_write(
        command, daemon, 202U, descriptor(), contract(),
        test_io([&](std::optional<std::uint32_t>) {
            ++io_calls;
            return 302U;
        }));
    assert(retried.object_id == 302U);
    assert(io_calls == 1U);
}

void check_gpio_terminal_sync_failure_forces_safe_cleanup() {
    std::uint64_t now = 32000000000ULL;
    std::uint32_t safe_calls = 0U;
    std::uint32_t close_calls = 0U;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 43U;
    const auto command = request(daemon, lease);
    gate.acquire(acquire_request(command), daemon, 203U, descriptor(),
                 contract());
    try {
        static_cast<void>(gate.gpio_write(
            command, daemon, 203U, descriptor(), contract(),
            toolbusd::RuntimeControlGate::GpioIo{
                [] { return 303U; }, [](std::uint32_t, bool) {},
                [&](std::uint32_t, std::uint64_t,
                    const std::array<std::uint8_t, 16>&, std::uint32_t) {
                    ++safe_calls;
                },
                [&](std::uint32_t, std::uint64_t,
                    const std::array<std::uint8_t, 16>&, std::uint32_t) {
                    ++close_calls;
                }},
            toolbusd::RuntimeControlGate::GpioDurability{
                [] {},
                [](const auto&) {
                    throw std::runtime_error("terminal sync failure");
                },
                [](auto, auto) { assert(false); }}));
        assert(false);
    } catch (const std::runtime_error&) {
    }
    assert(safe_calls == 1U);
    assert(close_calls == 1U);
    expect_error(toolbusd::RuntimeControlError::LeaseNotFound, [&] {
        static_cast<void>(gate.gpio_write(
            command, daemon, 203U, descriptor(), contract(),
            test_io([](std::optional<std::uint32_t>) { return 304U; })));
    });
}

void check_release_durability_uses_server_scope_and_reserves_it() {
    std::uint64_t now = 33000000000ULL;
    std::vector<std::string> events;
    toolbusd::RuntimeControlGate gate(4U, [&] { return now; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 1U;
    lease[0] = 44U;
    auto command = request(daemon, lease);
    command.value = false;
    gate.acquire(acquire_request(command), daemon, 204U, descriptor(),
                 contract());
    static_cast<void>(gate.gpio_write(
        command, daemon, 204U, descriptor(), contract(),
        toolbusd::RuntimeControlGate::GpioIo{
            [] { return 305U; }, [](std::uint32_t, bool) {},
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&, std::uint32_t) {
                events.emplace_back("safe");
            },
            [&](std::uint32_t, std::uint64_t,
                const std::array<std::uint8_t, 16>&, std::uint32_t) {
                events.emplace_back("close");
            }}));
    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = command.owner_key_id;
    gate.release(
        release, daemon,
        toolbusd::RuntimeControlGate::ReleaseDurability{
            [&](const toolbusd::RuntimeControlGate::ResolvedReleaseLease&
                    resolved) {
                assert(resolved.expected_node_uuid ==
                       command.expected_node_uuid);
                assert(resolved.node_id == command.node_id);
                assert(resolved.resource_id == command.resource_id);
                assert(resolved.permissions == command.permissions);
                events.emplace_back("pending");
            },
            [&] { events.emplace_back("committed"); },
            [&](auto, auto) { events.emplace_back("failed"); }});
    assert((events == std::vector<std::string>{
                          "pending", "safe", "close", "committed"}));
}

void check_release_readonly_resolution_tracks_lease_lifecycle() {
    toolbusd::RuntimeControlGate gate(
        4U, [] { return 1000000000ULL; }, false);
    std::array<std::uint8_t, 16> daemon{};
    std::array<std::uint8_t, 16> lease{};
    daemon[0] = 0x71U;
    lease[0] = 0x72U;
    auto first = request(daemon, lease);
    gate.acquire(acquire_request(first), daemon, 1U,
                 descriptor(first.resource_id), contract(first.resource_id));

    toolbusd::RuntimeControlReleaseRequest release;
    release.daemon_instance_id = daemon;
    release.lease_id = lease;
    release.owner_key_id = first.owner_key_id;
    const auto first_resolved = gate.resolve_release_lease(release, daemon);
    assert(first_resolved.has_value());
    assert(first_resolved->resource_id == first.resource_id);
    assert(first_resolved->admission_id != 0U);
    gate.release(release, daemon);
    assert(!gate.resolve_release_lease(release, daemon).has_value());

    auto second = first;
    second.resource_id = 0x01000006U;
    gate.acquire(acquire_request(second), daemon, 1U,
                 descriptor(second.resource_id), contract(second.resource_id));
    const auto second_resolved = gate.resolve_release_lease(release, daemon);
    assert(second_resolved.has_value());
    assert(second_resolved->resource_id == second.resource_id);
    assert(second_resolved->admission_id != first_resolved->admission_id);

    auto wrong_owner = release;
    wrong_owner.owner_key_id = "operator-b";
    expect_error(toolbusd::RuntimeControlError::PermissionDenied, [&] {
        static_cast<void>(gate.resolve_release_lease(wrong_owner, daemon));
    });
}

}  // namespace

int main() {
    check_identity_permission_contract_and_idempotency();
    check_scope_expiry_generation_and_reuse();
    check_ipc_round_trip_and_strict_lengths();
    check_ipc_error_envelope_is_strict_and_bounded();
    check_per_scope_isolation_and_serialization();
    check_release_stops_low_closes_and_reuses_scope();
    check_close_uncertain_retries_close_only();
    check_stop_failure_poison_and_other_scope_isolation();
    check_shutdown_attempts_every_scope();
    check_shutdown_closes_concurrent_admission_window();
    check_background_expiry_stops_without_new_request();
    check_first_create_crosses_ttl_without_orphan();
    check_create_response_loss_poison_until_generation_change();
    check_second_stage_uncertain_write_is_stopped_or_poisoned();
    check_expiry_clock_failure_is_reported_without_terminate();
    check_concurrent_idempotency_publishes_once();
    check_concurrent_shutdown_has_single_owner();
    check_concurrent_release_and_shutdown_close_once();
    check_create_registration_copy_failure_has_no_remote_side_effect();
    check_cleanup_task_copy_failure_rolls_back_in_flight();
    check_shutdown_task_copy_failure_is_memoized();
    check_gpio_durability_orders_disk_boundaries_around_io();
    check_gpio_pending_failure_rolls_back_without_remote_io();
    check_gpio_terminal_sync_failure_forces_safe_cleanup();
    check_release_durability_uses_server_scope_and_reserves_it();
    check_release_readonly_resolution_tracks_lease_lifecycle();
}
