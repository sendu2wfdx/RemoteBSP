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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace remotebsp::toolbusd {

constexpr std::uint16_t kRuntimeControlIpcVersion = 2U;
constexpr std::uint16_t kRuntimePermissionGpioWrite = 0x0001U;
constexpr std::uint16_t kRuntimePermissionPwmWrite = 0x0002U;
constexpr std::uint16_t kRuntimePermissionTimedBitstreamWrite = 0x0004U;
constexpr std::uint16_t kRuntimePermissionBusReset = 0x0008U;
constexpr std::size_t kMaximumRuntimeControlIdentityBytes = 64U;
constexpr std::size_t kMaximumRuntimeControlIdempotencyBytes = 64U;
constexpr std::uint32_t kMaximumRuntimeControlTtlMs = 30000U;

struct RuntimeControlAcquireRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
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
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionGpioWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
    bool value{};
};

struct RuntimePwmConfigureRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionPwmWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
};

struct RuntimePwmStopRequest : RuntimePwmConfigureRequest {};

struct RuntimeTimedBitstreamConfigureRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionTimedBitstreamWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
    std::uint32_t bit_period_ns{};
    std::uint32_t zero_high_ns{};
    std::uint32_t one_high_ns{};
    std::uint32_t reset_time_us{};
};

struct RuntimeTimedBitstreamFrameRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionTimedBitstreamWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
    std::uint16_t bit_count{};
    std::vector<std::uint8_t> data;
};

struct RuntimeTimedBitstreamStopRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionTimedBitstreamWrite};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
};

struct RuntimeBusResourceResetRequest {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::string owner_key_id;
    std::uint16_t permissions{kRuntimePermissionBusReset};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::string idempotency_key;
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

struct RuntimePwmResult {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::uint32_t object_id{};
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
    bool replayed{};
};

struct RuntimeTimedBitstreamResult {
    std::uint16_t version{kRuntimeControlIpcVersion};
    std::uint32_t object_id{};
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
    SafeStopFailed,
    ObjectRetired,
};

class RuntimeControlException : public std::runtime_error {
public:
    RuntimeControlException(RuntimeControlError code,
                            const std::string& message);
    RuntimeControlError code() const noexcept;

private:
    RuntimeControlError code_;
};

// toolbusd 内部的最终授权门。全局锁只覆盖校验、资源占位和结果提交；
// 远端 writer 在锁外执行，同一作用域由有界 in-flight 状态串行化。
class RuntimeControlGate {
public:
    using Clock = std::function<std::uint64_t()>;
    using GpioCreator = std::function<std::uint32_t()>;
    using GpioValueWriter = std::function<void(std::uint32_t object_id,
                                               bool value)>;
    using GpioSafeStopper = std::function<void(
        std::uint32_t node_id, std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        std::uint32_t object_id)>;
    using GpioCloser = std::function<void(
        std::uint32_t node_id, std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        std::uint32_t object_id)>;
    struct GpioIo {
        // create_low 必须只以安全低电平创建对象；Gate 在取得对象 ID 后先
        // 登记可清理状态，之后才允许 write_value 写请求值。
        GpioCreator create_low;
        GpioValueWriter write_value;
        GpioSafeStopper safe_stop;
        GpioCloser close;
    };

