#include "remotebsp/toolbusd/operation_ledger.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace remotebsp::toolbusd;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw std::runtime_error("测试检查失败: " #condition);           \
        }                                                                      \
    } while (false)

class TestDirectory {
public:
    TestDirectory() {
        std::array<char, 48> pattern{};
        const std::string source = "/tmp/remotebsp-ledger-XXXXXX";
        std::copy(source.begin(), source.end(), pattern.begin());
        auto* result = ::mkdtemp(pattern.data());
        if (result == nullptr) {
            throw std::system_error(errno, std::generic_category(),
                                    "mkdtemp失败");
        }
        path_ = result;
    }

    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

OperationIdentity identity(std::uint8_t value) {
    OperationIdentity result{};
    result.fill(value);
    return result;
}

RuntimeGpioWriteOperation gpio_operation(
    std::uint8_t lease = 0x22U, std::string idempotency = "idem",
    std::uint32_t resource = 0x01000001U, bool value = true) {
    return {
        identity(0x11U), identity(lease), identity(0x33U), "owner",
        std::move(idempotency), 1U, 7U, resource, value};
}

RuntimeControlReleaseOperation release_operation(std::uint8_t lease = 0x22U) {
    return {identity(0x11U), identity(lease), "owner"};
}

ServerResolvedLease resolved_lease(std::uint32_t resource = 0x01000001U) {
    return {identity(0x33U), 7U, resource, 1U, 1U};
}

OperationTerminalResult gpio_committed_result(bool value = true) {
    OperationTerminalResult result;
    result.object_id = 9U;
    result.value = value;
    return result;
}

std::string hex(const OperationDigest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

template <typename Callback>
void check_error(OperationLedgerError expected, Callback&& callback) {
    try {
        callback();
    } catch (const OperationLedgerException& error) {
        CHECK(error.code() == expected);
        return;
    }
    throw std::runtime_error("预期OperationLedgerException但未抛出");
}

OperationLedgerOptions options(const TestDirectory& directory,
                               std::uint64_t* now = nullptr) {
    OperationLedgerOptions result;
    result.directory = directory.path();
    if (now != nullptr) {
        result.wall_clock_ms = [now] { return *now; };
    }
    return result;
}

void check_stable_ids_and_business_conflict() {
    TestDirectory directory;
    auto first = gpio_operation();
    CHECK(hex(OperationLedger::derive_operation_id(first)) ==
          "42cb33195d34911bf44f318d0862c8b2a91d0093a5503190e330fddbe3b2df8d");

    auto changed_value = first;
    changed_value.value = false;
    CHECK(OperationLedger::derive_operation_id(first) ==
          OperationLedger::derive_operation_id(changed_value));
    CHECK(hex(OperationLedger::derive_request_digest(first)) ==
          "3e66708e0d23db0a03a03f6ca1fd028c51f33fb6a3635485bc2cee79b912749c");
    CHECK(OperationLedger::derive_request_digest(first) !=
          OperationLedger::derive_request_digest(changed_value));

    OperationLedger ledger(options(directory));
    const auto begun = ledger.begin_gpio_write(first);
    CHECK(begun.disposition ==
          OperationBeginDisposition::StartedDurablePending);
    const auto replay = ledger.begin_gpio_write(first);
    CHECK(replay.disposition == OperationBeginDisposition::ExistingPending);

    auto reused_across_lease = first;
    reused_across_lease.lease_id = identity(0x44U);
    check_error(OperationLedgerError::IdempotencyConflict, [&] {
        (void)ledger.begin_gpio_write(reused_across_lease);
    });

    const auto release = release_operation();
    auto other_scope = resolved_lease(0x01000002U);
    CHECK(OperationLedger::derive_operation_id(release) ==
          OperationLedger::derive_operation_id(release));
    CHECK(hex(OperationLedger::derive_operation_id(release)) ==
          "d4e765fa7b2a3743b81a6bdcf587bd850169dd09466fb462958d046ae66b7055");
    CHECK(hex(OperationLedger::derive_request_digest(
              release, resolved_lease())) ==
          "22f1fbfd47d4f76f6b93c5ad47a49e4e8157717dae6443e15421d1d7a1467519");
    CHECK(OperationLedger::derive_request_digest(release, resolved_lease()) !=
          OperationLedger::derive_request_digest(release, other_scope));
    auto next_admission = resolved_lease();
    next_admission.admission_id += 1U;
    CHECK(OperationLedger::derive_request_digest(release, resolved_lease()) !=
          OperationLedger::derive_request_digest(release, next_admission));
}

void check_process_lock_and_durable_terminal() {
    TestDirectory directory;
    const auto operation = gpio_operation();
    OperationDigest operation_id{};
    {
        OperationLedger ledger(options(directory));
        check_error(OperationLedgerError::AlreadyLocked, [&] {
            OperationLedger duplicate(options(directory));
        });
        auto begun = ledger.begin_gpio_write(operation);
        operation_id = begun.record.operation_id;
        CHECK(begun.record.recovery == OperationRecovery::None);
        const auto result = gpio_committed_result();
        const auto committed = ledger.finish(
            begun.record.operation_id, begun.record.request_digest,
            OperationState::Committed, OperationRecovery::None, result);
        CHECK(committed.state == OperationState::Committed);
    }
    OperationLedger reopened(options(directory));
    const auto found = reopened.lookup(operation_id, "owner");
    CHECK(found.disposition == OperationLookupDisposition::Found);
    CHECK(found.record->state == OperationState::Committed);
    CHECK(found.record->result.object_id == 9U);
    CHECK(reopened.blocked_scopes().size() == 1U);
    const auto hidden = reopened.lookup(operation_id, "another-owner");
    CHECK(hidden.disposition == OperationLookupDisposition::ExpiredUnknown);
    CHECK(!hidden.record.has_value());
}

void check_restart_pending_becomes_unknown() {
    TestDirectory directory;
    OperationDigest operation_id{};
    {
        OperationLedger ledger(options(directory));
        operation_id = ledger.begin_gpio_write(gpio_operation()).record.operation_id;
    }
    {
        OperationLedger recovered(options(directory));
        CHECK(recovered.mutation_available());
        const auto found = recovered.lookup(operation_id, "owner");
        CHECK(found.record->state == OperationState::Unknown);
        CHECK(found.record->recovery == OperationRecovery::ScopeBlocked);
        CHECK(found.record->result.stable_error_code ==
              kOperationLedgerPersistenceErrorCode);
        CHECK(recovered.blocked_scopes().size() == 1U);
    }
    OperationLedger stable(options(directory));
    CHECK(stable.lookup(operation_id, "owner").record->state ==
          OperationState::Unknown);
    CHECK(stable.lookup(operation_id, "owner")
              .record->result.stable_error_code ==
          kOperationLedgerPersistenceErrorCode);
}

void check_terminal_matrix_and_unknown_recovery_updates() {
    TestDirectory directory;
    OperationDigest operation_id{};
    OperationDigest request_digest{};
    {
        OperationLedger ledger(options(directory));
        const auto pending = ledger.begin_gpio_write(gpio_operation()).record;
        operation_id = pending.operation_id;
        request_digest = pending.request_digest;
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(operation_id, request_digest,
                                OperationState::Committed,
                                OperationRecovery::SafeClosed,
                                gpio_committed_result());
        });
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(operation_id, request_digest,
                                OperationState::Committed,
                                OperationRecovery::None);
        });
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(operation_id, request_digest,
                                OperationState::Rejected,
                                OperationRecovery::None,
                                OperationTerminalResult{std::nullopt,
                                                        std::nullopt, 2U});
        });
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(operation_id, request_digest,
                                OperationState::Unknown,
                                OperationRecovery::NotSent,
                                OperationTerminalResult{std::nullopt,
                                                        std::nullopt, 2U});
        });
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(operation_id, request_digest,
                                OperationState::Unknown,
                                OperationRecovery::ScopeBlocked);
        });
        const auto unknown = ledger.finish(
            operation_id, request_digest, OperationState::Unknown,
            OperationRecovery::ScopeBlocked,
            OperationTerminalResult{std::nullopt, std::nullopt, 2U});
        CHECK(unknown.recovery == OperationRecovery::ScopeBlocked);
        CHECK(ledger.update_unknown_recovery(
                  operation_id, request_digest,
                  OperationRecovery::AwaitingReboot).recovery ==
              OperationRecovery::AwaitingReboot);
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.update_unknown_recovery(
                operation_id, request_digest,
                OperationRecovery::ScopeBlocked);
        });
        CHECK(ledger.update_unknown_recovery(
                  operation_id, request_digest,
                  OperationRecovery::SafeClosed).recovery ==
              OperationRecovery::SafeClosed);
        CHECK(ledger.blocked_scopes().empty());
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.update_unknown_recovery(
                operation_id, request_digest,
                OperationRecovery::NodeRebootConfirmed);
        });
    }
    OperationLedger recovered(options(directory));
    const auto found = recovered.lookup(operation_id, "owner");
    CHECK(found.record->state == OperationState::Unknown);
    CHECK(found.record->recovery == OperationRecovery::SafeClosed);
    CHECK(recovered.blocked_scopes().empty());

    TestDirectory release_directory;
    OperationLedger release_ledger(options(release_directory));
    const auto release = release_ledger.begin_release(
        release_operation(), resolved_lease()).record;
    check_error(OperationLedgerError::InvalidTransition, [&] {
        (void)release_ledger.finish(
            release.operation_id, release.request_digest,
            OperationState::Committed, OperationRecovery::None);
    });
    check_error(OperationLedgerError::InvalidTransition, [&] {
        (void)release_ledger.finish(
            release.operation_id, release.request_digest,
            OperationState::Committed, OperationRecovery::SafeClosed,
            gpio_committed_result());
    });
    CHECK(release_ledger.finish(
              release.operation_id, release.request_digest,
              OperationState::Committed,
              OperationRecovery::SafeClosed).recovery ==
          OperationRecovery::SafeClosed);
}

