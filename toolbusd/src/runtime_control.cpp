#include "remotebsp/toolbusd/runtime_control.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace remotebsp::toolbusd {
namespace {

constexpr std::uint64_t kIdempotencyRetentionNs = 30000000000ULL;
constexpr auto kSameScopeWait = std::chrono::milliseconds(2000);

bool nonzero(const std::array<std::uint8_t, 16>& value) noexcept {
    return std::any_of(value.begin(), value.end(),
                       [](std::uint8_t byte) { return byte != 0U; });
}

bool valid_identity(const std::string& value) noexcept {
    if (value.empty() || value.size() > kMaximumRuntimeControlIdentityBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte >= 'A' && byte <= 'Z') ||
               (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

bool valid_idempotency(const std::string& value) noexcept {
    if (value.empty() ||
        value.size() > kMaximumRuntimeControlIdempotencyBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte >= 'A' && byte <= 'Z') ||
               (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || character == '.' ||
               character == '_' || character == '-' || character == ':';
    });
}

std::uint64_t default_monotonic_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[noreturn]] void reject(RuntimeControlError code,
                         const char* message) {
    throw RuntimeControlException(code, message);
}

}  // namespace

RuntimeControlException::RuntimeControlException(
    RuntimeControlError code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

RuntimeControlError RuntimeControlException::code() const noexcept {
    return code_;
}

RuntimeControlGate::RuntimeControlGate(std::size_t capacity,
                                       Clock monotonic_ns)
    : capacity_(capacity),
      history_capacity_(std::min<std::size_t>(capacity * 4U, 4096U)),
      monotonic_ns_(monotonic_ns ? std::move(monotonic_ns)
                                : Clock(default_monotonic_ns)) {
    if (capacity_ == 0U || capacity_ > 4096U) {
        throw std::invalid_argument("Runtime 控制门容量必须位于 1～4096");
    }
}

std::string RuntimeControlGate::binary_id(
    const std::array<std::uint8_t, 16>& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::uint64_t RuntimeControlGate::scope_key(
    std::uint32_t node_id, std::uint32_t resource_id) noexcept {
    return (static_cast<std::uint64_t>(node_id) << 32U) | resource_id;
}

void RuntimeControlGate::expire(std::uint64_t now_ns) {
    for (auto iterator = leases_.begin(); iterator != leases_.end();) {
        if (iterator->second.deadline_ns > now_ns) {
            ++iterator;
            continue;
        }
        leases_by_scope_.erase(scope_key(iterator->second.node_id,
                                         iterator->second.resource_id));
        iterator = leases_.erase(iterator);
    }
    for (auto iterator = completed_.begin(); iterator != completed_.end();) {
        if (iterator->second.retain_until_ns > now_ns) {
            ++iterator;
        } else {
            iterator = completed_.erase(iterator);
        }
    }
}

void RuntimeControlGate::acquire(
    const RuntimeControlAcquireRequest& request,
    const std::array<std::uint8_t, 16>& current_daemon_instance_id,
    std::uint64_t node_generation,
    const protocol::ResourceDescriptor& descriptor,
    const protocol::ResourceContract& contract) {
    if (request.version != kRuntimeControlIpcVersion ||
        !nonzero(request.lease_id) ||
        !valid_identity(request.owner_key_id) || request.node_id == 0U ||
        request.node_id > 127U || request.resource_id == 0U ||
        request.ttl_ms == 0U || request.ttl_ms > kMaximumRuntimeControlTtlMs) {
        reject(RuntimeControlError::InvalidRequest,
               "Runtime 控制租约登记字段无效");
    }
    if (request.daemon_instance_id != current_daemon_instance_id) {
        reject(RuntimeControlError::DaemonIdentityMismatch,
               "Runtime 控制租约绑定了其他 toolbusd 实例");
    }
    if (request.permissions != kRuntimePermissionGpioWrite) {
        reject(RuntimeControlError::PermissionDenied,
               "Runtime 控制租约没有唯一的 GPIO 写权限");
    }
    if (descriptor.resource_id != request.resource_id ||
        descriptor.type != protocol::ResourceType::Gpio ||
        contract.resource_id != request.resource_id ||
        (contract.access_flags & protocol::kResourceAccessWritable) == 0U ||
        (contract.access_flags &
         protocol::kResourceAccessExclusiveWrite) == 0U ||
        (contract.access_flags &
         protocol::kResourceAccessLeaseSupported) == 0U ||
        (contract.access_flags & protocol::kResourceAccessLeaseRequired) !=
            0U) {
        reject(RuntimeControlError::ContractRejected,
               "目标资源不是允许独占写入的静态 GPIO 合同");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now_ns = monotonic_ns_();
    expire(now_ns);
    const auto lease_key = binary_id(request.lease_id);
    const auto found = leases_.find(lease_key);
    if (found != leases_.end()) {
        const auto& old = found->second;
        if (old.owner_key_id != request.owner_key_id ||
            old.permissions != request.permissions ||
            old.node_id != request.node_id ||
            old.resource_id != request.resource_id ||
            old.node_generation != node_generation) {
            reject(RuntimeControlError::LeaseConflict,
                   "Runtime 控制租约 ID 已绑定其他身份、范围或节点代次");
        }
        return;
    }
    const auto scope = scope_key(request.node_id, request.resource_id);
    if (in_flight_scopes_.find(scope) != in_flight_scopes_.end()) {
        reject(RuntimeControlError::LeaseConflict,
               "目标 GPIO 仍有 Runtime 命令在执行");
    }
    if (leases_by_scope_.find(scope) != leases_by_scope_.end()) {
        reject(RuntimeControlError::LeaseConflict,
               "目标 GPIO 已由其他 Runtime 控制租约占用");
    }
    if (leases_.size() >= capacity_) {
        reject(RuntimeControlError::CapacityExceeded,
               "Runtime 控制租约已达到上限");
    }
    const auto ttl_ns = static_cast<std::uint64_t>(request.ttl_ms) *
                        1000000ULL;
    const auto deadline = std::numeric_limits<std::uint64_t>::max() - now_ns <
                                  ttl_ns
                              ? std::numeric_limits<std::uint64_t>::max()
                              : now_ns + ttl_ns;
    leases_.emplace(
        lease_key,
        LeaseState{request.lease_id, request.owner_key_id,
                   request.permissions, request.node_id,
                   request.resource_id, node_generation, deadline});
    leases_by_scope_[scope] = lease_key;
}

RuntimeGpioWriteResult RuntimeControlGate::gpio_write(
    const RuntimeGpioWriteRequest& request,
    const std::array<std::uint8_t, 16>& current_daemon_instance_id,
    std::uint64_t node_generation,
    const protocol::ResourceDescriptor& descriptor,
    const protocol::ResourceContract& contract,
    const GpioWriter& writer) {
    if (request.version != kRuntimeControlIpcVersion ||
        !nonzero(request.lease_id) ||
        !valid_identity(request.owner_key_id) ||
        !valid_idempotency(request.idempotency_key) ||
        request.node_id == 0U || request.node_id > 127U ||
        request.resource_id == 0U || !writer) {
        reject(RuntimeControlError::InvalidRequest,
               "Runtime GPIO 写请求字段无效");
    }
    if (request.daemon_instance_id != current_daemon_instance_id) {
        reject(RuntimeControlError::DaemonIdentityMismatch,
               "Runtime GPIO 写请求绑定了其他 toolbusd 实例");
    }
    if (request.permissions != kRuntimePermissionGpioWrite) {
        reject(RuntimeControlError::PermissionDenied,
               "Runtime GPIO 写请求权限不完整或包含未知位");
    }
    if (descriptor.resource_id != request.resource_id ||
        descriptor.type != protocol::ResourceType::Gpio ||
        contract.resource_id != request.resource_id ||
        (contract.access_flags & protocol::kResourceAccessWritable) == 0U ||
        (contract.access_flags &
         protocol::kResourceAccessExclusiveWrite) == 0U ||
        (contract.access_flags &
         protocol::kResourceAccessLeaseSupported) == 0U ||
        (contract.access_flags & protocol::kResourceAccessLeaseRequired) !=
            0U) {
        reject(RuntimeControlError::ContractRejected,
               "目标资源不是允许独占写入的静态 GPIO 合同");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto now_ns = monotonic_ns_();
    expire(now_ns);
    const auto lease_key = binary_id(request.lease_id);
    auto idempotency_key = request.owner_key_id;
    idempotency_key.push_back('\0');
    idempotency_key += request.idempotency_key;
    const auto scope = scope_key(request.node_id, request.resource_id);
    if (!scope_available_.wait_for(lock, kSameScopeWait, [&] {
            return in_flight_scopes_.find(scope) == in_flight_scopes_.end();
        })) {
        reject(RuntimeControlError::LeaseConflict,
               "同一 GPIO 的前序 Runtime 命令尚未结束");
    }
    now_ns = monotonic_ns_();
    expire(now_ns);
    auto lease = leases_.find(lease_key);
    if (lease == leases_.end()) {
        reject(RuntimeControlError::LeaseNotFound,
               "Runtime GPIO 写入引用了未登记或已过期的租约");
    } else if (lease->second.owner_key_id != request.owner_key_id ||
               lease->second.permissions != request.permissions ||
               lease->second.node_id != request.node_id ||
               lease->second.resource_id != request.resource_id) {
        reject(RuntimeControlError::LeaseConflict,
               "Runtime 控制租约身份或范围不匹配");
    } else if (lease->second.node_generation != node_generation) {
        reject(RuntimeControlError::LeaseExpired,
               "目标节点代次已变化，Runtime 控制租约失效");
    }
    const auto completed = completed_.find(idempotency_key);
    if (completed != completed_.end()) {
        const auto& old = completed->second;
        if (old.lease_id != request.lease_id ||
            old.owner_key_id != request.owner_key_id ||
            old.node_id != request.node_id ||
            old.resource_id != request.resource_id ||
            old.value != request.value) {
            reject(RuntimeControlError::IdempotencyConflict,
                   "Runtime GPIO 幂等键已用于不同命令");
        }
        auto replay = old.result;
        replay.replayed = true;
        return replay;
    }
    if (completed_.size() >= history_capacity_) {
        reject(RuntimeControlError::CapacityExceeded,
               "Runtime GPIO 幂等历史已达到上限");
    }
    auto object = gpio_objects_.find(scope);
    if (object != gpio_objects_.end() &&
        object->second.node_generation != node_generation) {
        gpio_objects_.erase(object);
        object = gpio_objects_.end();
    }
    if (object == gpio_objects_.end() && gpio_objects_.size() >= capacity_) {
        reject(RuntimeControlError::CapacityExceeded,
               "Runtime GPIO 对象表已达到上限");
    }
    const auto existing_object_id =
        object == gpio_objects_.end()
            ? std::optional<std::uint32_t>{}
            : std::optional<std::uint32_t>{object->second.object_id};
    in_flight_scopes_.insert(scope);
    lock.unlock();
    std::uint32_t object_id = 0U;
    try {
        object_id = writer(existing_object_id);
    } catch (...) {
        lock.lock();
        in_flight_scopes_.erase(scope);
        lock.unlock();
        scope_available_.notify_all();
        throw;
    }
    lock.lock();
    in_flight_scopes_.erase(scope);
    scope_available_.notify_all();
    if (object_id == 0U) {
        reject(RuntimeControlError::InvalidRequest,
               "Runtime GPIO 写入器返回了零对象 ID");
    }
    gpio_objects_[scope] = {object_id, node_generation};
    RuntimeGpioWriteResult result;
    result.object_id = object_id;
    result.value = request.value;
    const auto completed_ns = monotonic_ns_();
    const auto retain_until =
        std::numeric_limits<std::uint64_t>::max() - completed_ns <
                kIdempotencyRetentionNs
            ? std::numeric_limits<std::uint64_t>::max()
            : completed_ns + kIdempotencyRetentionNs;
    completed_.emplace(
        idempotency_key,
        CompletedCommand{request.lease_id, request.owner_key_id,
                         request.node_id, request.resource_id,
                         request.value, result,
                         retain_until});
    return result;
}

void RuntimeControlGate::release(
    const RuntimeControlReleaseRequest& request,
    const std::array<std::uint8_t, 16>& current_daemon_instance_id) {
    if (request.version != kRuntimeControlIpcVersion ||
        !nonzero(request.lease_id) || !valid_identity(request.owner_key_id)) {
        reject(RuntimeControlError::InvalidRequest,
               "Runtime 控制租约释放字段无效");
    }
    if (request.daemon_instance_id != current_daemon_instance_id) {
        reject(RuntimeControlError::DaemonIdentityMismatch,
               "Runtime 控制租约绑定了其他 toolbusd 实例");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now_ns = monotonic_ns_();
    expire(now_ns);
    const auto lease_key = binary_id(request.lease_id);
    const auto found = leases_.find(lease_key);
    if (found == leases_.end()) {
        // 未执行过 GPIO 命令的本地租约不会进入 daemon；释放保持幂等。
        return;
    }
    if (found->second.owner_key_id != request.owner_key_id) {
        reject(RuntimeControlError::PermissionDenied,
               "不能释放其他身份的 Runtime 控制租约");
    }
    if (in_flight_scopes_.find(scope_key(found->second.node_id,
                                         found->second.resource_id)) !=
        in_flight_scopes_.end()) {
        reject(RuntimeControlError::LeaseConflict,
               "Runtime GPIO 命令执行期间不能释放租约");
    }
    leases_by_scope_.erase(scope_key(found->second.node_id,
                                     found->second.resource_id));
    leases_.erase(found);
}

std::size_t RuntimeControlGate::active_lease_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(monotonic_ns_());
    return leases_.size();
}

}  // namespace remotebsp::toolbusd
