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

struct OperationTerminalResult {
    std::optional<std::uint32_t> object_id;
    std::optional<bool> value;
    std::uint16_t stable_error_code{};
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
    static OperationDigest derive_request_digest(
        const RuntimeGpioWriteOperation& operation);
    static OperationDigest derive_request_digest(
        const RuntimeControlReleaseOperation& operation,
        const ServerResolvedLease& lease);

    OperationBeginResult begin_gpio_write(
        const RuntimeGpioWriteOperation& operation);
    OperationBeginResult begin_release(
        const RuntimeControlReleaseOperation& operation,
        const ServerResolvedLease& server_resolved_lease);

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