void check_concurrent_begin_is_single_durable_operation() {
    TestDirectory directory;
    OperationLedger ledger(options(directory));
    const auto operation = gpio_operation();
    std::atomic<unsigned> started{0U};
    std::atomic<unsigned> existing{0U};
    std::vector<std::thread> workers;
    for (unsigned i = 0U; i < 32U; ++i) {
        workers.emplace_back([&] {
            const auto result = ledger.begin_gpio_write(operation);
            if (result.disposition ==
                OperationBeginDisposition::StartedDurablePending) {
                ++started;
            } else {
                ++existing;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(started == 1U);
    CHECK(existing == 31U);
    CHECK(ledger.operation_count() == 1U);
}

void check_release_unblocks_and_terminal_records_expire() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    OperationDigest write_id{};
    OperationDigest release_id{};
    {
        OperationLedger ledger(config);
        const auto write = ledger.begin_gpio_write(gpio_operation()).record;
        write_id = write.operation_id;
        (void)ledger.finish(write.operation_id, write.request_digest,
                            OperationState::Committed,
                            OperationRecovery::None,
                            gpio_committed_result());
        const auto release = ledger.begin_release(
            release_operation(), resolved_lease()).record;
        release_id = release.operation_id;
        (void)ledger.finish(release.operation_id, release.request_digest,
                            OperationState::Committed,
                            OperationRecovery::SafeClosed);
        CHECK(ledger.blocked_scopes().empty());
        now += kOperationLedgerDayMs;
        ledger.compact();
        CHECK(ledger.lookup(write_id, "owner").disposition ==
              OperationLookupDisposition::ExpiredUnknown);
        now += 30U * kOperationLedgerDayMs;
        ledger.compact();
        CHECK(ledger.operation_count() == 0U);
        CHECK(ledger.lookup(release_id, "owner").disposition ==
              OperationLookupDisposition::ExpiredUnknown);
    }
    OperationLedger reopened(config);
    CHECK(reopened.mutation_available());
    CHECK(reopened.blocked_scopes().empty());
}

void check_orphan_write_never_expires() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    OperationDigest operation_id{};
    {
        OperationLedger ledger(config);
        const auto begun = ledger.begin_gpio_write(gpio_operation()).record;
        operation_id = begun.operation_id;
        (void)ledger.finish(begun.operation_id, begun.request_digest,
                            OperationState::Committed,
                            OperationRecovery::None,
                            gpio_committed_result());
        now += 31U * kOperationLedgerDayMs;
        ledger.compact();
        CHECK(ledger.lookup(operation_id, "owner").disposition ==
              OperationLookupDisposition::Found);
        CHECK(ledger.blocked_scopes().size() == 1U);
    }
    OperationLedger reopened(config);
    CHECK(reopened.lookup(operation_id, "owner").disposition ==
          OperationLookupDisposition::Found);
    CHECK(reopened.blocked_scopes().size() == 1U);
}

void check_only_latest_scope_anchor_is_unresolved() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    OperationLedger ledger(config);
    const auto first = ledger.begin_gpio_write(
        gpio_operation(0x22U, "write-1")).record;
    (void)ledger.finish(first.operation_id, first.request_digest,
                        OperationState::Committed, OperationRecovery::None,
                        gpio_committed_result());
    const auto second = ledger.begin_gpio_write(
        gpio_operation(0x22U, "write-2", 0x01000001U, false)).record;
    (void)ledger.finish(second.operation_id, second.request_digest,
                        OperationState::Committed, OperationRecovery::None,
                        gpio_committed_result(false));
    now += kOperationLedgerDayMs;
    ledger.compact();
    CHECK(ledger.lookup(first.operation_id, "owner").disposition ==
          OperationLookupDisposition::ExpiredUnknown);
    CHECK(ledger.lookup(second.operation_id, "owner").disposition ==
          OperationLookupDisposition::Found);
    CHECK(ledger.blocked_scopes().size() == 1U);
    now += 30U * kOperationLedgerDayMs;
    ledger.compact();
    CHECK(ledger.operation_count() == 1U);
    CHECK(ledger.lookup(second.operation_id, "owner").disposition ==
          OperationLookupDisposition::Found);
}

void check_rejected_release_preserves_prior_write_anchor() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    OperationDigest write_id{};
    {
        OperationLedger ledger(config);
        const auto write = ledger.begin_gpio_write(gpio_operation()).record;
        write_id = write.operation_id;
        (void)ledger.finish(write.operation_id, write.request_digest,
                            OperationState::Committed,
                            OperationRecovery::None,
                            gpio_committed_result());
        const auto release = ledger.begin_release(
            release_operation(), resolved_lease()).record;
        (void)ledger.finish(
            release.operation_id, release.request_digest,
            OperationState::Rejected, OperationRecovery::NotSent,
            OperationTerminalResult{std::nullopt, std::nullopt, 1U});
        now += 31U * kOperationLedgerDayMs;
        ledger.compact();
        CHECK(ledger.lookup(write_id, "owner").disposition ==
              OperationLookupDisposition::Found);
        CHECK(ledger.blocked_scopes().size() == 1U);
    }
    OperationLedger reopened(config);
    CHECK(reopened.lookup(write_id, "owner").disposition ==
          OperationLookupDisposition::Found);
    CHECK(reopened.blocked_scopes().size() == 1U);
}

