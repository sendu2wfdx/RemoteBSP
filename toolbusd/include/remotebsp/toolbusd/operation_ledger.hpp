#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace remotebsp::toolbusd {

constexpr std::size_t kOperationLedgerDigestBytes = 32U;
constexpr std::size_t kOperationLedgerIdentityBytes = 16U;
constexpr std::size_t kOperationLedgerMaximumTextBytes = 64U;
constexpr std::uint16_t kOperationLedgerMaximumStableErrorCode = 4095U;
constexpr std::uint16_t kOperationLedgerPersistenceErrorCode = 4U;
constexpr std::uint64_t kOperationLedgerDayMs = 24ULL * 60ULL * 60ULL * 1000ULL;

using OperationDigest =
    std::array<std::uint8_t, kOperationLedgerDigestBytes>;
using OperationIdentity =
    std::array<std::uint8_t, kOperationLedgerIdentityBytes>;

enum class OperationKind : std::uint8_t {
    RuntimeGpioWrite = 1U,
    RuntimeControlRelease = 2U,
    RuntimePwmConfigure = 3U,
    RuntimePwmStop = 4U,
    RuntimeTimedBitstreamConfigure = 5U,
    RuntimeTimedBitstreamFrame = 6U,
    RuntimeTimedBitstreamStop = 7U,
    RuntimeBusResourceReset = 8U,
    RuntimeMotionGroupCancel = 9U,
};

enum class OperationState : std::uint8_t {
    Pending = 1U,
    Committed = 2U,
    Rejected = 3U,
    Unknown = 4U,
};

enum class OperationRecovery : std::uint8_t {
    None = 0U,
    NotSent = 1U,
    SafeClosed = 2U,
    ScopeBlocked = 3U,
    AwaitingReboot = 4U,
    NodeRebootConfirmed = 5U,
};

enum class LedgerIoPoint : std::uint8_t {
    RecordWrite,
    RecordDataSync,
    TerminalRecordDataSync,
    SegmentCreate,
    SegmentPublish,
    ManifestWrite,
    ManifestDataSync,
    ManifestRename,
    DirectorySync,
    SegmentDelete,
};

enum class OperationLedgerError : std::uint8_t {
    InvalidRequest,
    DirectoryUnsafe,
    AlreadyLocked,
    IoFailure,
    Corrupt,
    MutationUnavailable,
    CapacityExceeded,
    IdempotencyConflict,
    InvalidTransition,
    PermissionDenied,
};

class OperationLedgerException : public std::runtime_error {
public:
    OperationLedgerException(OperationLedgerError code,
                             const std::string& message);
    OperationLedgerError code() const noexcept;

private:
    OperationLedgerError code_;
};

struct OperationScope {
    OperationIdentity expected_node_uuid{};
    std::uint32_t resource_id{};

    bool operator==(const OperationScope& other) const noexcept;
};

// 只能由服务端已经验证的租约状态构造并传给 Release；该类型不得直接由
// Runtime IPC 的客户端字段填充。
struct ServerResolvedLease {
    OperationIdentity expected_node_uuid{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint16_t permissions{};
    std::uint64_t admission_id{};
};

struct RuntimeGpioWriteOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    bool value{};
};

struct RuntimeControlReleaseOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    std::string owner_key_id;
};

struct RuntimePwmConfigureOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
};

struct RuntimePwmStopOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
};

struct RuntimeTimedBitstreamConfigureOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint32_t bit_period_ns{};
    std::uint32_t zero_high_ns{};
    std::uint32_t one_high_ns{};
    std::uint32_t reset_time_us{};
};

struct RuntimeTimedBitstreamFrameOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint16_t bit_count{};
    std::vector<std::uint8_t> data;
};

struct RuntimeTimedBitstreamStopOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
};

// 对已持有独占租约的 I2C/SPI 设备执行一次有副作用的软件恢复。
// 幂等键只用于账本身份；Pending 持久化后调用方不得自动重发。
struct RuntimeBusResourceResetOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationIdentity expected_node_uuid{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
};

// 运动组停止的账本身份不借用资源字段。事务三元组进入请求摘要，确保同一
// owner/lease/idempotency_key 回放到不同运动组时稳定冲突。
struct RuntimeMotionGroupCancelOperation {
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
};

struct OperationTerminalResult {
    OperationTerminalResult() = default;
    OperationTerminalResult(std::optional<std::uint32_t> object,
                            std::optional<bool> gpio_value,
                            std::uint16_t error_code)
        : object_id(object), value(gpio_value),
          stable_error_code(error_code) {}
    std::optional<std::uint32_t> object_id;
    std::optional<bool> value;
    std::uint16_t stable_error_code{};
    std::optional<std::uint32_t> frequency_hz;
    std::optional<std::uint16_t> duty;
    std::optional<bool> active_low;
};

