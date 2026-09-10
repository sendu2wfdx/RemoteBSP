#include "remotebsp/toolbusd/runtime_control.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
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
                                       Clock monotonic_ns,
                                       bool start_expiry_worker)
    : capacity_(capacity),
      history_capacity_(std::min<std::size_t>(capacity * 4U, 4096U)),
      monotonic_ns_(monotonic_ns ? std::move(monotonic_ns)
                                : Clock(default_monotonic_ns)) {
    if (capacity_ == 0U || capacity_ > 4096U) {
        throw std::invalid_argument("Runtime 控制门容量必须位于 1～4096");
    }
    expiry_worker_enabled_ = start_expiry_worker;
    if (expiry_worker_enabled_) {
        expiry_worker_ = std::thread(&RuntimeControlGate::expiry_loop, this);
    }
}

RuntimeControlGate::~RuntimeControlGate() noexcept {
    try {
        static_cast<void>(shutdown());
    } catch (...) {
        // 析构路径绝不能因分配失败、时钟异常或清理实现异常 terminate。
        // 生产路径应在 transport 仍可用时显式调用 shutdown() 并检查结果。
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            expiry_changed_.notify_all();
        } catch (...) {
        }
        if (expiry_worker_.joinable()) {
            try {
                expiry_worker_.join();
            } catch (...) {
                // 绝不能 detach：后台线程仍持有 this，析构后继续运行会
                // 形成 use-after-free。join 失败属于无法安全恢复的进程级
                // 不变量破坏，受控终止比留下悬空线程更安全。
                std::terminate();
            }
        }
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

void RuntimeControlGate::erase_lease_locked(const std::string& lease_key) {
    const auto lease = leases_.find(lease_key);
    if (lease == leases_.end()) {
        return;
    }
    leases_by_scope_.erase(scope_key(lease->second.node_id,
                                     lease->second.resource_id));
    leases_.erase(lease);
}

std::vector<RuntimeControlGate::CleanupTask>
RuntimeControlGate::collect_expired_locked(std::uint64_t now_ns) {
    std::vector<CleanupTask> tasks;
    try {
        for (auto iterator = leases_.begin(); iterator != leases_.end();) {
            if (iterator->second.deadline_ns > now_ns ||
                iterator->second.cleanup_failed) {
                ++iterator;
                continue;
            }
            const auto scope = scope_key(iterator->second.node_id,
                                         iterator->second.resource_id);
            if (in_flight_scopes_.find(scope) != in_flight_scopes_.end()) {
                // 首次 CREATE 尚未返回时对象表可能为空；此时删除租约会让
                // 随后返回的 object_id 成为无法清理的孤儿。
                ++iterator;
                continue;
            }
            const auto object = gpio_objects_.find(scope);
            if (object == gpio_objects_.end()) {
                leases_by_scope_.erase(scope);
                iterator = leases_.erase(iterator);
                continue;
            }
            // 先完成所有可能抛异常的复制/扩容，再发布 in-flight 占位。
            // 若后续任一作用域准备失败，catch 会回滚本批次全部占位。
            tasks.push_back({scope, iterator->first, object->second});
            if (!in_flight_scopes_.insert(scope).second) {
                tasks.pop_back();
            }
            ++iterator;
        }
    } catch (...) {
        for (const auto& task : tasks) {
            in_flight_scopes_.erase(task.scope);
        }
        scope_available_.notify_all();
        expiry_changed_.notify_all();
        throw;
    }
    for (auto iterator = completed_.begin(); iterator != completed_.end();) {
        if (iterator->second.retain_until_ns > now_ns) {
            ++iterator;
        } else {
            iterator = completed_.erase(iterator);
        }
    }
    return tasks;
}

RuntimeControlGate::CleanupResult RuntimeControlGate::stop_in_flight_scope(
    std::uint64_t scope, const std::string& lease_key,
    std::uint64_t node_generation,
    const std::array<std::uint8_t, 16>& expected_node_uuid,
    std::uint32_t object_id, bool close_pending,
    const GpioSafeStopper& safe_stopper, const GpioCloser& closer,
    bool retain_failed) {
    if (!close_pending) {
        try {
            safe_stopper(static_cast<std::uint32_t>(scope >> 32U),
                         node_generation, expected_node_uuid, object_id);
            close_pending = true;
        } catch (...) {
        }
        if (close_pending) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto object = gpio_objects_.find(scope);
            if (object != gpio_objects_.end() &&
                object->second.object_id == object_id) {
                // Close 响应若丢失，后续 release 只重试幂等 Close，不能先
                // 对可能已经不存在的对象 GPIO_WRITE 而误判无法清理。
                object->second.close_pending = true;
            }
        }
    }

    bool closed = false;
    if (close_pending) {
        try {
            closer(static_cast<std::uint32_t>(scope >> 32U),
                   node_generation, expected_node_uuid, object_id);
            closed = true;
        } catch (...) {
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_scopes_.erase(scope);
    if (closed) {
        gpio_objects_.erase(scope);
        erase_lease_locked(lease_key);
    } else if (retain_failed) {
        const auto lease = leases_.find(lease_key);
        if (lease != leases_.end()) {
            lease->second.cleanup_failed = true;
        }
    }
    scope_available_.notify_all();
    expiry_changed_.notify_all();
    if (closed) {
        return CleanupResult::Closed;
    }
    return close_pending ? CleanupResult::CloseUncertain
                         : CleanupResult::SafeLowFailed;
}

std::size_t RuntimeControlGate::finish_cleanup(
    std::vector<CleanupTask> tasks, bool retain_failed) {
    std::size_t failures = 0U;
    for (auto& task : tasks) {
        if (stop_in_flight_scope(
                task.scope, task.lease_key,
                task.object.node_generation,
                task.object.expected_node_uuid,
                task.object.object_id, task.object.close_pending,
                task.object.safe_stopper, task.object.closer,
                retain_failed) != CleanupResult::Closed) {
            ++failures;
        }
    }
    return failures;
}

void RuntimeControlGate::acquire(
    const RuntimeControlAcquireRequest& request,
    const std::array<std::uint8_t, 16>& current_daemon_instance_id,
    std::uint64_t node_generation,
    const protocol::ResourceDescriptor& descriptor,
    const protocol::ResourceContract& contract) {
    if (request.version != kRuntimeControlIpcVersion ||
        !nonzero(request.lease_id) ||
        !nonzero(request.expected_node_uuid) ||
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
    if (stopping_) {
        reject(RuntimeControlError::SafeStopFailed,
               "Runtime 控制门正在关闭，拒绝登记新租约");
    }
    const auto now_ns = monotonic_ns_();
    const auto lease_key = binary_id(request.lease_id);
    const auto found = leases_.find(lease_key);
    if (found != leases_.end()) {
        const auto& old = found->second;
        if (old.owner_key_id != request.owner_key_id ||
            old.permissions != request.permissions ||
            old.expected_node_uuid != request.expected_node_uuid ||
            old.node_id != request.node_id ||
            old.resource_id != request.resource_id ||
            old.node_generation != node_generation) {
            reject(RuntimeControlError::LeaseConflict,
                   "Runtime 控制租约 ID 已绑定其他身份、范围或节点代次");
        }
        return;
    }
    const auto scope = scope_key(request.node_id, request.resource_id);
    const auto occupied = leases_by_scope_.find(scope);
    if (occupied != leases_by_scope_.end()) {
        const auto old = leases_.find(occupied->second);
        if (old != leases_.end() && old->second.cleanup_failed &&
            old->second.node_generation != node_generation) {
            // 节点换代意味着未知/未清理的旧 MCU 对象已经随会话重建消失，
            // 这是 daemon 重启之外唯一允许解除 poison 的路径。
            gpio_objects_.erase(scope);
            erase_lease_locked(old->first);
        }
    }
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
    const auto inserted_lease = leases_.emplace(
        lease_key,
        LeaseState{request.lease_id, request.expected_node_uuid,
                   request.owner_key_id,
                   request.permissions, request.node_id,
                   request.resource_id, node_generation, deadline});
    if (!inserted_lease.second) {
        reject(RuntimeControlError::LeaseConflict,
               "Runtime 控制租约 ID 并发登记冲突");
    }
    try {
        const auto inserted_scope = leases_by_scope_.emplace(scope, lease_key);
        if (!inserted_scope.second) {
            reject(RuntimeControlError::LeaseConflict,
                   "目标 GPIO 的独占索引登记冲突");
        }
    } catch (...) {
        if (inserted_lease.second) {
            leases_.erase(inserted_lease.first);
        }
        throw;
    }
    expiry_changed_.notify_all();
}

RuntimeGpioWriteResult RuntimeControlGate::gpio_write(
    const RuntimeGpioWriteRequest& request,
    const std::array<std::uint8_t, 16>& current_daemon_instance_id,
    std::uint64_t node_generation,
    const protocol::ResourceDescriptor& descriptor,
    const protocol::ResourceContract& contract,
    const GpioIo& io) {
    if (request.version != kRuntimeControlIpcVersion ||
        !nonzero(request.lease_id) ||
        !nonzero(request.expected_node_uuid) ||
        !valid_identity(request.owner_key_id) ||
        !valid_idempotency(request.idempotency_key) ||
        request.node_id == 0U || request.node_id > 127U ||
        request.resource_id == 0U || !io.create_low || !io.write_value ||
        !io.safe_stop || !io.close) {
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
    if (stopping_) {
        reject(RuntimeControlError::SafeStopFailed,
               "Runtime 控制门正在关闭，拒绝新的 GPIO 写入");
    }
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
    if (stopping_) {
        reject(RuntimeControlError::SafeStopFailed,
               "Runtime 控制门正在关闭，拒绝新的 GPIO 写入");
    }
    // 等待同作用域前序命令期间租约可能到期。此时不在持锁路径执行远端
    // 停机，而是拒绝新写；后台清理线程会完成写低或保留故障占位。
    auto lease = leases_.find(lease_key);
    if (lease == leases_.end()) {
        reject(RuntimeControlError::LeaseNotFound,
               "Runtime GPIO 写入引用了未登记或已过期的租约");
    } else if (lease->second.deadline_ns <= monotonic_ns_()) {
        reject(RuntimeControlError::LeaseNotFound,
               "Runtime GPIO 写入引用了未登记或已过期的租约");
    } else if (lease->second.cleanup_failed) {
        reject(RuntimeControlError::SafeStopFailed,
               "Runtime GPIO 作用域处于未知对象或安全停机失败状态");
    } else if (lease->second.owner_key_id != request.owner_key_id ||
               lease->second.expected_node_uuid !=
                   request.expected_node_uuid ||
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
    if (object == gpio_objects_.end() &&
        gpio_objects_.size() >= capacity_) {
        reject(RuntimeControlError::CapacityExceeded,
               "Runtime GPIO 活动对象表已达到上限");
    }
    // 在任何远端 I/O 前分配完整幂等节点。执行成功后只原地更新固定大小
    // 字段，不再发生字符串复制或容器扩容；失败路径必须删除本 pending。
    RuntimeGpioWriteResult pending_result;
    pending_result.value = request.value;
    const auto pending = completed_.emplace(
        idempotency_key,
        CompletedCommand{request.lease_id, request.owner_key_id,
                         request.node_id, request.resource_id,
                         request.value, pending_result,
                         std::numeric_limits<std::uint64_t>::max()});
    if (!pending.second) {
        reject(RuntimeControlError::IdempotencyConflict,
               "Runtime GPIO 幂等 pending 登记冲突");
    }
    const bool creating = object == gpio_objects_.end();
    std::uint32_t object_id = creating ? 0U : object->second.object_id;
    bool object_close_pending =
        creating ? false : object->second.close_pending;
    try {
        if (creating) {
            // stopper 的复制与对象表扩容必须发生在远端 CREATE 之前。这样
            // 本地异常不会留下 MCU 对象；object_id 在响应后原地更新。
            GpioObject staged_object{0U, node_generation,
                                     request.expected_node_uuid,
                                     false, io.safe_stop, io.close};
            const auto inserted = gpio_objects_.emplace(
                scope, std::move(staged_object));
            if (!inserted.second) {
                reject(RuntimeControlError::LeaseConflict,
                       "Runtime GPIO 对象占位登记冲突");
            }
        }
        if (!in_flight_scopes_.insert(scope).second) {
            if (creating) {
                gpio_objects_.erase(scope);
            }
            reject(RuntimeControlError::LeaseConflict,
                   "Runtime GPIO 作用域占位冲突");
        }
    } catch (...) {
        if (creating) {
            gpio_objects_.erase(scope);
        }
        completed_.erase(idempotency_key);
        throw;
    }
    lock.unlock();

    if (creating) {
        try {
            // 首次创建固定使用安全低电平。即使成功响应丢失，本次命令也
            // 不会把引脚拉高；但未知 object_id 必须毒化 scope。
            object_id = io.create_low();
        } catch (...) {
            lock.lock();
            gpio_objects_.erase(scope);
            completed_.erase(idempotency_key);
            const auto active = leases_.find(lease_key);
            if (active != leases_.end()) {
                active->second.cleanup_failed = true;
            }
            in_flight_scopes_.erase(scope);
            lock.unlock();
            scope_available_.notify_all();
            expiry_changed_.notify_all();
            throw;
        }
        if (object_id == 0U) {
            lock.lock();
            gpio_objects_.erase(scope);
            completed_.erase(idempotency_key);
            const auto active = leases_.find(lease_key);
            if (active != leases_.end()) {
                active->second.cleanup_failed = true;
            }
            in_flight_scopes_.erase(scope);
            lock.unlock();
            scope_available_.notify_all();
            expiry_changed_.notify_all();
            reject(RuntimeControlError::InvalidRequest,
                   "Runtime GPIO_CREATE 未返回可清理的对象 ID");
        }
        // 在任何非安全电平写入之前登记 object_id 与 stopper。reaper 会
        // 因 in-flight 保留租约，writer 返回后本线程负责到期清理。
        lock.lock();
        const auto tracked = gpio_objects_.find(scope);
        if (tracked == gpio_objects_.end()) {
            completed_.erase(idempotency_key);
            const auto active = leases_.find(lease_key);
            if (active != leases_.end()) {
                active->second.cleanup_failed = true;
            }
            in_flight_scopes_.erase(scope);
            lock.unlock();
            scope_available_.notify_all();
            expiry_changed_.notify_all();
            // 在锁保护和 in-flight 不变量成立时不应发生；若发生则对象 ID
            // 已知，仍必须立即尝试写低，不能让 shutdown 永久等待。
            try {
                io.safe_stop(request.node_id, node_generation,
                             request.expected_node_uuid, object_id);
            } catch (...) {
            }
            reject(RuntimeControlError::SafeStopFailed,
                   "Runtime GPIO 对象登记不变量失效");
        }
        tracked->second.object_id = object_id;
        lock.unlock();
    }

    // CREATE 可能在远端等待较久；对象登记后、任何目标值写入前必须再次
    // 核对关闭状态与租约期限。否则 shutdown 已经开始或租约已经过期时，
    // 第二阶段仍可能把 GPIO 短暂拉高，再由事后清理写低。
    lock.lock();
    const auto prewrite_lease = leases_.find(lease_key);
    std::uint64_t prewrite_ns = 0U;
    bool prewrite_clock_failed = false;
    try {
        prewrite_ns = monotonic_ns_();
    } catch (...) {
        prewrite_clock_failed = true;
    }
    const bool prewrite_must_stop =
        stopping_ || prewrite_lease == leases_.end() ||
        prewrite_clock_failed ||
        prewrite_lease->second.deadline_ns <= prewrite_ns;
    const bool will_write_value = !creating || request.value;
    lock.unlock();
    if (prewrite_must_stop) {
        const auto cleanup = stop_in_flight_scope(
                scope, lease_key, node_generation,
                request.expected_node_uuid, object_id,
                object_close_pending, io.safe_stop, io.close, true);
        lock.lock();
        completed_.erase(idempotency_key);
        lock.unlock();
        expiry_changed_.notify_all();
        if (cleanup == CleanupResult::SafeLowFailed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "目标值写入前租约失效且安全写低未获得确定成功");
        }
        if (cleanup == CleanupResult::CloseUncertain) {
            reject(RuntimeControlError::SafeStopFailed,
                   "目标值写入前租约失效且 GPIO_CLOSE 未获得确定成功");
        }
        if (prewrite_clock_failed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "目标值写入前单调时钟不可验证，已安全写低并终止租约");
        }
        reject(RuntimeControlError::LeaseExpired,
               "目标值写入前租约已过期或 daemon 正在关闭");
    }

    std::exception_ptr write_error;
    try {
        if (will_write_value) {
            io.write_value(object_id, request.value);
        }
    } catch (...) {
        write_error = std::current_exception();
    }
    if (write_error) {
        const auto cleanup = stop_in_flight_scope(
                scope, lease_key, node_generation,
                request.expected_node_uuid, object_id, false,
                io.safe_stop, io.close, true);
        lock.lock();
        completed_.erase(idempotency_key);
        lock.unlock();
        expiry_changed_.notify_all();
        if (cleanup == CleanupResult::SafeLowFailed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "GPIO 写响应不确定且安全写低未获得确定成功");
        }
        if (cleanup == CleanupResult::CloseUncertain) {
            reject(RuntimeControlError::SafeStopFailed,
                   "GPIO 写响应不确定且 GPIO_CLOSE 未获得确定成功");
        }
        std::rethrow_exception(write_error);
    }

    lock.lock();
    const auto active = leases_.find(lease_key);
    std::uint64_t completed_ns = 0U;
    bool clock_failed = false;
    try {
        completed_ns = monotonic_ns_();
    } catch (...) {
        clock_failed = true;
    }
    const bool must_stop = stopping_ || active == leases_.end() ||
                           clock_failed ||
                           active->second.deadline_ns <= completed_ns;
    if (must_stop) {
        lock.unlock();
        const auto cleanup = stop_in_flight_scope(
                scope, lease_key, node_generation,
                request.expected_node_uuid, object_id,
                object_close_pending, io.safe_stop, io.close, true);
        lock.lock();
        completed_.erase(idempotency_key);
        lock.unlock();
        expiry_changed_.notify_all();
        if (cleanup == CleanupResult::SafeLowFailed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "跨期限 GPIO 写入后安全写低未获得确定成功");
        }
        if (cleanup == CleanupResult::CloseUncertain) {
            reject(RuntimeControlError::SafeStopFailed,
                   "跨期限 GPIO 写入后 GPIO_CLOSE 未获得确定成功");
        }
        if (clock_failed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "GPIO 写入完成后单调时钟不可验证，已安全写低并终止租约");
        }
        reject(RuntimeControlError::LeaseExpired,
               "GPIO 写入完成时租约已过期或 daemon 正在关闭");
    }
    RuntimeGpioWriteResult result;
    result.object_id = object_id;
    result.value = request.value;
    const auto retain_until =
        std::numeric_limits<std::uint64_t>::max() - completed_ns <
                kIdempotencyRetentionNs
            ? std::numeric_limits<std::uint64_t>::max()
            : completed_ns + kIdempotencyRetentionNs;
    const auto published = completed_.find(idempotency_key);
    if (published == completed_.end()) {
        lock.unlock();
        static_cast<void>(stop_in_flight_scope(
            scope, lease_key, node_generation,
            request.expected_node_uuid, object_id,
            object_close_pending, io.safe_stop, io.close, true));
        reject(RuntimeControlError::SafeStopFailed,
               "Runtime GPIO 幂等 pending 不变量失效，已进入安全清理");
    }
    // pending 节点已经拥有全部动态字段，此处只更新固定大小值，不分配。
    published->second.result = result;
    published->second.retain_until_ns = retain_until;
    in_flight_scopes_.erase(scope);
    scope_available_.notify_all();
    expiry_changed_.notify_all();
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
    std::unique_lock<std::mutex> lock(mutex_);
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
    const auto scope = scope_key(found->second.node_id,
                                 found->second.resource_id);
    const auto object = gpio_objects_.find(scope);
    if (object == gpio_objects_.end()) {
        if (found->second.cleanup_failed) {
            reject(RuntimeControlError::SafeStopFailed,
                   "GPIO_CREATE 结果未知，节点换代前不能释放清理占位");
        }
        erase_lease_locked(lease_key);
        expiry_changed_.notify_all();
        return;
    }
    // CleanupTask/std::function 的复制与分配先完成，再发布 in-flight。
    // 任一异常都保持 lease/object 原样，可由调用者重试。
    CleanupTask task{scope, lease_key, object->second};
    if (!in_flight_scopes_.insert(scope).second) {
        reject(RuntimeControlError::LeaseConflict,
               "Runtime GPIO 清理占位冲突");
    }
    lock.unlock();
    const auto cleanup = stop_in_flight_scope(
        task.scope, task.lease_key, task.object.node_generation,
        task.object.expected_node_uuid, task.object.object_id,
        task.object.close_pending, task.object.safe_stopper,
        task.object.closer, true);
    if (cleanup == CleanupResult::SafeLowFailed) {
        reject(RuntimeControlError::SafeStopFailed,
               "GPIO 安全写低未获得确定成功，租约保持故障占位");
    }
    if (cleanup == CleanupResult::CloseUncertain) {
        reject(RuntimeControlError::SafeStopFailed,
               "GPIO_CLOSE 未获得确定成功，租约保持故障占位");
    }
}