void check_tombstone_gets_its_own_retention_horizon() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    OperationLedger ledger(config);
    const auto release = ledger.begin_release(
        release_operation(), resolved_lease()).record;
    (void)ledger.finish(release.operation_id, release.request_digest,
                        OperationState::Committed,
                        OperationRecovery::SafeClosed);
    now += 31U * kOperationLedgerDayMs;
    ledger.compact();
    CHECK(ledger.operation_count() == 1U);
    CHECK(ledger.lookup(release.operation_id, "owner").disposition ==
          OperationLookupDisposition::ExpiredUnknown);
    CHECK(ledger.lookup(release.operation_id, "another-owner").disposition ==
          OperationLookupDisposition::ExpiredUnknown);
    OperationDigest never_seen{};
    never_seen.fill(0xa5U);
    CHECK(ledger.lookup(never_seen, "another-owner").disposition ==
          OperationLookupDisposition::ExpiredUnknown);
    ledger.compact();
    CHECK(ledger.operation_count() == 1U);
    now += 30U * kOperationLedgerDayMs;
    ledger.compact();
    CHECK(ledger.operation_count() == 0U);
}

void check_wall_clock_rollback_never_shortens_retention() {
    TestDirectory directory;
    std::uint64_t now = 10U * kOperationLedgerDayMs;
    auto config = options(directory, &now);
    OperationLedger ledger(config);
    const auto release = ledger.begin_release(
        release_operation(), resolved_lease()).record;
    (void)ledger.finish(release.operation_id, release.request_digest,
                        OperationState::Committed,
                        OperationRecovery::SafeClosed);
    now = 1U;
    ledger.compact();
    CHECK(ledger.lookup(release.operation_id, "owner").disposition ==
          OperationLookupDisposition::Found);
}

