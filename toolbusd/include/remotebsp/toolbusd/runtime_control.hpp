#pragma once

#include "remotebsp/protocol/resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace remotebsp::toolbusd {

constexpr std::uint16_t kRuntimeControlIpcVersion = 1U;
constexpr std::uint16_t kRuntimePermissionGpioWrite = 0x0001U;
constexpr std::size_t kMaximumRuntimeControlIdentityBytes = 64U;
constexpr std::size_t kMaximumRuntimeControlIdempotencyBytes = 64U;
constexpr std::uint32_t kMaximumRuntimeControlTtlMs = 30000U;

struct RuntimeControlAcquireRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionGpioWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint32_t ttl_ms{};
};

struct RuntimeGpioWriteRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionGpioWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
    bool value{};
};

struct RuntimeControlReleaseRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::string owner_key_id;
};

struct RuntimeGpioWriteResult {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::uint32_t object_id{};
    bool value{};
    bool replayed{};
};

enum class RuntimeControlError {
    InvalidRequest,
    DaemonIdentityMismatch,
    PermissionDenied,
    LeaseConflict,
    LeaseNotFound,
    LeaseExpired,
    ContractRejected,
    CapacityExceeded,
    IdempotencyConflict,
};

class RuntimeControlException : public std::runtime_error {
public:
    RuntimeControlException(RuntimeControlError code,
                            const std::string& message);
    RuntimeControlError code() const noexcept;

private:
    RuntimeControlError code_;
};

// toolbusd 内部的最终授权门。所有字段校验、资源互斥、幂等判定和命令提交
// 都在同一把锁下完成；回调只允许提交已经验证的单个 GPIO 目标。
class RuntimeControlGate {
public:
    using Clock = std::function<std::uint64_t()>;
    using GpioWriter = std::function<std::uint32_t(
        std::optional<std::uint32_t> existing_object_id)>;

    explicit RuntimeControlGate(std::size_t capacity = 256U,
                                Clock monotonic_ns = {});

    void acquire(
        const RuntimeControlAcquireRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract);

    RuntimeGpioWriteResult gpio_write(
        const RuntimeGpioWriteRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract,
        const GpioWriter& writer);

    void release(
        const RuntimeControlReleaseRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id);

    std::size_t active_lease_count();

private:
    struct LeaseState {
        std::array<std::uint8_t, 16> lease_id{};
        std::string owner_key_id;
        std::uint16_t permissions{};
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        std::uint64_t node_generation{};
        std::uint64_t deadline_ns{};
    };

    struct CompletedCommand {
        std::array<std::uint8_t, 16> lease_id{};
        std::string owner_key_id;
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        bool value{};
        RuntimeGpioWriteResult result;
        std::uint64_t retain_until_ns{};
    };

    struct GpioObject {
        std::uint32_t object_id{};
        std::uint64_t node_generation{};
    };

    static std::string binary_id(
        const std::array<std::uint8_t, 16>& value);
    static std::uint64_t scope_key(std::uint32_t node_id,
                                   std::uint32_t resource_id) noexcept;
    void expire(std::uint64_t now_ns);

    std::size_t capacity_;
    std::size_t history_capacity_;
    Clock monotonic_ns_;
    std::mutex mutex_;
    std::condition_variable scope_available_;
    std::unordered_map<std::string, LeaseState> leases_;
    std::unordered_map<std::uint64_t, std::string> leases_by_scope_;
    std::unordered_map<std::string, CompletedCommand> completed_;
    std::unordered_map<std::uint64_t, GpioObject> gpio_objects_;
    std::unordered_set<std::uint64_t> in_flight_scopes_;
};

}  // namespace remotebsp::toolbusd