std::size_t RuntimeControlGate::active_lease_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now_ns = monotonic_ns_();
    return static_cast<std::size_t>(std::count_if(
        leases_.begin(), leases_.end(), [&](const auto& entry) {
            return entry.second.deadline_ns > now_ns;
        }));
}

std::size_t RuntimeControlGate::reap_expired() {
    std::vector<CleanupTask> tasks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks = collect_expired_locked(monotonic_ns_());
    }
    return finish_cleanup(std::move(tasks), true);
}

std::size_t RuntimeControlGate::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutdown_complete_) {
            return shutdown_failures_;
        }
        if (shutdown_in_progress_) {
            shutdown_changed_.wait(lock,
                                   [&] { return shutdown_complete_; });
            return shutdown_failures_;
        }
        shutdown_in_progress_ = true;
        stopping_ = true;
        expiry_changed_.notify_all();
    }
    if (expiry_worker_.joinable()) {
        expiry_worker_.join();
    }
    std::vector<CleanupTask> tasks;
    std::size_t unknown_object_failures = 0U;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        scope_available_.wait(lock, [&] { return in_flight_scopes_.empty(); });
        // 以对象表为权威兜底，而不是只遍历 leases；即使内部不变量曾被
        // 破坏形成孤儿对象，shutdown 仍会尝试写低。
        try {
            tasks.reserve(gpio_objects_.size());
            for (const auto& object : gpio_objects_) {
                std::string lease_key;
                const auto owner = leases_by_scope_.find(object.first);
                if (owner != leases_by_scope_.end()) {
                    lease_key = owner->second;
                }
                // 先完成任务复制，再发布占位；异常时统一回滚本批次。
                tasks.push_back({object.first, std::move(lease_key),
                                 object.second});
                if (!in_flight_scopes_.insert(object.first).second) {
                    tasks.pop_back();
                }
            }
        } catch (...) {
            for (const auto& task : tasks) {
                in_flight_scopes_.erase(task.scope);
            }
            scope_available_.notify_all();
            expiry_changed_.notify_all();
            shutdown_failures_ = std::max<std::size_t>(
                1U, gpio_objects_.size());
            shutdown_in_progress_ = false;
            shutdown_complete_ = true;
            shutdown_changed_.notify_all();
            return shutdown_failures_;
        }
        for (const auto& lease : leases_) {
            const auto scope = scope_key(lease.second.node_id,
                                         lease.second.resource_id);
            if (lease.second.cleanup_failed &&
                gpio_objects_.find(scope) == gpio_objects_.end()) {
                ++unknown_object_failures;
            }
        }
    }
    const auto failures = finish_cleanup(std::move(tasks), true) +
                          unknown_object_failures +
                          (expiry_worker_failed_ ? 1U : 0U);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failures == 0U) {
            leases_.clear();
            leases_by_scope_.clear();
            gpio_objects_.clear();
        }
        shutdown_failures_ = failures;
        shutdown_in_progress_ = false;
        shutdown_complete_ = true;
        shutdown_changed_.notify_all();
    }
    return failures;
}