void check_compaction_spans_multiple_segments() {
    TestDirectory directory;
    std::uint64_t now = 1U;
    auto config = options(directory, &now);
    config.maximum_segment_bytes = 1024U;
    {
        OperationLedger ledger(config);
        for (std::uint8_t index = 1U; index <= 10U; ++index) {
            const auto pending = ledger.begin_gpio_write(gpio_operation(
                static_cast<std::uint8_t>(0x30U + index),
                "unknown-" + std::to_string(index),
                0x01000000U + index)).record;
            (void)ledger.finish(
                pending.operation_id, pending.request_digest,
                OperationState::Unknown, OperationRecovery::ScopeBlocked,
                OperationTerminalResult{std::nullopt, std::nullopt, 2U});
        }
        const auto release = ledger.begin_release(
            release_operation(0x70U),
            resolved_lease(0x01000070U)).record;
        (void)ledger.finish(release.operation_id, release.request_digest,
                            OperationState::Committed,
                            OperationRecovery::SafeClosed);
        now += kOperationLedgerDayMs;
        ledger.compact();
        CHECK(ledger.operation_count() == 11U);
    }
    OperationLedger reopened(config);
    CHECK(reopened.mutation_available());
    CHECK(reopened.operation_count() == 11U);
    CHECK(reopened.blocked_scopes().size() == 10U);
}

