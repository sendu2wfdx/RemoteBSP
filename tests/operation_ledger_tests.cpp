#include "remotebsp/toolbusd/operation_ledger.hpp"
#include "remotebsp/protocol/crc32.hpp"

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

RuntimePwmConfigureOperation pwm_operation(std::uint32_t frequency = 20000U,
                                           std::uint16_t duty = 4200U,
                                           bool active_low = false) {
    return {identity(0x11U), identity(0x44U), identity(0x33U), "owner",
            "pwm-idem", 2U, 7U, 0x06000000U, frequency, duty, active_low};
}

RuntimePwmStopOperation pwm_stop_operation(
    std::uint8_t lease = 0x55U,
    std::string idempotency = "pwm-stop-idem") {
    return {identity(0x11U), identity(lease), identity(0x33U), "owner",
            std::move(idempotency), 2U, 7U, 0x06000000U};
}

RuntimeBusResourceResetOperation bus_reset_operation(
    std::uint32_t resource = 0x0A000001U,
    std::string idempotency = "bus-reset-idem") {
    return {identity(0x11U), identity(0x66U), identity(0x33U), "owner",
            std::move(idempotency), 2U, 7U, resource};
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

std::vector<std::uint8_t> decode_hex(const std::string& text) {
    CHECK((text.size() % 2U) == 0U);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2U);
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10U;
        throw std::runtime_error("黄金样例包含非法十六进制字符");
    };
    for (std::size_t i = 0; i < text.size(); i += 2U) {
        bytes.push_back(static_cast<std::uint8_t>(
            (nibble(text[i]) << 4U) | nibble(text[i + 1U])));
    }
    return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    CHECK(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
    output.close();
    CHECK(::chmod(path.c_str(), 0600) == 0);
}

void put_u32_at(std::vector<std::uint8_t>& bytes, std::size_t offset,
                std::uint32_t value) {
    for (std::size_t i = 0; i < 4U; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
    }
}

// 已发布 v1 布局的真实磁盘字节：recorded_at_ms=123 的 GPIO pending。
// 固定黄金样例不借用当前 v2 编码器，避免编码器与解码器同步出错时漏检。
const std::vector<std::uint8_t>& v1_pending_record_golden() {
    static const auto bytes = decode_hex(
        "52424f504c4731000100010100008800ef0001000000000000007b00000000000000"
        "42cb33195d34911bf44f318d0862c8b2a91d0093a5503190e330fddbe3b2df8d"
        "3e66708e0d23db0a03a03f6ca1fd028c51f33fb6a3635485bc2cee79b912749c"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "5b000000000001010100070000000100000111111111111111111111111111111111"
        "2222222222222222222222222222222233333333333333333333333333333333"
        "01000000000000000000000000000000000005006f776e657204006964656d"
        "fa5f0c0152424f4c434f4d31");
    return bytes;
}

const std::vector<std::uint8_t>& v1_pending_manifest_golden() {
    static const auto bytes = decode_hex(
        "52424f4c4d463100010098000200000000000000010000000000000001000000"
        "00000000ef0000000000000001000000000000000100000000000000ef000000"
        "0000000001000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000008c65061a1b44e09c62f5bdec73859bb19fae09cf"
        "8f8d330aa073f2b199d1f5090ee4e14c52424f4c434f4d31");
    return bytes;
}

void install_v1_pending_golden(const TestDirectory& directory) {
    write_bytes(directory.path() / "segment-0000000000000001.rbol",
                v1_pending_record_golden());
    write_bytes(directory.path() / "manifest.v1", v1_pending_manifest_golden());
}

void check_v1_golden_restarts_and_upgrades_with_v2_recovery() {
    TestDirectory directory;
    install_v1_pending_golden(directory);
    const auto operation_id = OperationLedger::derive_operation_id(gpio_operation());
    {
        OperationLedger ledger(options(directory));
        if (!ledger.mutation_available()) {
            throw std::runtime_error("v1黄金样例加载失败: " +
                                     ledger.failure_reason());
        }
        const auto recovered = ledger.lookup(operation_id, "owner");
        CHECK(recovered.disposition == OperationLookupDisposition::Found);
        CHECK(recovered.record->state == OperationState::Unknown);
        CHECK(recovered.record->recovery == OperationRecovery::ScopeBlocked);
        CHECK(recovered.record->requested_value == true);
        CHECK(ledger.blocked_scopes().size() == 1U);
    }
    const auto segment = directory.path() / "segment-0000000000000001.rbol";
    const auto size_after_recovery = std::filesystem::file_size(segment);
    OperationLedger reopened(options(directory));
    CHECK(reopened.mutation_available());
    CHECK(reopened.operation_count() == 1U);
    CHECK(std::filesystem::file_size(segment) == size_after_recovery);
    CHECK(reopened.lookup(operation_id, "owner").record->state ==
          OperationState::Unknown);
}