struct OperationRecord {
    OperationDigest operation_id{};
    OperationDigest request_digest{};
    OperationKind kind{OperationKind::RuntimeGpioWrite};
    OperationState state{OperationState::Pending};
    OperationRecovery recovery{OperationRecovery::None};
    OperationIdentity daemon_origin{};
    OperationIdentity lease_id{};
    OperationScope scope{};
    std::string owner_key_id;
    std::string idempotency_key;
    std::uint16_t permissions{};
    std::uint32_t node_id{};
    std::uint64_t admission_id{};
    std::optional<bool> requested_value;
    std::optional<std::uint32_t> requested_frequency_hz;
    std::optional<std::uint16_t> requested_duty;
    std::optional<bool> requested_active_low;
    // v3仅保存定时位流参数或整帧正文的摘要，不持久化帧正文。
    std::optional<OperationDigest> requested_payload_digest;
    std::uint64_t motion_transaction_id{};
    std::uint32_t motion_group_id{};
    std::uint32_t motion_plan_generation{};
    OperationTerminalResult result;
    std::uint64_t sequence{};
    std::uint64_t recorded_at_ms{};
    bool full_record{true};
};

enum class OperationBeginDisposition : std::uint8_t {
    StartedDurablePending,
    ExistingPending,
    ExistingTerminal,
};

struct OperationBeginResult {
    OperationBeginDisposition disposition{
        OperationBeginDisposition::StartedDurablePending};
    OperationRecord record;
};

enum class OperationLookupDisposition : std::uint8_t {
    Found,
    ExpiredUnknown,
};

struct OperationLookupResult {
    OperationLookupDisposition disposition{
        OperationLookupDisposition::ExpiredUnknown};
    std::optional<OperationRecord> record;
};

struct OperationLedgerOptions {
    std::filesystem::path directory;
    std::size_t maximum_operations{65536U};
    std::uint64_t maximum_total_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_segment_bytes{4ULL * 1024ULL * 1024ULL};
    std::uint64_t full_terminal_retention_ms{kOperationLedgerDayMs};
    std::uint64_t tombstone_retention_ms{30ULL * kOperationLedgerDayMs};
    std::function<std::uint64_t()> wall_clock_ms;
    std::function<void(LedgerIoPoint)> fault_hook;
    // 仅用于确定性验证短写循环；0 表示使用内核允许的完整长度。
    std::size_t maximum_write_chunk{};
};

class OperationLedger {
public:
    explicit OperationLedger(OperationLedgerOptions options);
    ~OperationLedger() noexcept;

    OperationLedger(const OperationLedger&) = delete;
    OperationLedger& operator=(const OperationLedger&) = delete;
    OperationLedger(OperationLedger&&) = delete;
    OperationLedger& operator=(OperationLedger&&) = delete;

    static OperationDigest derive_operation_id(
        const RuntimeGpioWriteOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeControlReleaseOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimePwmConfigureOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimePwmStopOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeTimedBitstreamConfigureOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeTimedBitstreamFrameOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeTimedBitstreamStopOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeBusResourceResetOperation& operation);
    static OperationDigest derive_operation_id(
        const RuntimeMotionGroupCancelOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeGpioWriteOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeControlReleaseOperation& operation,
        const ServerResolvedLease& lease);
    static OperationDigest derive_request_digest(
        const RuntimePwmConfigureOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimePwmStopOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeTimedBitstreamConfigureOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeTimedBitstreamFrameOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeTimedBitstreamStopOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeBusResourceResetOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeMotionGroupCancelOperation& operation);

    OperationBeginResult begin_gpio_write(
        const RuntimeGpioWriteOperation& operation);
    OperationBeginResult begin_release(
        const RuntimeControlReleaseOperation& operation,
        const ServerResolvedLease& server_resolved_lease);
    OperationBeginResult begin_pwm_configure(
        const RuntimePwmConfigureOperation& operation);
    OperationBeginResult begin_pwm_stop(
        const RuntimePwmStopOperation& operation);
    OperationBeginResult begin_timed_bitstream_configure(
        const RuntimeTimedBitstreamConfigureOperation& operation);
    OperationBeginResult begin_timed_bitstream_frame(
        const RuntimeTimedBitstreamFrameOperation& operation);
    OperationBeginResult begin_timed_bitstream_stop(
        const RuntimeTimedBitstreamStopOperation& operation);
    OperationBeginResult begin_bus_resource_reset(
        const RuntimeBusResourceResetOperation& operation);
    OperationBeginResult begin_motion_group_cancel(
        const RuntimeMotionGroupCancelOperation& operation);

    OperationRecord finish(const OperationDigest& operation_id,
                           const OperationDigest& request_digest,
                           OperationState state,
                           OperationRecovery recovery,
                           const OperationTerminalResult& result = {});

    OperationRecord update_unknown_recovery(
        const OperationDigest& operation_id,
        const OperationDigest& request_digest,
        OperationRecovery recovery);

    OperationLookupResult lookup(const OperationDigest& operation_id,
                                 const std::string& owner_key_id) const;
    std::vector<OperationScope> blocked_scopes() const;
    bool mutation_available() const noexcept;
    std::string failure_reason() const;
    std::size_t operation_count() const;

    // 压缩只会处理已满足保留期的确定终态；墙钟回拨或未来时间戳均保守保留。
    void compact();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remotebsp::toolbusd