void check_capacity_keeps_unresolved() {
    TestDirectory directory;
    auto config = options(directory);
    config.maximum_operations = 1U;
    OperationLedger ledger(config);
    (void)ledger.begin_gpio_write(gpio_operation());
    check_error(OperationLedgerError::CapacityExceeded, [&] {
        (void)ledger.begin_gpio_write(
            gpio_operation(0x44U, "different-business-key", 0x01000002U));
    });
    CHECK(ledger.operation_count() == 1U);
}

void check_short_write_and_eintr_retry() {
    TestDirectory directory;
    auto config = options(directory);
    config.maximum_write_chunk = 3U;
    auto interrupted = std::make_shared<std::atomic<bool>>(false);
    config.fault_hook = [interrupted](LedgerIoPoint point) {
        if (point == LedgerIoPoint::RecordWrite && !interrupted->exchange(true)) {
            throw std::system_error(EINTR, std::generic_category());
        }
    };
    OperationLedger ledger(config);
    CHECK(ledger.begin_gpio_write(gpio_operation()).disposition ==
          OperationBeginDisposition::StartedDurablePending);
    CHECK(*interrupted);
}

void check_terminal_sync_failure_recovers_as_unknown() {
    TestDirectory directory;
    auto fail_sync = std::make_shared<std::atomic<bool>>(false);
    auto config = options(directory);
    config.fault_hook = [fail_sync](LedgerIoPoint point) {
        if (fail_sync->load() && point == LedgerIoPoint::RecordDataSync) {
            throw std::system_error(ENOSPC, std::generic_category());
        }
    };
    OperationDigest operation_id{};
    OperationDigest request_digest{};
    {
        OperationLedger ledger(config);
        const auto pending = ledger.begin_gpio_write(gpio_operation()).record;
        operation_id = pending.operation_id;
        request_digest = pending.request_digest;
        fail_sync->store(true);
        check_error(OperationLedgerError::IoFailure, [&] {
            (void)ledger.finish(
                operation_id, request_digest, OperationState::Committed,
                OperationRecovery::None, gpio_committed_result());
        });
        CHECK(!ledger.mutation_available());
        check_error(OperationLedgerError::MutationUnavailable, [&] {
            (void)ledger.lookup(operation_id, "owner");
        });
    }
    fail_sync->store(false);
    OperationLedger recovered(config);
    CHECK(recovered.mutation_available());
    const auto found = recovered.lookup(operation_id, "owner");
    CHECK(found.record->state == OperationState::Unknown);
    CHECK(found.record->recovery == OperationRecovery::ScopeBlocked);
    CHECK(found.record->result.stable_error_code ==
          kOperationLedgerPersistenceErrorCode);
}