void check_unknown_disk_versions_fail_closed() {
    TestDirectory directory;
    install_v1_pending_golden(directory);
    auto manifest = v1_pending_manifest_golden();
    manifest[8U] = 3U;
    manifest[9U] = 0U;
    put_u32_at(manifest, manifest.size() - 12U,
               remotebsp::protocol::crc32(manifest.data(), manifest.size() - 12U));
    write_bytes(directory.path() / "manifest.v1", manifest);
    OperationLedger failed(options(directory));
    CHECK(!failed.mutation_available());
    check_error(OperationLedgerError::MutationUnavailable, [&] {
        (void)failed.lookup(OperationLedger::derive_operation_id(gpio_operation()),
                            "owner");
    });
}

void check_pwm_record_is_lossless_and_durable() {
    TestDirectory directory;
    OperationDigest id{};
    {
        OperationLedger ledger(options(directory));
        const auto first = ledger.begin_pwm_configure(pwm_operation());
        CHECK(first.record.kind == OperationKind::RuntimePwmConfigure);
        CHECK(first.record.requested_frequency_hz == 20000U);
        CHECK(first.record.requested_duty == 4200U);
        CHECK(first.record.requested_active_low == false);
        auto changed = pwm_operation(25000U, 4200U, false);
        check_error(OperationLedgerError::IdempotencyConflict, [&] {
            (void)ledger.begin_pwm_configure(changed);
        });
        OperationTerminalResult result;
        result.object_id = 19U;
        result.frequency_hz = 20000U;
        result.duty = 4200U;
        result.active_low = false;
        (void)ledger.finish(first.record.operation_id,
                            first.record.request_digest,
                            OperationState::Committed,
                            OperationRecovery::None, result);
        id = first.record.operation_id;
    }
    OperationLedger reopened(options(directory));
    const auto found = reopened.lookup(id, "owner");
    CHECK(found.disposition == OperationLookupDisposition::Found);
    CHECK(found.record->result.object_id == 19U);
    CHECK(found.record->result.frequency_hz == 20000U);
    CHECK(found.record->result.duty == 4200U);
    CHECK(found.record->result.active_low == false);

    TestDirectory pending_directory;
    OperationDigest pending_id{};
    {
        OperationLedger ledger(options(pending_directory));
        pending_id = ledger.begin_pwm_configure(
            pwm_operation(1000U, 0U, true)).record.operation_id;
    }
    OperationLedger recovered(options(pending_directory));
    const auto pending = recovered.lookup(pending_id, "owner");
    CHECK(pending.record->state == OperationState::Unknown);
    CHECK(pending.record->requested_active_low == true);
    CHECK(!pending.record->result.active_low.has_value());
}