    // 持久操作账本的回调边界。回调始终在 Gate 锁外执行；pending 返回前
    // 不会调用任何可能改变远端状态的 I/O，committed 返回前也不会把
    // 进程内结果发布给并发重放者。
    enum class DurableRecovery {
        SafeClosed,
        ScopeBlocked,
    };
    using DurableFailure = std::function<void(
        DurableRecovery recovery, RuntimeControlError error)>;
    struct GpioDurability {
        std::function<void()> pending;
        std::function<void(const RuntimeGpioWriteResult&)> committed;
        DurableFailure failed;
    };
    using PwmCreator = std::function<std::uint32_t(
        std::uint32_t frequency_hz, std::uint16_t duty, bool active_low)>;
    using PwmStopper = std::function<void(
        std::uint32_t node_id, std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        std::uint32_t object_id)>;
    using PwmLeaseReleaser = std::function<void(
        std::uint32_t node_id, std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        std::uint32_t resource_id, std::uint64_t remote_lease_id)>;
    struct PwmIo {
        PwmCreator create;
        PwmStopper stop;
    };
    struct PwmDurability {
        std::function<void()> pending;
        std::function<void(const RuntimePwmResult&)> committed;
        DurableFailure failed;
    };
    using TimedBitstreamCreator = std::function<std::uint32_t(
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>;
    using TimedBitstreamWriter = std::function<void(
        std::uint32_t, std::uint16_t, const std::vector<std::uint8_t>&)>;
    struct TimedBitstreamIo {
        TimedBitstreamCreator create;
        TimedBitstreamWriter write;
        PwmStopper stop;
    };
    struct TimedBitstreamDurability {
        std::function<void()> pending;
        std::function<void(const RuntimeTimedBitstreamResult&)> committed;
        DurableFailure failed;
    };

    struct ResolvedReleaseLease {
        std::array<std::uint8_t, 16> expected_node_uuid{};
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        std::uint16_t permissions{};
        // daemon 生命周期内单调递增；同一 lease_id 被重新登记也必须拥有
        // 不同 admission_id，防止旧 Release 终态冒充新租约生命周期。
        std::uint64_t admission_id{};
    };
    struct ReleaseDurability {
        std::function<void(const ResolvedReleaseLease&)> pending;
        std::function<void()> committed;
        DurableFailure failed;
    };

    explicit RuntimeControlGate(std::size_t capacity = 256U,
                                Clock monotonic_ns = {},
                                bool start_expiry_worker = true);
    ~RuntimeControlGate() noexcept;

    void acquire(
        const RuntimeControlAcquireRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract);
    void bind_pwm_remote_lease(
        const std::array<std::uint8_t, 16>& lease_id,
        std::uint64_t remote_lease_id, PwmLeaseReleaser releaser);

    RuntimeGpioWriteResult gpio_write(
        const RuntimeGpioWriteRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract,
        const GpioIo& io,
        const GpioDurability& durability = {});

    RuntimePwmResult pwm_configure(
        const RuntimePwmConfigureRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract, const PwmIo& io,
        const PwmDurability& durability = {});
    RuntimePwmResult pwm_stop(
        const RuntimePwmStopRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract, const PwmIo& io,
        const PwmDurability& durability = {});
    RuntimeTimedBitstreamResult timed_bitstream_configure(
        const RuntimeTimedBitstreamConfigureRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract,
        const TimedBitstreamIo& io,
        const TimedBitstreamDurability& durability = {});
    RuntimeTimedBitstreamResult timed_bitstream_frame(
        const RuntimeTimedBitstreamFrameRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract,
        const TimedBitstreamIo& io,
        const TimedBitstreamDurability& durability = {});
    RuntimeTimedBitstreamResult timed_bitstream_stop(
        const RuntimeTimedBitstreamStopRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        std::uint64_t node_generation,
        const protocol::ResourceDescriptor& descriptor,
        const protocol::ResourceContract& contract,
        const TimedBitstreamIo& io,
        const TimedBitstreamDurability& durability = {});

    // 只读解析当前活动租约，供持久账本在历史回放前核对服务端范围。
    // 返回空值只表示当前 Gate 中没有该租约；身份不匹配仍严格拒绝。
    std::optional<ResolvedReleaseLease> resolve_release_lease(
        const RuntimeControlReleaseRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id);
    void forget_lease_after_remote_reset(
        const RuntimeControlReleaseRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id);

    void release(
        const RuntimeControlReleaseRequest& request,
        const std::array<std::uint8_t, 16>& current_daemon_instance_id,
        const ReleaseDurability& durability = {});

    std::size_t active_lease_count();

    // 立即处理已经到期的租约。返回无法验证安全低电平的资源数；失败资源
    // 会保留占位并阻止后续所有者接管，不影响其他资源继续清理。
    std::size_t reap_expired();

    // daemon 会话结束前停止后台清理，并对所有已创建对象执行安全停机。
    // 返回停机失败数；本方法幂等，且会继续处理其余资源。
    std::size_t shutdown();

private:
    struct LeaseState {
        std::array<std::uint8_t, 16> lease_id{};
        std::array<std::uint8_t, 16> expected_node_uuid{};
        std::string owner_key_id;
        std::uint16_t permissions{};
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        std::uint64_t node_generation{};
        std::uint64_t deadline_ns{};
        std::uint64_t admission_id{};
        bool cleanup_failed{};
        std::uint64_t remote_lease_id{};
        PwmLeaseReleaser remote_lease_releaser;
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
        std::array<std::uint8_t, 16> expected_node_uuid{};
        // 仅当安全写低已确定成功、但 Close 尚未确定成功时为 true。
        bool close_pending{};
        GpioSafeStopper safe_stopper;
        GpioCloser closer;
    };

    struct PwmObject {
        std::uint32_t object_id{};
        std::uint64_t node_generation{};
        std::array<std::uint8_t, 16> expected_node_uuid{};
        std::uint32_t frequency_hz{};
        std::uint16_t duty{};
        bool active_low{};
        PwmStopper stopper;
    };

    struct CompletedPwmCommand {
        std::array<std::uint8_t, 16> lease_id{};
        std::string owner_key_id;
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        bool stop{};
        std::uint32_t frequency_hz{};
        std::uint16_t duty{};
        bool active_low{};
        RuntimePwmResult result;
        std::uint64_t retain_until_ns{};
    };

    struct CompletedTimedBitstreamCommand {
        std::array<std::uint8_t, 16> lease_id{};
        std::string owner_key_id;
        std::uint32_t node_id{};
        std::uint32_t resource_id{};
        std::uint8_t kind{};
        std::uint32_t bit_period_ns{};
        std::uint32_t zero_high_ns{};
        std::uint32_t one_high_ns{};
        std::uint32_t reset_time_us{};
        std::uint16_t bit_count{};
        std::vector<std::uint8_t> data;
        RuntimeTimedBitstreamResult result;
        std::uint64_t retain_until_ns{};
    };

    struct CleanupTask {
        std::uint64_t scope{};
        std::string lease_key;
        GpioObject object;
        std::optional<PwmObject> pwm_object;
        std::uint32_t resource_id{};
        std::uint64_t node_generation{};
        std::array<std::uint8_t, 16> expected_node_uuid{};
        std::uint64_t remote_lease_id{};
        PwmLeaseReleaser remote_lease_releaser;
    };

    enum class CleanupResult {
        Closed,
        SafeLowFailed,
        CloseUncertain,
    };

    static std::string binary_id(
        const std::array<std::uint8_t, 16>& value);
    static std::uint64_t scope_key(std::uint32_t node_id,
                                   std::uint32_t resource_id) noexcept;
    void erase_lease_locked(const std::string& lease_key);
    std::vector<CleanupTask> collect_expired_locked(std::uint64_t now_ns);
    CleanupResult stop_in_flight_scope(
        std::uint64_t scope, const std::string& lease_key,
        std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        std::uint32_t object_id, bool close_pending,
        const GpioSafeStopper& safe_stopper,
        const GpioCloser& closer,
        bool retain_failed);
    std::size_t finish_cleanup(std::vector<CleanupTask> tasks,
                               bool retain_failed);
    void expiry_loop();

    std::size_t capacity_;
    std::size_t history_capacity_;
    Clock monotonic_ns_;
    std::mutex mutex_;
    std::condition_variable scope_available_;
    std::unordered_map<std::string, LeaseState> leases_;
    std::unordered_map<std::uint64_t, std::string> leases_by_scope_;
    std::unordered_map<std::string, CompletedCommand> completed_;
    std::unordered_map<std::string, CompletedPwmCommand> pwm_completed_;
    std::unordered_map<std::string, CompletedTimedBitstreamCommand>
        timed_bitstream_completed_;
    std::unordered_map<std::uint64_t, GpioObject> gpio_objects_;
    std::unordered_map<std::uint64_t, PwmObject> pwm_objects_;
    std::unordered_set<std::uint64_t> in_flight_scopes_;
    std::uint64_t next_admission_id_{1U};
    std::condition_variable expiry_changed_;
    std::condition_variable shutdown_changed_;
    bool expiry_worker_enabled_{};
    bool stopping_{};
    bool shutdown_in_progress_{};
    bool shutdown_complete_{};
    std::size_t shutdown_failures_{};
    bool expiry_worker_failed_{};
    std::thread expiry_worker_;
};

}  // namespace remotebsp::toolbusd