void check_manifest_and_directory_sync_fail_closed() {
    for (const auto failed_point : {
             LedgerIoPoint::ManifestDataSync,
             LedgerIoPoint::DirectorySync}) {
        TestDirectory directory;
        auto enabled = std::make_shared<std::atomic<bool>>(false);
        auto config = options(directory);
        config.fault_hook = [enabled, failed_point](LedgerIoPoint point) {
            if (enabled->load() && point == failed_point) {
                throw std::system_error(EIO, std::generic_category());
            }
        };
        OperationLedger ledger(config);
        enabled->store(true);
        check_error(OperationLedgerError::IoFailure, [&] {
            (void)ledger.begin_gpio_write(gpio_operation());
        });
        CHECK(!ledger.mutation_available());
        check_error(OperationLedgerError::MutationUnavailable, [&] {
            (void)ledger.lookup(
                OperationLedger::derive_operation_id(gpio_operation()),
                "owner");
        });
    }
}

void check_uncommitted_tail_is_truncated_to_manifest() {
    TestDirectory directory;
    OperationDigest operation_id{};
    const auto segment =
        directory.path() / "segment-0000000000000001.rbol";
    std::uintmax_t durable_size{};
    {
        OperationLedger ledger(options(directory));
        const auto pending = ledger.begin_gpio_write(gpio_operation()).record;
        operation_id = pending.operation_id;
        durable_size = std::filesystem::file_size(segment);
    }
    {
        std::ofstream tail(segment, std::ios::binary | std::ios::app);
        tail << "uncommitted-tail";
    }
    CHECK(std::filesystem::file_size(segment) > durable_size);
    OperationLedger recovered(options(directory));
    CHECK(recovered.mutation_available());
    CHECK(std::filesystem::file_size(segment) >= durable_size);
    const auto found = recovered.lookup(operation_id, "owner");
    CHECK(found.record->state == OperationState::Unknown);
}

void check_unreferenced_segment_is_cleaned_or_fails_closed() {
    {
        TestDirectory directory;
        {
            OperationLedger ledger(options(directory));
        }
        const auto stale =
            directory.path() / "segment-0000000000000002.rbol";
        {
            std::ofstream output(stale, std::ios::binary);
            output << "orphan";
        }
        CHECK(::chmod(stale.c_str(), 0600) == 0);
        OperationLedger recovered(options(directory));
        CHECK(recovered.mutation_available());
        CHECK(!std::filesystem::exists(stale));
    }
    {
        TestDirectory directory;
        {
            OperationLedger ledger(options(directory));
        }
        const auto stale =
            directory.path() / "segment-0000000000000002.rbol";
        {
            std::ofstream output(stale, std::ios::binary);
            output << "orphan";
        }
        CHECK(::chmod(stale.c_str(), 0600) == 0);
        auto config = options(directory);
        config.fault_hook = [](LedgerIoPoint point) {
            if (point == LedgerIoPoint::SegmentDelete) {
                throw std::system_error(EIO, std::generic_category());
            }
        };
        OperationLedger failed(config);
        CHECK(!failed.mutation_available());
    }
}

void check_missing_manifest_never_silently_reinitializes() {
    TestDirectory directory;
    {
        OperationLedger ledger(options(directory));
        (void)ledger.begin_gpio_write(gpio_operation());
    }
    const auto first =
        directory.path() / "segment-0000000000000001.rbol";
    const auto second =
        directory.path() / "segment-0000000000000002.rbol";
    std::filesystem::rename(first, second);
    CHECK(std::filesystem::remove(directory.path() / "manifest.v1"));
    OperationLedger failed(options(directory));
    CHECK(!failed.mutation_available());
    CHECK(std::filesystem::exists(second));
    CHECK(!std::filesystem::exists(first));
}