void check_pwm_stop_ids_terminal_and_restart_recovery() {
    static_assert(static_cast<std::uint8_t>(OperationKind::RuntimePwmStop) == 4U,
                  "PWM停止磁盘编号不得漂移");
    const auto operation = pwm_stop_operation();
    CHECK(hex(OperationLedger::derive_operation_id(operation)) ==
          "34ca3f5d0430c70af9c60d759f37cd88b26d20b1c2650d45ccd137a42cd3cafa");
    CHECK(hex(OperationLedger::derive_request_digest(operation)) ==
          "14399ef77506b923686100ff048e5caee411684d577b8e408d7827e2b4577903");

    TestDirectory terminal_directory;
    OperationDigest terminal_id{};
    {
        OperationLedger ledger(options(terminal_directory));
        const auto begun = ledger.begin_pwm_stop(operation);
        CHECK(begun.record.kind == OperationKind::RuntimePwmStop);
        terminal_id = begun.record.operation_id;
        CHECK(ledger.begin_pwm_stop(operation).disposition ==
              OperationBeginDisposition::ExistingPending);
        auto conflicting = operation;
        conflicting.resource_id += 1U;
        check_error(OperationLedgerError::IdempotencyConflict, [&] {
            (void)ledger.begin_pwm_stop(conflicting);
        });
        OperationTerminalResult result;
        result.object_id = 23U;
        check_error(OperationLedgerError::InvalidTransition, [&] {
            (void)ledger.finish(begun.record.operation_id,
                                begun.record.request_digest,
                                OperationState::Committed,
                                OperationRecovery::None, result);
        });
        (void)ledger.finish(begun.record.operation_id,
                            begun.record.request_digest,
                            OperationState::Committed,
                            OperationRecovery::SafeClosed, result);
    }
    OperationLedger terminal_reopened(options(terminal_directory));
    const auto terminal = terminal_reopened.lookup(terminal_id, "owner");
    CHECK(terminal.record->state == OperationState::Committed);
    CHECK(terminal.record->recovery == OperationRecovery::SafeClosed);
    CHECK(terminal.record->kind == OperationKind::RuntimePwmStop);
    CHECK(terminal.record->result.object_id == 23U);
    CHECK(!terminal.record->result.frequency_hz.has_value());

    TestDirectory pending_directory;
    OperationDigest pending_id{};
    {
        OperationLedger ledger(options(pending_directory));
        pending_id = ledger.begin_pwm_stop(pwm_stop_operation(0x66U)).record.operation_id;
    }
    OperationLedger recovered(options(pending_directory));
    const auto pending = recovered.lookup(pending_id, "owner");
    CHECK(pending.record->state == OperationState::Unknown);
    CHECK(pending.record->recovery == OperationRecovery::ScopeBlocked);
    CHECK(pending.record->kind == OperationKind::RuntimePwmStop);
    CHECK(recovered.blocked_scopes().size() == 1U);
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

void check_timed_bitstream_v3_digest_only_and_recovery() {
    TestDirectory directory;
    const RuntimeTimedBitstreamFrameOperation frame{
        identity(0x11U), identity(0x71U), identity(0x33U), "owner",
        "frame-idem", 2U, 7U, 0x0A000000U, 16176U,
        std::vector<std::uint8_t>(2022U, 0xA5U)};
    OperationDigest operation_id{};
    {
        OperationLedger ledger(options(directory));
        const auto begun = ledger.begin_timed_bitstream_frame(frame);
        operation_id = begun.record.operation_id;
        CHECK(begun.record.requested_payload_digest.has_value());
        CHECK(!begun.record.requested_value.has_value());
        CHECK(std::filesystem::file_size(
                  directory.path() / "segment-0000000000000001.rbol") < 1024U);
        auto changed = frame;
        changed.data.back() ^= 1U;
        CHECK(OperationLedger::derive_operation_id(changed) == operation_id);
        CHECK(OperationLedger::derive_request_digest(changed) !=
              begun.record.request_digest);
        check_error(OperationLedgerError::IdempotencyConflict, [&] {
            static_cast<void>(ledger.begin_timed_bitstream_frame(changed));
        });
    }
    OperationLedger recovered(options(directory));
    CHECK(recovered.mutation_available());
    const auto found = recovered.lookup(operation_id, "owner");
    CHECK(found.record->state == OperationState::Unknown);
    CHECK(found.record->recovery == OperationRecovery::ScopeBlocked);
    CHECK(found.record->requested_payload_digest.has_value());

    TestDirectory stop_directory;
    RuntimeTimedBitstreamStopOperation stop{
        identity(0x11U), identity(0x72U), identity(0x33U), "owner",
        "stop-idem", 2U, 7U, 0x0A000000U};
    OperationLedger stop_ledger(options(stop_directory));
    const auto begun = stop_ledger.begin_timed_bitstream_stop(stop);
    OperationTerminalResult result;
    result.object_id = 42U;
    const auto terminal = stop_ledger.finish(
        begun.record.operation_id, begun.record.request_digest,
        OperationState::Committed, OperationRecovery::SafeClosed, result);
    CHECK(terminal.state == OperationState::Committed);
    CHECK(stop_ledger.blocked_scopes().empty());
}

void check_bus_resource_reset_v4_identity_and_scope_recovery() {
    static_assert(static_cast<std::uint8_t>(
                      OperationKind::RuntimeBusResourceReset) == 8U,
                  "总线资源复位操作类型必须保持稳定");
    TestDirectory directory;
    const auto operation = bus_reset_operation();
    const auto peer = bus_reset_operation(0x0A000002U, "peer-reset");
    CHECK(OperationLedger::derive_operation_id(operation) !=
          OperationLedger::derive_operation_id(peer));
    CHECK(OperationLedger::derive_request_digest(operation) !=
          OperationLedger::derive_request_digest(peer));

    OperationDigest pending_id{};
    {
        OperationLedger ledger(options(directory));
        const auto begun = ledger.begin_bus_resource_reset(operation);
        pending_id = begun.record.operation_id;
        CHECK(begun.record.kind == OperationKind::RuntimeBusResourceReset);
        CHECK(begun.disposition ==
              OperationBeginDisposition::StartedDurablePending);
        CHECK(ledger.begin_bus_resource_reset(operation).disposition ==
              OperationBeginDisposition::ExistingPending);
        CHECK(ledger.blocked_scopes().size() == 1U);

        const auto peer_begun = ledger.begin_bus_resource_reset(peer);
        CHECK(ledger.blocked_scopes().size() == 2U);
        (void)ledger.finish(peer_begun.record.operation_id,
                            peer_begun.record.request_digest,
                            OperationState::Committed,
                            OperationRecovery::SafeClosed);
        CHECK(ledger.blocked_scopes().size() == 1U);

        auto conflict = operation;
        conflict.node_id = 8U;
        CHECK(OperationLedger::derive_operation_id(conflict) == pending_id);
        check_error(OperationLedgerError::IdempotencyConflict, [&] {
            static_cast<void>(ledger.begin_bus_resource_reset(conflict));
        });
    }

    OperationLedger recovered(options(directory));
    CHECK(recovered.mutation_available());
    const auto found = recovered.lookup(pending_id, "owner");
    CHECK(found.disposition == OperationLookupDisposition::Found);
    CHECK(found.record->kind == OperationKind::RuntimeBusResourceReset);
    CHECK(found.record->state == OperationState::Unknown);
    CHECK(found.record->recovery == OperationRecovery::ScopeBlocked);
    CHECK(recovered.blocked_scopes().size() == 1U);
    CHECK(recovered.blocked_scopes().front().expected_node_uuid ==
          operation.expected_node_uuid);
    CHECK(recovered.blocked_scopes().front().resource_id ==
          operation.resource_id);
}

void check_motion_group_cancel_identity_contract() {
    RuntimeMotionGroupCancelOperation operation{
        identity(0x11U), identity(0x22U), "owner", "stop-7", 1U,
        0x1122334455667788ULL, 7U, 3U};
    const auto operation_id = OperationLedger::derive_operation_id(operation);
    const auto request_digest = OperationLedger::derive_request_digest(operation);

    auto changed_group = operation;
    changed_group.group_id = 8U;
    CHECK(OperationLedger::derive_operation_id(changed_group) == operation_id);
    CHECK(OperationLedger::derive_request_digest(changed_group) != request_digest);

    auto changed_generation = operation;
    changed_generation.plan_generation = 4U;
    CHECK(OperationLedger::derive_operation_id(changed_generation) == operation_id);
    CHECK(OperationLedger::derive_request_digest(changed_generation) !=
          request_digest);

    auto changed_owner = operation;
    changed_owner.owner_key_id = "other";
    CHECK(OperationLedger::derive_operation_id(changed_owner) != operation_id);

    auto invalid = operation;
    invalid.transaction_id = 0U;
    check_error(OperationLedgerError::InvalidRequest, [&] {
        static_cast<void>(OperationLedger::derive_request_digest(invalid));
    });

    TestDirectory directory;
    OperationDigest persisted_id{};
    {
        OperationLedger ledger(options(directory));
        const auto begun = ledger.begin_motion_group_cancel(operation);
        persisted_id = begun.record.operation_id;
        CHECK(begun.record.kind == OperationKind::RuntimeMotionGroupCancel);
        CHECK(begun.record.motion_transaction_id == operation.transaction_id);
        CHECK(begun.record.motion_group_id == operation.group_id);
        CHECK(begun.record.motion_plan_generation == operation.plan_generation);
    }
    OperationLedger recovered(options(directory));
    const auto found = recovered.lookup(persisted_id, "owner");
    CHECK(found.disposition == OperationLookupDisposition::Found);
    CHECK(found.record->state == OperationState::Unknown);
    CHECK(found.record->recovery == OperationRecovery::ScopeBlocked);
    CHECK(found.record->motion_transaction_id == operation.transaction_id);
    CHECK(found.record->motion_group_id == operation.group_id);
    CHECK(found.record->motion_plan_generation == operation.plan_generation);
}

}  // namespace

int main() {
    check_v1_golden_restarts_and_upgrades_with_v2_recovery();
    check_unknown_disk_versions_fail_closed();
    check_pwm_record_is_lossless_and_durable();
    check_pwm_stop_ids_terminal_and_restart_recovery();
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
    check_timed_bitstream_v3_digest_only_and_recovery();
    check_bus_resource_reset_v4_identity_and_scope_recovery();
    check_motion_group_cancel_identity_contract();
    return 0;
}