void RuntimeControlGate::expiry_loop() {
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            const auto now_ns = monotonic_ns_();
            auto tasks = collect_expired_locked(now_ns);
            if (!tasks.empty()) {
                lock.unlock();
                static_cast<void>(finish_cleanup(std::move(tasks), true));
                lock.lock();
                continue;
            }
            std::uint64_t earliest =
                std::numeric_limits<std::uint64_t>::max();
            for (const auto& lease : leases_) {
                const auto scope = scope_key(lease.second.node_id,
                                             lease.second.resource_id);
                if (!lease.second.cleanup_failed &&
                    in_flight_scopes_.find(scope) ==
                        in_flight_scopes_.end()) {
                    earliest = std::min(earliest, lease.second.deadline_ns);
                }
            }
            for (const auto& command : completed_) {
                earliest = std::min(earliest,
                                    command.second.retain_until_ns);
            }
            if (earliest == std::numeric_limits<std::uint64_t>::max()) {
                expiry_changed_.wait(lock);
                continue;
            }
            const auto delay_ns = earliest > now_ns ? earliest - now_ns : 0U;
            expiry_changed_.wait_for(lock,
                                     std::chrono::nanoseconds(delay_ns));
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        expiry_worker_failed_ = true;
        stopping_ = true;
        scope_available_.notify_all();
    }
}

}  // namespace remotebsp::toolbusd