void check_unsafe_permissions_and_symlink_are_rejected() {
    {
        TestDirectory directory;
        {
            OperationLedger ledger(options(directory));
        }
        const auto segment =
            directory.path() / "segment-0000000000000001.rbol";
        CHECK(::chmod(segment.c_str(), 0644) == 0);
        OperationLedger failed(options(directory));
        CHECK(!failed.mutation_available());
    }
    {
        TestDirectory parent;
        const auto real = parent.path() / "real";
        const auto link = parent.path() / "link";
        CHECK(::mkdir(real.c_str(), 0700) == 0);
        CHECK(::symlink(real.c_str(), link.c_str()) == 0);
        OperationLedgerOptions config;
        config.directory = link;
        check_error(OperationLedgerError::DirectoryUnsafe, [&] {
            OperationLedger ledger(config);
        });
    }
}

void check_recovery_sync_failure_hides_partial_index() {
    TestDirectory directory;
    OperationDigest operation_id{};
    {
        OperationLedger ledger(options(directory));
        operation_id = ledger.begin_gpio_write(gpio_operation()).record.operation_id;
    }
    auto config = options(directory);
    config.fault_hook = [](LedgerIoPoint point) {
        if (point == LedgerIoPoint::RecordDataSync) {
            throw std::system_error(EIO, std::generic_category());
        }
    };
    OperationLedger failed(config);
    CHECK(!failed.mutation_available());
    check_error(OperationLedgerError::MutationUnavailable, [&] {
        (void)failed.lookup(operation_id, "owner");
    });
    check_error(OperationLedgerError::MutationUnavailable, [&] {
        (void)failed.blocked_scopes();
    });
}

void check_mid_log_corruption_fails_closed() {
    TestDirectory directory;
    OperationDigest operation_id{};
    {
        OperationLedger ledger(options(directory));
        const auto begun = ledger.begin_gpio_write(gpio_operation()).record;
        operation_id = begun.operation_id;
        (void)ledger.finish(begun.operation_id, begun.request_digest,
                            OperationState::Committed,
                            OperationRecovery::None,
                            gpio_committed_result());
    }
    const auto segment = directory.path() / "segment-0000000000000001.rbol";
    std::fstream file(segment, std::ios::in | std::ios::out | std::ios::binary);
    CHECK(file.good());
    file.seekg(150);
    char byte{};
    file.read(&byte, 1);
    byte ^= 0x01;
    file.seekp(150);
    file.write(&byte, 1);
    file.close();

    OperationLedger corrupt(options(directory));
    CHECK(!corrupt.mutation_available());
    check_error(OperationLedgerError::MutationUnavailable, [&] {
        (void)corrupt.lookup(operation_id, "owner");
    });
}

}  // namespace

int main() {
    check_stable_ids_and_business_conflict();
    check_process_lock_and_durable_terminal();
    check_restart_pending_becomes_unknown();
    check_terminal_matrix_and_unknown_recovery_updates();
    check_concurrent_begin_is_single_durable_operation();
    check_release_unblocks_and_terminal_records_expire();
    check_orphan_write_never_expires();
    check_only_latest_scope_anchor_is_unresolved();
    check_rejected_release_preserves_prior_write_anchor();
    check_tombstone_gets_its_own_retention_horizon();
    check_wall_clock_rollback_never_shortens_retention();
    check_compaction_spans_multiple_segments();
    check_capacity_keeps_unresolved();
    check_short_write_and_eintr_retry();
    check_terminal_sync_failure_recovers_as_unknown();
    check_manifest_and_directory_sync_fail_closed();
    check_uncommitted_tail_is_truncated_to_manifest();
    check_unreferenced_segment_is_cleaned_or_fails_closed();
    check_missing_manifest_never_silently_reinitializes();
    check_unsafe_permissions_and_symlink_are_rejected();
    check_recovery_sync_failure_hides_partial_index();
    check_mid_log_corruption_fails_closed();
    return 0;
}
