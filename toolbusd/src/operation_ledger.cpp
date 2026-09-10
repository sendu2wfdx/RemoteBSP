#include "remotebsp/toolbusd/operation_ledger.hpp"

#include "remotebsp/protocol/crc32.hpp"
#include "remotebsp/protocol/waveform.hpp"
#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace remotebsp::toolbusd {
namespace {

constexpr std::size_t kRecordHeaderBytes = 136U;
constexpr std::size_t kRecordTrailerBytes = 12U;
constexpr std::size_t kMaximumRecordBytes = 1024U;
constexpr std::size_t kManifestBytes = 152U;
constexpr std::uint16_t kLedgerFormatVersion = 2U;
constexpr std::uint16_t kRecordFormatVersion = 3U;
constexpr std::uint64_t kCommitMarker = 0x314D4F434C4F4252ULL;
constexpr char kRecordMagic[8] = {'R', 'B', 'O', 'P', 'L', 'G', '1', '\0'};
constexpr char kManifestMagic[8] = {'R', 'B', 'O', 'L', 'M', 'F', '1', '\0'};
constexpr const char* kManifestName = "manifest.v1";
constexpr const char* kLockName = "ledger.lock";

static_assert(static_cast<std::uint16_t>(
                  RuntimeOperationError::Persistence) ==
              kOperationLedgerPersistenceErrorCode,
              "账本Persistence错误码必须与IPC合同一致");

enum class DiskRecordType : std::uint8_t {
    Full = 1U,
    Tombstone = 2U,
    CompactedFull = 3U,
};

struct Sha256State {
    std::array<std::uint32_t, 8> words{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> block{};
    std::uint64_t bytes{};
    std::size_t used{};
};

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

void sha256_transform(Sha256State& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16U; ++i) {
        const auto offset = i * 4U;
        schedule[i] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                      (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t i = 16U; i < schedule.size(); ++i) {
        const auto s0 = rotate_right(schedule[i - 15U], 7U) ^
                        rotate_right(schedule[i - 15U], 18U) ^
                        (schedule[i - 15U] >> 3U);
        const auto s1 = rotate_right(schedule[i - 2U], 17U) ^
                        rotate_right(schedule[i - 2U], 19U) ^
                        (schedule[i - 2U] >> 10U);
        schedule[i] = schedule[i - 16U] + s0 + schedule[i - 7U] + s1;
    }
    auto a = state.words[0];
    auto b = state.words[1];
    auto c = state.words[2];
    auto d = state.words[3];
    auto e = state.words[4];
    auto f = state.words[5];
    auto g = state.words[6];
    auto h = state.words[7];
    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const auto sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                          rotate_right(e, 25U);
        const auto choice = (e & f) ^ ((~e) & g);
        const auto temp1 = h + sum1 + choice + kSha256Constants[i] +
                           schedule[i];
        const auto sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                          rotate_right(a, 22U);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state.words[0] += a;
    state.words[1] += b;
    state.words[2] += c;
    state.words[3] += d;
    state.words[4] += e;
    state.words[5] += f;
    state.words[6] += g;
    state.words[7] += h;
}

void sha256_update(Sha256State& state, const std::uint8_t* data,
                   std::size_t size) {
    state.bytes += size;
    while (size != 0U) {
        const auto count = std::min(size, state.block.size() - state.used);
        std::memcpy(state.block.data() + state.used, data, count);
        state.used += count;
        data += count;
        size -= count;
        if (state.used == state.block.size()) {
            sha256_transform(state, state.block.data());
            state.used = 0U;
        }
    }
}

OperationDigest sha256(const std::uint8_t* data, std::size_t size) {
    Sha256State state;
    sha256_update(state, data, size);
    const auto bit_count = state.bytes * 8ULL;
    const std::uint8_t one = 0x80U;
    sha256_update(state, &one, 1U);
    const std::uint8_t zero = 0U;
    while (state.used != 56U) {
        sha256_update(state, &zero, 1U);
    }
    std::array<std::uint8_t, 8> length{};
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[length.size() - 1U - i] =
            static_cast<std::uint8_t>(bit_count >> (i * 8U));
    }
    sha256_update(state, length.data(), length.size());
    OperationDigest result{};
    for (std::size_t i = 0; i < state.words.size(); ++i) {
        result[i * 4U] = static_cast<std::uint8_t>(state.words[i] >> 24U);
        result[i * 4U + 1U] =
            static_cast<std::uint8_t>(state.words[i] >> 16U);
        result[i * 4U + 2U] =
            static_cast<std::uint8_t>(state.words[i] >> 8U);
        result[i * 4U + 3U] = static_cast<std::uint8_t>(state.words[i]);
    }
    return result;
}

OperationDigest sha256(const std::vector<std::uint8_t>& bytes) {
    return sha256(bytes.data(), bytes.size());
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t get_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t get_u32(const std::uint8_t* data) {
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(data[shift / 8U]) << shift;
    }
    return value;
}

std::uint64_t get_u64(const std::uint8_t* data) {
    std::uint64_t value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(data[shift / 8U]) << shift;
    }
    return value;
}

template <std::size_t N>
void put_array(std::vector<std::uint8_t>& out,
               const std::array<std::uint8_t, N>& value) {
    out.insert(out.end(), value.begin(), value.end());
}

template <std::size_t N>
std::array<std::uint8_t, N> get_array(const std::uint8_t* data) {
    std::array<std::uint8_t, N> result{};
    std::copy(data, data + N, result.begin());
    return result;
}

bool is_zero(const OperationIdentity& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](std::uint8_t byte) { return byte == 0U; });
}

bool valid_text(const std::string& value, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) ||
        value.size() > kOperationLedgerMaximumTextBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

std::string digest_key(const OperationDigest& value) {
    return std::string(reinterpret_cast<const char*>(value.data()),
                       value.size());
}

std::string scope_key(const OperationScope& scope) {
    std::string result(
        reinterpret_cast<const char*>(scope.expected_node_uuid.data()),
        scope.expected_node_uuid.size());
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        result.push_back(static_cast<char>(scope.resource_id >> shift));
    }
    return result;
}

void append_domain(std::vector<std::uint8_t>& out) {
    constexpr char domain[] = "RemoteBSP/runtime-operation/v1";
    out.insert(out.end(), domain, domain + sizeof(domain));
}

void append_text(std::vector<std::uint8_t>& out, const std::string& value) {
    put_u16(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void validate_common(const OperationIdentity& daemon,
                     const OperationIdentity& lease,
                     const std::string& owner) {
    if (is_zero(daemon) || is_zero(lease) || !valid_text(owner)) {
        throw OperationLedgerException(
            OperationLedgerError::InvalidRequest,
            "操作身份、租约或owner不符合账本合同");
    }
}

std::uint64_t default_wall_clock_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string segment_name(std::uint64_t number) {
    constexpr char digits[] = "0123456789abcdef";
    std::string name = "segment-0000000000000000.rbol";
    for (std::size_t i = 0; i < 16U; ++i) {
        name[23U - i] = digits[(number >> (i * 4U)) & 0x0fU];
    }
    return name;
}

bool valid_kind(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(OperationKind::RuntimeGpioWrite) ||
           value == static_cast<std::uint8_t>(
               OperationKind::RuntimeControlRelease) ||
           value == static_cast<std::uint8_t>(OperationKind::RuntimePwmConfigure) ||
           value == static_cast<std::uint8_t>(OperationKind::RuntimePwmStop) ||
           value == static_cast<std::uint8_t>(OperationKind::RuntimeTimedBitstreamConfigure) ||
           value == static_cast<std::uint8_t>(OperationKind::RuntimeTimedBitstreamFrame) ||
           value == static_cast<std::uint8_t>(OperationKind::RuntimeTimedBitstreamStop);
}

bool parse_segment_name(const std::string& name,
                        std::uint64_t& number) noexcept {
    if (name.size() != 29U || name.compare(0U, 8U, "segment-") != 0 ||
        name.compare(24U, 5U, ".rbol") != 0) {
        return false;
    }
    number = 0U;
    for (std::size_t i = 8U; i < 24U; ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        std::uint8_t nibble{};
        if (byte >= '0' && byte <= '9') {
            nibble = static_cast<std::uint8_t>(byte - '0');
        } else if (byte >= 'a' && byte <= 'f') {
            nibble = static_cast<std::uint8_t>(byte - 'a' + 10U);
        } else {
            return false;
        }
        number = (number << 4U) | nibble;
    }
    return number != 0U;
}

bool valid_state(std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(OperationState::Pending) &&
           value <= static_cast<std::uint8_t>(OperationState::Unknown);
}

bool valid_recovery(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
               OperationRecovery::NodeRebootConfirmed);
}

bool valid_state_recovery(OperationKind kind, OperationState state,
                          OperationRecovery recovery) noexcept {
    if (state == OperationState::Pending) {
        return recovery == OperationRecovery::None;
    }
    if (state == OperationState::Committed) {
        const bool closes_scope =
            kind == OperationKind::RuntimeControlRelease ||
            kind == OperationKind::RuntimePwmStop ||
            kind == OperationKind::RuntimeTimedBitstreamStop;
        return recovery == (closes_scope ? OperationRecovery::SafeClosed
                                         : OperationRecovery::None);
    }
    if (state == OperationState::Rejected) {
        return recovery == OperationRecovery::NotSent ||
               recovery == OperationRecovery::SafeClosed;
    }
    return state == OperationState::Unknown &&
           (recovery == OperationRecovery::ScopeBlocked ||
            recovery == OperationRecovery::AwaitingReboot ||
            recovery == OperationRecovery::SafeClosed ||
            recovery == OperationRecovery::NodeRebootConfirmed);
}

bool valid_terminal_result(const OperationRecord& existing,
                           OperationState state,
                           const OperationTerminalResult& result) noexcept {
    if (state == OperationState::Committed) {
        if (result.stable_error_code != 0U) {
            return false;
        }
        if (existing.kind == OperationKind::RuntimeGpioWrite) {
            return result.object_id.has_value() &&
                   *result.object_id != 0U && result.value.has_value() &&
                   existing.requested_value == result.value;
        }
        if (existing.kind == OperationKind::RuntimePwmConfigure) {
            return result.object_id.has_value() && *result.object_id != 0U &&
                   result.frequency_hz == existing.requested_frequency_hz &&
                   result.duty == existing.requested_duty &&
                   result.active_low == existing.requested_active_low;
        }
        if (existing.kind == OperationKind::RuntimePwmStop) {
            return result.object_id.has_value() && *result.object_id != 0U &&
                   !result.value.has_value() && !result.frequency_hz.has_value() &&
                   !result.duty.has_value() && !result.active_low.has_value();
        }
        if (existing.kind == OperationKind::RuntimeTimedBitstreamConfigure ||
            existing.kind == OperationKind::RuntimeTimedBitstreamFrame ||
            existing.kind == OperationKind::RuntimeTimedBitstreamStop) {
            return result.object_id.has_value() && *result.object_id != 0U &&
                   !result.value.has_value() && !result.frequency_hz.has_value() &&
                   !result.duty.has_value() && !result.active_low.has_value();
        }
        return !result.object_id.has_value() && !result.value.has_value() &&
               !result.frequency_hz.has_value() && !result.duty.has_value() &&
               !result.active_low.has_value();
    }
    return !result.object_id.has_value() && !result.value.has_value() &&
           !result.frequency_hz.has_value() && !result.duty.has_value() &&
           !result.active_low.has_value() &&
           result.stable_error_code != 0U &&
           result.stable_error_code <=
               kOperationLedgerMaximumStableErrorCode;
}

bool recovery_advances(OperationRecovery previous,
                       OperationRecovery next) noexcept {
    if (previous == next) {
        return true;
    }
    if (previous == OperationRecovery::ScopeBlocked) {
        return next == OperationRecovery::AwaitingReboot ||
               next == OperationRecovery::SafeClosed ||
               next == OperationRecovery::NodeRebootConfirmed;
    }
    if (previous == OperationRecovery::AwaitingReboot) {
        return next == OperationRecovery::SafeClosed ||
               next == OperationRecovery::NodeRebootConfirmed;
    }
    return false;
}

OperationDigest business_key_hash(OperationKind kind,
                                  const OperationIdentity& lease_id,
                                  const std::string& owner,
                                  const std::string& idempotency) {
    std::vector<std::uint8_t> canonical;
    constexpr char domain[] = "RemoteBSP/runtime-operation-business-key/v1";
    canonical.insert(canonical.end(), domain, domain + sizeof(domain));
    canonical.push_back(static_cast<std::uint8_t>(kind));
    append_text(canonical, owner);
    if (kind == OperationKind::RuntimeControlRelease) {
        // Release的固定键只能在租约命名空间内使用，否则同一owner将无法
        // 依次释放两个不同租约。
        put_array(canonical, lease_id);
    }
    append_text(canonical, idempotency);
    return sha256(canonical);
}

OperationDigest timed_request_digest(
    const OperationDigest& operation_id, const OperationIdentity& daemon_origin,
    const OperationIdentity& lease_id, const OperationIdentity& node_uuid,
    const std::string& owner, std::uint16_t permissions, std::uint32_t node_id,
    std::uint32_t resource_id, std::uint8_t domain,
    const OperationDigest* payload_digest);

}  // namespace

OperationLedgerException::OperationLedgerException(
    OperationLedgerError code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

OperationLedgerError OperationLedgerException::code() const noexcept {
    return code_;
}

bool OperationScope::operator==(const OperationScope& other) const noexcept {
    return expected_node_uuid == other.expected_node_uuid &&
           resource_id == other.resource_id;
}

class OperationLedger::Impl {
public:
    explicit Impl(OperationLedgerOptions options) : options_(std::move(options)) {
        validate_options();
        try {
            open_directory_and_lock();
        } catch (...) {
            if (lock_fd_ >= 0) {
                (void)::close(lock_fd_);
                lock_fd_ = -1;
            }
            if (directory_fd_ >= 0) {
                (void)::close(directory_fd_);
                directory_fd_ = -1;
            }
            throw;
        }
        try {
            load_or_initialize();
            recover_pending();
            available_.store(true);
        } catch (const std::exception& error) {
            set_failure(error.what());
        }
    }

    ~Impl() noexcept {
        if (lock_fd_ >= 0) {
            (void)::flock(lock_fd_, LOCK_UN);
            (void)::close(lock_fd_);
        }
        if (directory_fd_ >= 0) {
            (void)::close(directory_fd_);
        }
    }

    OperationBeginResult begin_gpio_write(
        const RuntimeGpioWriteOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U ||
            !valid_text(operation.idempotency_key)) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "GPIO写参数不符合账本合同");
        }
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest =
            OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimeGpioWrite;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        record.requested_value = operation.value;
        return begin(std::move(record));
    }

    OperationBeginResult begin_pwm_configure(
        const RuntimePwmConfigureOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U || operation.frequency_hz == 0U ||
            operation.duty > 10000U ||
            !valid_text(operation.idempotency_key)) {
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "PWM配置参数不符合账本合同");
        }
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest = OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimePwmConfigure;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        record.requested_frequency_hz = operation.frequency_hz;
        record.requested_duty = operation.duty;
        record.requested_active_low = operation.active_low;
        return begin(std::move(record));
    }

    OperationBeginResult begin_pwm_stop(
        const RuntimePwmStopOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U ||
            !valid_text(operation.idempotency_key)) {
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "PWM停止参数不符合账本合同");
        }
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest = OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimePwmStop;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        return begin(std::move(record));
    }

    OperationBeginResult begin_timed_bitstream_configure(
        const RuntimeTimedBitstreamConfigureOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        const protocol::TimedBitstreamCreatePayload payload{
            0U, operation.bit_period_ns, operation.zero_high_ns,
            operation.one_high_ns, operation.reset_time_us};
        try { static_cast<void>(protocol::encode_timed_bitstream_create(payload)); }
        catch (const protocol::WaveformPayloadException&) {
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "定时位流配置参数不符合账本合同");
        }
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U || !valid_text(operation.idempotency_key))
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "定时位流配置身份不符合账本合同");
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest = OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimeTimedBitstreamConfigure;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        std::vector<std::uint8_t> canonical;
        put_u32(canonical, operation.bit_period_ns);
        put_u32(canonical, operation.zero_high_ns);
        put_u32(canonical, operation.one_high_ns);
        put_u32(canonical, operation.reset_time_us);
        record.requested_payload_digest = sha256(canonical);
        return begin(std::move(record));
    }

    OperationBeginResult begin_timed_bitstream_frame(
        const RuntimeTimedBitstreamFrameOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        try { static_cast<void>(protocol::encode_timed_bitstream_write(
                  {operation.bit_count, operation.data})); }
        catch (const protocol::WaveformPayloadException&) {
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "定时位流帧不符合账本合同");
        }
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U || !valid_text(operation.idempotency_key))
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "定时位流帧身份不符合账本合同");
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest = OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimeTimedBitstreamFrame;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        std::vector<std::uint8_t> canonical;
        put_u16(canonical, operation.bit_count);
        canonical.insert(canonical.end(), operation.data.begin(), operation.data.end());
        record.requested_payload_digest = sha256(canonical);
        return begin(std::move(record));
    }

    OperationBeginResult begin_timed_bitstream_stop(
        const RuntimeTimedBitstreamStopOperation& operation) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
            operation.node_id > 127U || operation.resource_id == 0U ||
            operation.permissions == 0U || !valid_text(operation.idempotency_key))
            throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                           "定时位流停止参数不符合账本合同");
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest = OperationLedger::derive_request_digest(operation);
        record.kind = OperationKind::RuntimeTimedBitstreamStop;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {operation.expected_node_uuid, operation.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = operation.idempotency_key;
        record.permissions = operation.permissions;
        record.node_id = operation.node_id;
        return begin(std::move(record));
    }

    OperationBeginResult begin_release(
        const RuntimeControlReleaseOperation& operation,
        const ServerResolvedLease& lease) {
        validate_common(operation.daemon_origin, operation.lease_id,
                        operation.owner_key_id);
        if (is_zero(lease.expected_node_uuid) || lease.node_id == 0U ||
            lease.node_id > 127U || lease.resource_id == 0U ||
            lease.permissions == 0U || lease.admission_id == 0U) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "服务端解析的Release租约无效");
        }
        OperationRecord record;
        record.operation_id = OperationLedger::derive_operation_id(operation);
        record.request_digest =
            OperationLedger::derive_request_digest(operation, lease);
        record.kind = OperationKind::RuntimeControlRelease;
        record.daemon_origin = operation.daemon_origin;
        record.lease_id = operation.lease_id;
        record.scope = {lease.expected_node_uuid, lease.resource_id};
        record.owner_key_id = operation.owner_key_id;
        record.idempotency_key = "release:v1";
        record.permissions = lease.permissions;
        record.node_id = lease.node_id;
        record.admission_id = lease.admission_id;
        return begin(std::move(record));
    }

    OperationRecord finish(const OperationDigest& operation_id,
                           const OperationDigest& request_digest,
                           OperationState state,
                           OperationRecovery recovery,
                           const OperationTerminalResult& result) {
        if (state == OperationState::Pending ||
            !valid_state(static_cast<std::uint8_t>(state)) ||
            !valid_recovery(static_cast<std::uint8_t>(recovery))) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "账本终态或恢复状态无效");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        const auto found = entries_.find(digest_key(operation_id));
        if (found == entries_.end() || !found->second.record.full_record) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "待完成的账本操作不存在");
        }
        auto& existing = found->second.record;
        if (existing.request_digest != request_digest) {
            throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict,
                "operation_id与request_digest不匹配");
        }
        if (!valid_state_recovery(existing.kind, state, recovery) ||
            !valid_terminal_result(existing, state, result)) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "账本终态、恢复证据或结果字段组合无效");
        }
        if (existing.state != OperationState::Pending) {
            if (existing.state == state && existing.recovery == recovery &&
                existing.result.object_id == result.object_id &&
                existing.result.value == result.value &&
                existing.result.stable_error_code == result.stable_error_code) {
                return existing;
            }
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "账本终态不可反转或改写");
        }
        OperationRecord terminal = existing;
        terminal.state = state;
        terminal.recovery = recovery;
        terminal.result = result;
        try {
            append_record(terminal, DiskRecordType::Full, nullptr, nullptr,
                          false);
        } catch (...) {
            existing.state = OperationState::Unknown;
            existing.recovery = OperationRecovery::ScopeBlocked;
            existing.result = {};
            existing.result.stable_error_code =
                kOperationLedgerPersistenceErrorCode;
            set_failure("OperationLedger终态无法持久化");
            try {
                apply_scope(existing);
            } catch (...) {
            }
            throw;
        }
        found->second.record = std::move(terminal);
        try {
            if (found->second.record.state == OperationState::Rejected &&
                found->second.record.recovery ==
                    OperationRecovery::NotSent) {
                // pending只是临时阻断；确定未发送后必须恢复它之前的生命周期
                // 锚点，不能让no-op Release替代真正的孤儿对象证据。
                rebuild_blocked_scopes();
            } else {
                apply_scope(found->second.record);
            }
        } catch (...) {
            set_failure("OperationLedger终态作用域索引发布失败");
            throw;
        }
        return found->second.record;
    }

    OperationRecord update_unknown_recovery(
        const OperationDigest& operation_id,
        const OperationDigest& request_digest,
        OperationRecovery recovery) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        const auto found = entries_.find(digest_key(operation_id));
        if (found == entries_.end() || !found->second.record.full_record) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "待更新的unknown操作不存在");
        }
        auto& existing = found->second.record;
        if (existing.request_digest != request_digest) {
            throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict,
                "unknown恢复更新的request_digest不匹配");
        }
        if (existing.state != OperationState::Unknown ||
            !valid_state_recovery(existing.kind, OperationState::Unknown,
                                  recovery) ||
            !recovery_advances(existing.recovery, recovery)) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidTransition,
                "unknown恢复证据发生非法倒退或分叉");
        }
        if (existing.recovery == recovery) {
            return existing;
        }
        OperationRecord updated = existing;
        updated.recovery = recovery;
        append_record(updated, DiskRecordType::Full, nullptr, nullptr, false);
        existing = std::move(updated);
        try {
            apply_scope(existing);
        } catch (...) {
            set_failure("OperationLedger恢复作用域索引发布失败");
            throw;
        }
        return existing;
    }

    OperationLookupResult lookup(const OperationDigest& operation_id,
                                 const std::string& owner) const {
        if (!valid_text(owner)) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "查询owner不符合账本合同");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        const auto found = entries_.find(digest_key(operation_id));
        if (found == entries_.end()) {
            return {};
        }
        if (!found->second.record.full_record) {
            return {};
        }
        const auto supplied_hash = sha256(
            reinterpret_cast<const std::uint8_t*>(owner.data()), owner.size());
        if (found->second.owner_hash != supplied_hash) {
            // 与真正不存在或已轮转的记录返回完全相同的形状，避免调用者
            // 通过错误类别探测其他 owner 的 operation_id 是否存在。
            return {};
        }
        return {OperationLookupDisposition::Found, found->second.record};
    }

    std::vector<OperationScope> blocked_scopes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        std::vector<OperationScope> result;
        result.reserve(blocked_scopes_.size());
        for (const auto& item : blocked_scopes_) {
            result.push_back(item.second);
        }
        return result;
    }

    bool mutation_available() const noexcept {
        return available_.load();
    }

    std::string failure_reason() const {
        std::lock_guard<std::mutex> lock(failure_mutex_);
        return failure_reason_;
    }

    std::size_t operation_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    void compact() {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        compact_locked(options_.wall_clock_ms());
    }

private:
    struct StoredEntry {
        OperationRecord record;
        OperationDigest owner_hash{};
        OperationDigest business_key_hash{};
    };

    struct Manifest {
        std::uint64_t generation{1U};
        std::uint64_t first_segment{1U};
        std::uint64_t active_segment{1U};
        std::uint64_t active_size{};
        std::uint64_t first_sequence{1U};
        std::uint64_t last_sequence{};
        std::uint64_t logical_total_bytes{};
        std::uint64_t operation_count{};
        OperationDigest first_previous_hash{};
        OperationDigest last_record_hash{};
    };

    struct DecodedRecord {
        OperationRecord record;
        OperationDigest owner_hash{};
        OperationDigest business_key_hash{};
        OperationDigest previous_hash{};
        OperationDigest record_hash{};
        DiskRecordType type{DiskRecordType::Full};
        std::size_t bytes{};
    };

    void validate_options() {
        if (options_.directory.empty() || options_.maximum_operations == 0U ||
            options_.maximum_operations > 65536U ||
            options_.maximum_segment_bytes < kMaximumRecordBytes ||
            options_.maximum_segment_bytes > 4ULL * 1024ULL * 1024ULL ||
            options_.maximum_total_bytes < 2U * kMaximumRecordBytes ||
            options_.maximum_total_bytes > 64ULL * 1024ULL * 1024ULL ||
            options_.full_terminal_retention_ms < kOperationLedgerDayMs ||
            options_.tombstone_retention_ms <
                options_.full_terminal_retention_ms) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "OperationLedger容量或保留参数无效");
        }
        if (!options_.wall_clock_ms) {
            options_.wall_clock_ms = default_wall_clock_ms;
        }
    }

    void open_directory_and_lock() {
        const auto path = options_.directory.string();
        if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            throw_io("创建账本目录失败");
        }
        directory_fd_ = ::open(path.c_str(),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory_fd_ < 0) {
            throw OperationLedgerException(
                OperationLedgerError::DirectoryUnsafe,
                "账本目录不是可安全打开的真实目录");
        }
        struct stat status {};
        if (::fstat(directory_fd_, &status) != 0) {
            throw_io("读取账本目录属性失败");
        }
        if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
            (status.st_mode & 0077) != 0) {
            throw OperationLedgerException(
                OperationLedgerError::DirectoryUnsafe,
                "账本目录属主或权限不安全");
        }
        lock_fd_ = ::openat(directory_fd_, kLockName,
                            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lock_fd_ < 0) {
            throw_io("打开账本锁文件失败");
        }
        validate_regular_file(lock_fd_, kLockName);
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                throw OperationLedgerException(
                    OperationLedgerError::AlreadyLocked,
                    "账本已被另一个toolbusd实例锁定");
            }
            throw_io("取得账本进程锁失败");
        }
    }

    void validate_regular_file(int fd, const std::string& name) const {
        struct stat status {};
        if (::fstat(fd, &status) != 0) {
            throw_io("读取账本文件属性失败: " + name);
        }
        if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
            status.st_nlink != 1 ||
            (status.st_mode & 0077) != 0) {
            throw OperationLedgerException(
                OperationLedgerError::DirectoryUnsafe,
                "账本文件属主、类型或权限不安全: " + name);
        }
    }

    [[noreturn]] static void throw_io(const std::string& message) {
        throw OperationLedgerException(
            OperationLedgerError::IoFailure,
            message + ": " + std::strerror(errno));
    }

    [[noreturn]] static void throw_corrupt(const std::string& message) {
        throw OperationLedgerException(OperationLedgerError::Corrupt,
                                       message);
    }

    void require_available() const {
        if (!available_.load()) {
            throw OperationLedgerException(
                OperationLedgerError::MutationUnavailable,
                "OperationLedger处于失败关闭状态");
        }
    }

    void set_failure(const std::string& reason) noexcept {
        available_.store(false);
        try {
            std::lock_guard<std::mutex> lock(failure_mutex_);
            failure_reason_ = reason;
        } catch (...) {
        }
    }

    void invoke_fault(LedgerIoPoint point) {
        if (!options_.fault_hook) {
            return;
        }
        for (;;) {
            try {
                options_.fault_hook(point);
                return;
            } catch (const std::system_error& error) {
                if (error.code().value() == EINTR) {
                    continue;
                }
                throw OperationLedgerException(
                    OperationLedgerError::IoFailure,
                    "OperationLedger故障注入: " +
                        std::string(error.what()));
            } catch (const OperationLedgerException&) {
                throw;
            } catch (const std::exception& error) {
                throw OperationLedgerException(
                    OperationLedgerError::IoFailure,
                    "OperationLedger故障注入: " +
                        std::string(error.what()));
            }
        }
    }

    void write_all(int fd, const std::vector<std::uint8_t>& data,
                   LedgerIoPoint point) {
        std::size_t offset = 0U;
        while (offset < data.size()) {
            invoke_fault(point);
            auto count = data.size() - offset;
            if (options_.maximum_write_chunk != 0U) {
                count = std::min(count, options_.maximum_write_chunk);
            }
            const auto written = ::write(fd, data.data() + offset, count);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_io("写入OperationLedger失败");
            }
            if (written == 0) {
                errno = EIO;
                throw_io("OperationLedger发生零长度短写");
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    void data_sync(int fd, LedgerIoPoint point) {
        invoke_fault(point);
        while (::fdatasync(fd) != 0) {
            if (errno != EINTR) {
                throw_io("同步OperationLedger数据失败");
            }
        }
    }

    void directory_sync() {
        invoke_fault(LedgerIoPoint::DirectorySync);
        while (::fsync(directory_fd_) != 0) {
            if (errno != EINTR) {
                throw_io("同步OperationLedger目录失败");
            }
        }
    }

    int open_segment(std::uint64_t number, int flags) const {
        const auto name = segment_name(number);
        const auto fd = ::openat(directory_fd_, name.c_str(),
                                 flags | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw_io("打开账本分段失败: " + name);
        }
        try {
            validate_regular_file(fd, name);
        } catch (...) {
            (void)::close(fd);
            throw;
        }
        return fd;
    }

    void create_segment(std::uint64_t number) {
        invoke_fault(LedgerIoPoint::SegmentCreate);
        const auto fd = open_segment(number, O_RDWR | O_CREAT | O_EXCL);
        try {
            data_sync(fd, LedgerIoPoint::RecordDataSync);
        } catch (...) {
            (void)::close(fd);
            throw;
        }
        (void)::close(fd);
        directory_sync();
    }

    std::vector<std::uint8_t> read_exact_file(int fd, std::size_t size) const {
        std::vector<std::uint8_t> result(size);
        std::size_t offset = 0U;
        while (offset < size) {
            const auto count = ::pread(fd, result.data() + offset,
                                       size - offset,
                                       static_cast<off_t>(offset));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw_io("读取OperationLedger失败");
            }
            if (count == 0) {
                throw_corrupt("OperationLedger文件意外截断");
            }
            offset += static_cast<std::size_t>(count);
        }
        return result;
    }

    std::vector<std::uint8_t> encode_manifest(const Manifest& value) const {
        std::vector<std::uint8_t> out;
        out.reserve(kManifestBytes);
        out.insert(out.end(), kManifestMagic, kManifestMagic + 8U);
        put_u16(out, kLedgerFormatVersion);
        put_u16(out, static_cast<std::uint16_t>(kManifestBytes));
        put_u64(out, value.generation);
        put_u64(out, value.first_segment);
        put_u64(out, value.active_segment);
        put_u64(out, value.active_size);
        put_u64(out, value.first_sequence);
        put_u64(out, value.last_sequence);
        put_u64(out, value.logical_total_bytes);
        put_u64(out, value.operation_count);
        put_array(out, value.first_previous_hash);
        put_array(out, value.last_record_hash);
        const auto crc = protocol::crc32(out.data(), out.size());
        put_u32(out, crc);
        put_u64(out, kCommitMarker);
        if (out.size() != kManifestBytes) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "内部manifest编码长度错误");
        }
        return out;
    }

    Manifest decode_manifest(const std::vector<std::uint8_t>& data) const {
        if (data.size() != kManifestBytes ||
            std::memcmp(data.data(), kManifestMagic, 8U) != 0 ||
            (get_u16(data.data() + 8U) != 1U &&
             get_u16(data.data() + 8U) != kLedgerFormatVersion) ||
            get_u16(data.data() + 10U) != kManifestBytes ||
            get_u64(data.data() + kManifestBytes - 8U) != kCommitMarker ||
            get_u32(data.data() + kManifestBytes - 12U) !=
                protocol::crc32(data.data(), kManifestBytes - 12U)) {
            throw_corrupt("OperationLedger manifest版本、长度或校验无效");
        }
        Manifest value;
        value.generation = get_u64(data.data() + 12U);
        value.first_segment = get_u64(data.data() + 20U);
        value.active_segment = get_u64(data.data() + 28U);
        value.active_size = get_u64(data.data() + 36U);
        value.first_sequence = get_u64(data.data() + 44U);
        value.last_sequence = get_u64(data.data() + 52U);
        value.logical_total_bytes = get_u64(data.data() + 60U);
        value.operation_count = get_u64(data.data() + 68U);
        value.first_previous_hash =
            get_array<32U>(data.data() + 76U);
        value.last_record_hash = get_array<32U>(data.data() + 108U);
        if (value.generation == 0U || value.first_segment == 0U ||
            value.active_segment < value.first_segment ||
            value.active_segment - value.first_segment >= 65536U ||
            value.active_size > options_.maximum_segment_bytes ||
            value.logical_total_bytes > options_.maximum_total_bytes ||
            value.operation_count > options_.maximum_operations ||
            value.first_sequence == 0U ||
            value.last_sequence + 1U < value.first_sequence) {
            throw_corrupt("OperationLedger manifest字段越界");
        }
        return value;
    }

    std::string unique_temp_name() {
        const auto counter = temp_counter_.fetch_add(1U);
        return ".manifest.tmp." + std::to_string(::getpid()) + "." +
               std::to_string(counter);
    }

    void store_manifest(const Manifest& value) {
        const auto data = encode_manifest(value);
        const auto temporary = unique_temp_name();
        int fd = -1;
        try {
            invoke_fault(LedgerIoPoint::ManifestWrite);
            fd = ::openat(directory_fd_, temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600);
            if (fd < 0) {
                throw_io("创建manifest临时文件失败");
            }
            validate_regular_file(fd, temporary);
            write_all(fd, data, LedgerIoPoint::ManifestWrite);
            data_sync(fd, LedgerIoPoint::ManifestDataSync);
            (void)::close(fd);
            fd = -1;
            invoke_fault(LedgerIoPoint::ManifestRename);
            if (::renameat(directory_fd_, temporary.c_str(), directory_fd_,
                           kManifestName) != 0) {
                throw_io("原子替换manifest失败");
            }
            directory_sync();
        } catch (...) {
            if (fd >= 0) {
                (void)::close(fd);
            }
            (void)::unlinkat(directory_fd_, temporary.c_str(), 0);
            throw;
        }
    }

    void initialize_empty() {
        ensure_no_ledger_remnants();
        create_segment(1U);
        Manifest initial;
        store_manifest(initial);
        manifest_ = initial;
    }

    void ensure_no_ledger_remnants() {
        const auto duplicate = ::dup(directory_fd_);
        if (duplicate < 0) {
            throw_io("复制账本目录描述符失败");
        }
        auto* directory = ::fdopendir(duplicate);
        if (directory == nullptr) {
            (void)::close(duplicate);
            throw_io("枚举账本目录失败");
        }
        bool found_remnant = false;
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            const std::string name(entry->d_name);
            std::uint64_t ignored{};
            if (parse_segment_name(name, ignored) ||
                name.compare(0U, 9U, ".compact.") == 0 ||
                name.compare(0U, 14U, ".manifest.tmp.") == 0) {
                found_remnant = true;
                break;
            }
        }
        const auto read_error = errno;
        (void)::closedir(directory);
        if (read_error != 0) {
            errno = read_error;
            throw_io("枚举账本目录项失败");
        }
        if (found_remnant) {
            throw_corrupt("manifest缺失但存在账本遗留文件，禁止初始化空账本");
        }
    }

    void cleanup_unreferenced_files() {
        const auto duplicate = ::dup(directory_fd_);
        if (duplicate < 0) {
            throw_io("复制账本目录描述符失败");
        }
        auto* directory = ::fdopendir(duplicate);
        if (directory == nullptr) {
            (void)::close(duplicate);
            throw_io("枚举账本目录失败");
        }
        std::vector<std::string> stale;
        std::size_t entry_count = 0U;
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            if (++entry_count > 131072U) {
                (void)::closedir(directory);
                throw_corrupt("OperationLedger目录项数量超过上限");
            }
            const std::string name(entry->d_name);
            std::uint64_t number{};
            if (parse_segment_name(name, number)) {
                if (number < manifest_.first_segment ||
                    number > manifest_.active_segment) {
                    stale.push_back(name);
                }
            } else if (name.compare(0U, 9U, ".compact.") == 0 ||
                       name.compare(0U, 14U, ".manifest.tmp.") == 0) {
                stale.push_back(name);
            }
        }
        const auto read_error = errno;
        (void)::closedir(directory);
        if (read_error != 0) {
            errno = read_error;
            throw_io("枚举账本目录项失败");
        }
        bool removed = false;
        for (const auto& name : stale) {
            const auto fd = ::openat(directory_fd_, name.c_str(),
                                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                throw_io("打开未引用账本文件失败");
            }
            try {
                validate_regular_file(fd, name);
            } catch (...) {
                (void)::close(fd);
                throw;
            }
            (void)::close(fd);
            invoke_fault(LedgerIoPoint::SegmentDelete);
            if (::unlinkat(directory_fd_, name.c_str(), 0) != 0) {
                throw_io("清理未引用账本文件失败");
            }
            removed = true;
        }
        if (removed) {
            directory_sync();
        }
    }

    void load_or_initialize() {
        const auto fd = ::openat(directory_fd_, kManifestName,
                                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (errno != ENOENT) {
                throw_io("打开OperationLedger manifest失败");
            }
            initialize_empty();
            return;
        }
        try {
            validate_regular_file(fd, kManifestName);
            struct stat status {};
            if (::fstat(fd, &status) != 0) {
                throw_io("读取manifest长度失败");
            }
            if (status.st_size != static_cast<off_t>(kManifestBytes)) {
                throw_corrupt("OperationLedger manifest长度无效");
            }
            manifest_ = decode_manifest(read_exact_file(fd, kManifestBytes));
        } catch (...) {
            (void)::close(fd);
            throw;
        }
        (void)::close(fd);
        cleanup_unreferenced_files();
        scan_segments();
    }

    std::vector<std::uint8_t> encode_full_payload(
        const OperationRecord& record) const {
        std::vector<std::uint8_t> payload;
        std::uint8_t flags = 0U;
        if (record.requested_value.has_value()) {
            flags |= 0x01U;
        }
        if (record.result.object_id.has_value()) {
            flags |= 0x02U;
        }
        if (record.result.value.has_value()) {
            flags |= 0x04U;
        }
        if (record.requested_frequency_hz.has_value()) flags |= 0x08U;
        if (record.result.frequency_hz.has_value()) flags |= 0x10U;
        if (record.requested_duty.has_value()) flags |= 0x20U;
        if (record.result.duty.has_value()) flags |= 0x40U;
        payload.push_back(static_cast<std::uint8_t>(record.kind));
        payload.push_back(flags);
        put_u16(payload, record.permissions);
        put_u32(payload, record.node_id);
        put_u32(payload, record.scope.resource_id);
        put_array(payload, record.daemon_origin);
        put_array(payload, record.lease_id);
        put_array(payload, record.scope.expected_node_uuid);
        payload.push_back(record.requested_value.value_or(false) ? 1U : 0U);
        payload.push_back(record.result.value.value_or(false) ? 1U : 0U);
        put_u16(payload, 0U);
        put_u32(payload, record.result.object_id.value_or(0U));
        put_u16(payload, record.result.stable_error_code);
        put_u64(payload, record.admission_id);
        put_u32(payload, record.requested_frequency_hz.value_or(0U));
        put_u32(payload, record.result.frequency_hz.value_or(0U));
        put_u16(payload, record.requested_duty.value_or(0U));
        put_u16(payload, record.result.duty.value_or(0U));
        payload.push_back(record.requested_active_low.value_or(false) ? 1U : 0U);
        payload.push_back(record.result.active_low.value_or(false) ? 1U : 0U);
        payload.push_back(record.requested_payload_digest.has_value() ? 1U : 0U);
        payload.push_back(0U);
        put_u16(payload, 0U);
        put_array(payload, record.requested_payload_digest.value_or(OperationDigest{}));
        append_text(payload, record.owner_key_id);
        append_text(payload, record.idempotency_key);
        return payload;
    }

    std::vector<std::uint8_t> encode_tombstone_payload(
        const StoredEntry& entry) const {
        std::vector<std::uint8_t> payload;
        payload.push_back(static_cast<std::uint8_t>(entry.record.kind));
        payload.push_back(0U);
        put_u16(payload, 0U);
        put_array(payload, entry.record.scope.expected_node_uuid);
        put_u32(payload, entry.record.scope.resource_id);
        put_array(payload, entry.owner_hash);
        put_array(payload, entry.business_key_hash);
        return payload;
    }

    std::vector<std::uint8_t> encode_record(
        const OperationRecord& record, DiskRecordType type,
        const OperationDigest& previous_hash,
        const OperationDigest* tombstone_owner = nullptr,
        const OperationDigest* tombstone_business_key = nullptr) const {
        const auto payload = type == DiskRecordType::Tombstone
            ? encode_tombstone_payload(
                  StoredEntry{record, *tombstone_owner,
                              *tombstone_business_key})
            : encode_full_payload(record);
        const auto total = kRecordHeaderBytes + payload.size() +
                           kRecordTrailerBytes;
        if (total > kMaximumRecordBytes ||
            total > std::numeric_limits<std::uint16_t>::max()) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "OperationLedger记录超过1KiB");
        }
        std::vector<std::uint8_t> out;
        out.reserve(total);
        out.insert(out.end(), kRecordMagic, kRecordMagic + 8U);
        put_u16(out, kRecordFormatVersion);
        out.push_back(static_cast<std::uint8_t>(type));
        out.push_back(static_cast<std::uint8_t>(record.state));
        out.push_back(static_cast<std::uint8_t>(record.recovery));
        out.push_back(0U);
        put_u16(out, static_cast<std::uint16_t>(kRecordHeaderBytes));
        put_u16(out, static_cast<std::uint16_t>(total));
        put_u64(out, record.sequence);
        put_u64(out, record.recorded_at_ms);
        put_array(out, record.operation_id);
        put_array(out, record.request_digest);
        put_array(out, previous_hash);
        put_u16(out, static_cast<std::uint16_t>(payload.size()));
        put_u32(out, 0U);
        if (out.size() != kRecordHeaderBytes) {
            throw OperationLedgerException(
                OperationLedgerError::InvalidRequest,
                "内部记录头长度错误");
        }
        out.insert(out.end(), payload.begin(), payload.end());
        put_u32(out, protocol::crc32(out.data(), out.size()));
        put_u64(out, kCommitMarker);
        return out;
    }

    DecodedRecord decode_record(const std::vector<std::uint8_t>& bytes,
                                std::size_t offset) const {
        if (bytes.size() - offset < kRecordHeaderBytes) {
            throw_corrupt("OperationLedger中段记录头截断");
        }
        const auto* data = bytes.data() + offset;
        const auto type_value = data[10U];
        const auto format_version = get_u16(data + 8U);
        if (std::memcmp(data, kRecordMagic, 8U) != 0 ||
            (format_version < 1U || format_version > kRecordFormatVersion) ||
            (type_value < 1U || type_value > 3U) ||
            !valid_state(data[11U]) || !valid_recovery(data[12U]) ||
            data[13U] != 0U || get_u16(data + 14U) != kRecordHeaderBytes ||
            get_u32(data + 132U) != 0U) {
            throw_corrupt("OperationLedger记录头字段无效");
        }
        const auto total = static_cast<std::size_t>(get_u16(data + 16U));
        const auto payload_size =
            static_cast<std::size_t>(get_u16(data + 130U));
        if (total < kRecordHeaderBytes + kRecordTrailerBytes ||
            total > kMaximumRecordBytes ||
            payload_size != total - kRecordHeaderBytes - kRecordTrailerBytes ||
            total > bytes.size() - offset) {
            throw_corrupt("OperationLedger记录长度无效");
        }
        if (get_u64(data + total - 8U) != kCommitMarker ||
            get_u32(data + total - 12U) !=
                protocol::crc32(data, total - 12U)) {
            throw_corrupt("OperationLedger记录CRC或提交标记无效");
        }
        DecodedRecord decoded;
        decoded.type = static_cast<DiskRecordType>(type_value);
        decoded.bytes = total;
        decoded.record.sequence = get_u64(data + 18U);
        decoded.record.recorded_at_ms = get_u64(data + 26U);
        decoded.record.operation_id = get_array<32U>(data + 34U);
        decoded.record.request_digest = get_array<32U>(data + 66U);
        decoded.previous_hash = get_array<32U>(data + 98U);
        decoded.record.state = static_cast<OperationState>(data[11U]);
        decoded.record.recovery = static_cast<OperationRecovery>(data[12U]);
        decoded.record.full_record =
            decoded.type != DiskRecordType::Tombstone;
        const auto* payload = data + kRecordHeaderBytes;
        if (decoded.type == DiskRecordType::Tombstone) {
            if (payload_size != 88U || !valid_kind(payload[0]) ||
                payload[1] != 0U || get_u16(payload + 2U) != 0U) {
                throw_corrupt("OperationLedger墓碑载荷无效");
            }
            decoded.record.kind = static_cast<OperationKind>(payload[0]);
            decoded.record.scope.expected_node_uuid =
                get_array<16U>(payload + 4U);
            decoded.record.scope.resource_id = get_u32(payload + 20U);
            decoded.owner_hash = get_array<32U>(payload + 24U);
            decoded.business_key_hash = get_array<32U>(payload + 56U);
        } else {
            decode_full_payload(decoded, payload, payload_size,
                                format_version);
        }
        if (format_version == 1U &&
            decoded.record.kind == OperationKind::RuntimePwmStop) {
            throw_corrupt("OperationLedger v1记录包含未定义的PWM停止类型");
        }
        if (!valid_state_recovery(decoded.record.kind, decoded.record.state,
                                  decoded.record.recovery)) {
            throw_corrupt("OperationLedger状态与恢复证据组合无效");
        }
        if (decoded.record.full_record) {
            const auto& result = decoded.record.result;
            const auto result_valid =
                decoded.record.state == OperationState::Pending
                ? !result.object_id.has_value() && !result.value.has_value() &&
                      !result.frequency_hz.has_value() && !result.duty.has_value() &&
                      !result.active_low.has_value() &&
                      result.stable_error_code == 0U
                : valid_terminal_result(decoded.record,
                                        decoded.record.state, result);
            if (!result_valid) {
                throw_corrupt("OperationLedger结果字段组合无效");
            }
        }
        decoded.record_hash = sha256(data, total);
        return decoded;
    }

    void decode_full_payload(DecodedRecord& decoded, const std::uint8_t* data,
                             std::size_t size, std::uint16_t format_version) const {
        const std::size_t fixed = format_version == 1U ? 78U :
                                  (format_version == 2U ? 92U : 128U);
        if (size < fixed || !valid_kind(data[0]) ||
            (format_version == 1U && (data[1] & 0xf8U) != 0U) ||
            (format_version >= 2U && (data[1] & 0x80U) != 0U) ||
            get_u16(data + 62U) != 0U) {
            throw_corrupt("OperationLedger完整载荷固定字段无效");
        }
        auto& record = decoded.record;
        record.kind = static_cast<OperationKind>(data[0]);
        const auto flags = data[1];
        record.permissions = get_u16(data + 2U);
        record.node_id = get_u32(data + 4U);
        record.scope.resource_id = get_u32(data + 8U);
        record.daemon_origin = get_array<16U>(data + 12U);
        record.lease_id = get_array<16U>(data + 28U);
        record.scope.expected_node_uuid = get_array<16U>(data + 44U);
        if (data[60U] > 1U || data[61U] > 1U) {
            throw_corrupt("OperationLedger布尔字段无效");
        }
        if ((flags & 0x01U) != 0U) {
            record.requested_value = data[60U] != 0U;
        } else if (data[60U] != 0U) {
            throw_corrupt("OperationLedger缺失请求值却包含非零数据");
        }
        const auto object = get_u32(data + 64U);
        if ((flags & 0x02U) != 0U) {
            record.result.object_id = object;
        } else if (object != 0U) {
            throw_corrupt("OperationLedger缺失对象ID却包含非零数据");
        }
        if ((flags & 0x04U) != 0U) {
            record.result.value = data[61U] != 0U;
        } else if (data[61U] != 0U) {
            throw_corrupt("OperationLedger缺失结果值却包含非零数据");
        }
        record.result.stable_error_code = get_u16(data + 68U);
        record.admission_id = get_u64(data + 70U);
        if (format_version != 1U) {
        const auto requested_frequency = get_u32(data + 78U);
        const auto result_frequency = get_u32(data + 82U);
        const auto requested_duty = get_u16(data + 86U);
        const auto result_duty = get_u16(data + 88U);
        if (data[90U] > 1U || data[91U] > 1U) throw_corrupt("OperationLedger PWM布尔字段无效");
        if ((flags & 0x08U) != 0U) record.requested_frequency_hz = requested_frequency;
        else if (requested_frequency != 0U) throw_corrupt("OperationLedger缺失PWM请求频率");
        if ((flags & 0x10U) != 0U) record.result.frequency_hz = result_frequency;
        else if (result_frequency != 0U) throw_corrupt("OperationLedger缺失PWM结果频率");
        if ((flags & 0x20U) != 0U) record.requested_duty = requested_duty;
        else if (requested_duty != 0U) throw_corrupt("OperationLedger缺失PWM请求占空比");
        if ((flags & 0x40U) != 0U) record.result.duty = result_duty;
        else if (result_duty != 0U) throw_corrupt("OperationLedger缺失PWM结果占空比");
        if (record.kind == OperationKind::RuntimePwmConfigure) {
            record.requested_active_low = data[90U] != 0U;
            if (record.state == OperationState::Committed) {
                record.result.active_low = data[91U] != 0U;
            }
        } else if (data[90U] != 0U || data[91U] != 0U) {
            throw_corrupt("OperationLedger非PWM记录包含极性");
        }
        if (format_version >= 3U) {
            if (data[92U] > 1U || data[93U] != 0U ||
                get_u16(data + 94U) != 0U) {
                throw_corrupt("OperationLedger定时位流摘要标志无效");
            }
            const auto digest = get_array<32U>(data + 96U);
            if (data[92U] != 0U) {
                if (std::all_of(digest.begin(), digest.end(),
                                [](std::uint8_t value) { return value == 0U; }))
                    throw_corrupt("OperationLedger定时位流摘要为零");
                record.requested_payload_digest = digest;
            } else if (!std::all_of(
                           digest.begin(), digest.end(),
                           [](std::uint8_t value) { return value == 0U; })) {
                throw_corrupt("OperationLedger缺失定时位流摘要标志");
            }
        }
        }
        std::size_t cursor = fixed;
        if (cursor + 2U > size) {
            throw_corrupt("OperationLedger owner长度缺失");
        }
        const auto owner_size = get_u16(data + cursor);
        cursor += 2U;
        if (owner_size == 0U || owner_size > kOperationLedgerMaximumTextBytes ||
            cursor + owner_size + 2U > size) {
            throw_corrupt("OperationLedger owner长度无效");
        }
        record.owner_key_id.assign(
            reinterpret_cast<const char*>(data + cursor), owner_size);
        cursor += owner_size;
        const auto idempotency_size = get_u16(data + cursor);
        cursor += 2U;
        if (idempotency_size == 0U ||
            idempotency_size > kOperationLedgerMaximumTextBytes ||
            cursor + idempotency_size != size) {
            throw_corrupt("OperationLedger幂等键长度无效");
        }
        record.idempotency_key.assign(
            reinterpret_cast<const char*>(data + cursor), idempotency_size);
        if (!valid_text(record.owner_key_id) ||
            !valid_text(record.idempotency_key)) {
            throw_corrupt("OperationLedger文本字段无效");
        }
        decoded.owner_hash = sha256(
            reinterpret_cast<const std::uint8_t*>(record.owner_key_id.data()),
            record.owner_key_id.size());
        decoded.business_key_hash = business_key_hash(
            record.kind, record.lease_id, record.owner_key_id,
            record.idempotency_key);
        validate_decoded_identity(record);
    }

    void validate_decoded_identity(const OperationRecord& record) const {
        try {
            if (record.kind == OperationKind::RuntimeGpioWrite) {
                if (!record.requested_value.has_value() ||
                    record.admission_id != 0U) {
                    throw_corrupt("GPIO写记录缺少目标值");
                }
                RuntimeGpioWriteOperation operation{
                    record.daemon_origin, record.lease_id,
                    record.scope.expected_node_uuid, record.owner_key_id,
                    record.idempotency_key, record.permissions, record.node_id,
                    record.scope.resource_id, *record.requested_value};
                if (OperationLedger::derive_operation_id(operation) !=
                        record.operation_id ||
                    OperationLedger::derive_request_digest(operation) !=
                        record.request_digest) {
                    throw_corrupt("GPIO写记录身份摘要不匹配");
                }
            } else if (record.kind == OperationKind::RuntimePwmConfigure) {
                if (!record.requested_frequency_hz.has_value() ||
                    !record.requested_duty.has_value() ||
                    !record.requested_active_low.has_value() ||
                    record.requested_value.has_value() || record.admission_id != 0U) {
                    throw_corrupt("PWM配置记录字段组合无效");
                }
                RuntimePwmConfigureOperation operation{
                    record.daemon_origin, record.lease_id,
                    record.scope.expected_node_uuid, record.owner_key_id,
                    record.idempotency_key, record.permissions, record.node_id,
                    record.scope.resource_id, *record.requested_frequency_hz,
                    *record.requested_duty, *record.requested_active_low};
                if (OperationLedger::derive_operation_id(operation) != record.operation_id ||
                    OperationLedger::derive_request_digest(operation) != record.request_digest) {
                    throw_corrupt("PWM配置记录身份摘要不匹配");
                }
            } else if (record.kind == OperationKind::RuntimePwmStop) {
                if (record.requested_value.has_value() ||
                    record.requested_frequency_hz.has_value() ||
                    record.requested_duty.has_value() ||
                    record.requested_active_low.has_value() ||
                    record.admission_id != 0U) {
                    throw_corrupt("PWM停止记录字段组合无效");
                }
                RuntimePwmStopOperation operation{
                    record.daemon_origin, record.lease_id,
                    record.scope.expected_node_uuid, record.owner_key_id,
                    record.idempotency_key, record.permissions, record.node_id,
                    record.scope.resource_id};
                if (OperationLedger::derive_operation_id(operation) != record.operation_id ||
                    OperationLedger::derive_request_digest(operation) != record.request_digest) {
                    throw_corrupt("PWM停止记录身份摘要不匹配");
                }
            } else if (record.kind == OperationKind::RuntimeTimedBitstreamConfigure ||
                       record.kind == OperationKind::RuntimeTimedBitstreamFrame) {
                if (!record.requested_payload_digest.has_value() ||
                    record.requested_value.has_value() ||
                    record.requested_frequency_hz.has_value() ||
                    record.requested_duty.has_value() ||
                    record.requested_active_low.has_value() ||
                    record.admission_id != 0U) {
                    throw_corrupt("定时位流记录字段组合无效");
                }
                OperationDigest operation_id;
                std::uint8_t domain;
                if (record.kind == OperationKind::RuntimeTimedBitstreamConfigure) {
                    RuntimeTimedBitstreamConfigureOperation operation{
                        record.daemon_origin, record.lease_id,
                        record.scope.expected_node_uuid, record.owner_key_id,
                        record.idempotency_key, record.permissions, record.node_id,
                        record.scope.resource_id, 0U, 0U, 0U, 0U};
                    operation_id = OperationLedger::derive_operation_id(operation);
                    domain = 0x85U;
                } else {
                    RuntimeTimedBitstreamFrameOperation operation{
                        record.daemon_origin, record.lease_id,
                        record.scope.expected_node_uuid, record.owner_key_id,
                        record.idempotency_key, record.permissions, record.node_id,
                        record.scope.resource_id, 0U, {}};
                    operation_id = OperationLedger::derive_operation_id(operation);
                    domain = 0x86U;
                }
                if (operation_id != record.operation_id ||
                    timed_request_digest(operation_id, record.daemon_origin,
                        record.lease_id, record.scope.expected_node_uuid,
                        record.owner_key_id, record.permissions, record.node_id,
                        record.scope.resource_id, domain,
                        &*record.requested_payload_digest) != record.request_digest) {
                    throw_corrupt("定时位流记录身份摘要不匹配");
                }
            } else if (record.kind == OperationKind::RuntimeTimedBitstreamStop) {
                if (record.requested_payload_digest.has_value() ||
                    record.requested_value.has_value() || record.admission_id != 0U)
                    throw_corrupt("定时位流停止记录字段组合无效");
                RuntimeTimedBitstreamStopOperation operation{
                    record.daemon_origin, record.lease_id,
                    record.scope.expected_node_uuid, record.owner_key_id,
                    record.idempotency_key, record.permissions, record.node_id,
                    record.scope.resource_id};
                if (OperationLedger::derive_operation_id(operation) != record.operation_id ||
                    OperationLedger::derive_request_digest(operation) != record.request_digest)
                    throw_corrupt("定时位流停止记录身份摘要不匹配");
            } else {
                if (record.requested_value.has_value() ||
                    record.idempotency_key != "release:v1") {
                    throw_corrupt("Release记录字段组合无效");
                }
                RuntimeControlReleaseOperation operation{
                    record.daemon_origin, record.lease_id,
                    record.owner_key_id};
                ServerResolvedLease lease{
                    record.scope.expected_node_uuid, record.node_id,
                    record.scope.resource_id, record.permissions,
                    record.admission_id};
                if (OperationLedger::derive_operation_id(operation) !=
                        record.operation_id ||
                    OperationLedger::derive_request_digest(operation, lease) !=
                        record.request_digest) {
                    throw_corrupt("Release记录身份摘要不匹配");
                }
            }
        } catch (const OperationLedgerException&) {
            throw_corrupt("OperationLedger记录包含非法身份字段");
        }
    }

    void scan_segments() {
        OperationDigest expected_hash = manifest_.first_previous_hash;
        std::uint64_t expected_sequence = manifest_.first_sequence;
        std::uint64_t total_bytes = 0U;
        for (auto number = manifest_.first_segment;
             number <= manifest_.active_segment; ++number) {
            const auto fd = open_segment(number, O_RDWR);
            std::uint64_t logical{};
            std::vector<std::uint8_t> data;
            try {
                struct stat status {};
                if (::fstat(fd, &status) != 0) {
                    throw_io("读取账本分段长度失败");
                }
                const auto actual =
                    static_cast<std::uint64_t>(status.st_size);
                logical = number == manifest_.active_segment
                    ? manifest_.active_size : actual;
                if (actual < logical ||
                    logical > options_.maximum_segment_bytes) {
                    throw_corrupt("OperationLedger分段长度与manifest不一致");
                }
                if (number == manifest_.active_segment && actual > logical) {
                    if (::ftruncate(fd, static_cast<off_t>(logical)) != 0) {
                        throw_io("回退未提交账本尾部失败");
                    }
                    data_sync(fd, LedgerIoPoint::RecordDataSync);
                }
                data = read_exact_file(
                    fd, static_cast<std::size_t>(logical));
            } catch (...) {
                (void)::close(fd);
                throw;
            }
            (void)::close(fd);
            std::size_t offset = 0U;
            while (offset < data.size()) {
                auto decoded = decode_record(data, offset);
                if (decoded.record.sequence != expected_sequence ||
                    decoded.previous_hash != expected_hash) {
                    throw_corrupt("OperationLedger序列或摘要链断裂");
                }
                apply_decoded(decoded);
                expected_hash = decoded.record_hash;
                ++expected_sequence;
                offset += decoded.bytes;
            }
            total_bytes += logical;
            if (number == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
        }
        if (total_bytes != manifest_.logical_total_bytes ||
            expected_hash != manifest_.last_record_hash ||
            expected_sequence != manifest_.last_sequence + 1U ||
            entries_.size() != manifest_.operation_count) {
            throw_corrupt("OperationLedger与manifest最终锚点不一致");
        }
        rebuild_blocked_scopes();
    }

    void apply_decoded(const DecodedRecord& decoded) {
        const auto key = digest_key(decoded.record.operation_id);
        const auto found = entries_.find(key);
        if (decoded.type == DiskRecordType::Full) {
            if (found == entries_.end()) {
                if (decoded.record.state != OperationState::Pending) {
                    throw_corrupt("普通账本操作未从pending开始");
                }
            } else {
                const auto& previous = found->second.record;
                const auto pending_to_terminal =
                    previous.state == OperationState::Pending &&
                    decoded.record.state != OperationState::Pending;
                const auto unknown_evidence_update =
                    previous.state == OperationState::Unknown &&
                    decoded.record.state == OperationState::Unknown &&
                    recovery_advances(previous.recovery,
                                      decoded.record.recovery) &&
                    previous.result.object_id ==
                        decoded.record.result.object_id &&
                    previous.result.value == decoded.record.result.value &&
                    previous.result.stable_error_code ==
                        decoded.record.result.stable_error_code;
                if (!previous.full_record ||
                    previous.request_digest != decoded.record.request_digest ||
                    (!pending_to_terminal && !unknown_evidence_update)) {
                    throw_corrupt("OperationLedger出现冲突或矛盾状态转换");
                }
            }
        } else if (found != entries_.end()) {
            throw_corrupt("压缩分段包含重复operation_id");
        }
        const auto business_key = digest_key(decoded.business_key_hash);
        const auto business_found = business_index_.find(business_key);
        if (business_found != business_index_.end() &&
            business_found->second != key) {
            throw_corrupt("OperationLedger业务幂等键映射到多个操作");
        }
        entries_[key] = {decoded.record, decoded.owner_hash,
                         decoded.business_key_hash};
        business_index_[business_key] = key;
    }

    void recover_pending() {
        std::vector<std::string> pending;
        for (const auto& item : entries_) {
            if (item.second.record.full_record &&
                item.second.record.state == OperationState::Pending) {
                pending.push_back(item.first);
            }
        }
        for (const auto& key : pending) {
            auto recovered = entries_.at(key).record;
            recovered.state = OperationState::Unknown;
            recovered.recovery = OperationRecovery::ScopeBlocked;
            recovered.result.stable_error_code =
                kOperationLedgerPersistenceErrorCode;
            append_record(recovered, DiskRecordType::Full, nullptr, nullptr,
                          false);
            entries_.at(key).record = std::move(recovered);
        }
        rebuild_blocked_scopes();
    }

    OperationBeginResult begin(OperationRecord record) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_available();
        const auto key = digest_key(record.operation_id);
        const auto business_hash = business_key_hash(
            record.kind, record.lease_id, record.owner_key_id,
            record.idempotency_key);
        const auto business_key = digest_key(business_hash);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second.record.request_digest != record.request_digest) {
                throw OperationLedgerException(
                    OperationLedgerError::IdempotencyConflict,
                    "相同operation_id绑定了不同请求摘要");
            }
            if (!found->second.record.full_record) {
                throw OperationLedgerException(
                    OperationLedgerError::InvalidTransition,
                    "该operation_id仅剩墓碑，禁止再次执行");
            }
            const auto disposition =
                found->second.record.state == OperationState::Pending
                ? OperationBeginDisposition::ExistingPending
                : OperationBeginDisposition::ExistingTerminal;
            return {disposition, found->second.record};
        }
        const auto prior_business = business_index_.find(business_key);
        if (prior_business != business_index_.end()) {
            const auto& prior = entries_.at(prior_business->second).record;
            if (prior.request_digest != record.request_digest) {
                throw OperationLedgerException(
                    OperationLedgerError::IdempotencyConflict,
                    "owner、操作类型与业务幂等键已绑定不同请求");
            }
            throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict,
                "业务幂等索引与operation_id不一致");
        }
        ensure_begin_capacity();
        record.state = OperationState::Pending;
        record.recovery = OperationRecovery::None;
        record.full_record = true;
        StoredEntry entry{
            record,
            sha256(reinterpret_cast<const std::uint8_t*>(
                       record.owner_key_id.data()),
                   record.owner_key_id.size()),
            business_hash};
        entries_.reserve(entries_.size() + 1U);
        business_index_.reserve(business_index_.size() + 1U);
        blocked_scopes_.reserve(blocked_scopes_.size() + 1U);
        blocking_operations_.reserve(blocking_operations_.size() + 1U);
        const auto inserted = entries_.emplace(key, std::move(entry));
        try {
            business_index_.emplace(business_key, key);
        } catch (...) {
            entries_.erase(inserted.first);
            throw;
        }
        try {
            append_record(record, DiskRecordType::Full, nullptr, nullptr,
                          true);
        } catch (...) {
            business_index_.erase(business_key);
            entries_.erase(inserted.first);
            throw;
        }
        inserted.first->second.record.sequence = record.sequence;
        inserted.first->second.record.recorded_at_ms = record.recorded_at_ms;
        try {
            apply_scope(inserted.first->second.record);
        } catch (...) {
            set_failure("OperationLedger pending作用域索引发布失败");
            throw;
        }
        return {OperationBeginDisposition::StartedDurablePending,
                inserted.first->second.record};
    }

    void ensure_begin_capacity() {
        const auto pending = static_cast<std::uint64_t>(std::count_if(
            entries_.begin(), entries_.end(), [](const auto& item) {
                return item.second.record.full_record &&
                       item.second.record.state == OperationState::Pending;
            }));
        const auto needs_compaction =
            entries_.size() >= options_.maximum_operations ||
            manifest_.logical_total_bytes +
                    (pending + 2U) * kMaximumRecordBytes >
                options_.maximum_total_bytes;
        if (needs_compaction) {
            compact_locked(options_.wall_clock_ms());
        }
        const auto pending_after = static_cast<std::uint64_t>(std::count_if(
            entries_.begin(), entries_.end(), [](const auto& item) {
                return item.second.record.full_record &&
                       item.second.record.state == OperationState::Pending;
            }));
        if (entries_.size() >= options_.maximum_operations ||
            manifest_.logical_total_bytes +
                    (pending_after + 2U) * kMaximumRecordBytes >
                options_.maximum_total_bytes) {
            throw OperationLedgerException(
                OperationLedgerError::CapacityExceeded,
                "OperationLedger容量已满，未写pending且未授权I/O");
        }
    }

    void append_record(OperationRecord& record, DiskRecordType type,
                       const OperationDigest* owner_hash = nullptr,
                       const OperationDigest* business_hash = nullptr,
                       bool new_operation = false) {
        record.sequence = manifest_.last_sequence + 1U;
        record.recorded_at_ms = options_.wall_clock_ms();
        auto bytes = encode_record(record, type, manifest_.last_record_hash,
                                   owner_hash, business_hash);
        if (manifest_.active_size != 0U &&
            manifest_.active_size + bytes.size() >
                options_.maximum_segment_bytes) {
            rotate_segment();
            bytes = encode_record(record, type, manifest_.last_record_hash,
                                  owner_hash, business_hash);
        }
        const auto fd = open_segment(manifest_.active_segment,
                                     O_WRONLY | O_APPEND);
        try {
            struct stat status {};
            if (::fstat(fd, &status) != 0 ||
                static_cast<std::uint64_t>(status.st_size) !=
                    manifest_.active_size) {
                throw_corrupt("活动分段长度在追加前发生变化");
            }
            write_all(fd, bytes, LedgerIoPoint::RecordWrite);
            if (type == DiskRecordType::Full &&
                record.state != OperationState::Pending) {
                // 测试故障点位于 terminal 记录完整追加之后、成为 durable
                // 之前。默认未配置 fault_hook 时不改变生产路径。
                invoke_fault(LedgerIoPoint::TerminalRecordDataSync);
            }
            data_sync(fd, LedgerIoPoint::RecordDataSync);
        } catch (...) {
            (void)::close(fd);
            set_failure("OperationLedger记录写入或同步失败");
            throw;
        }
        (void)::close(fd);
        Manifest next = manifest_;
        next.generation += 1U;
        next.active_size += bytes.size();
        next.logical_total_bytes += bytes.size();
        next.last_sequence = record.sequence;
        next.last_record_hash = sha256(bytes);
        if (new_operation) {
            next.operation_count += 1U;
        }
        try {
            store_manifest(next);
        } catch (...) {
            set_failure("OperationLedger manifest提交失败");
            throw;
        }
        manifest_ = next;
    }

    void rotate_segment() {
        if (manifest_.active_segment ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw OperationLedgerException(
                OperationLedgerError::CapacityExceeded,
                "OperationLedger分段编号耗尽");
        }
        const auto next_number = manifest_.active_segment + 1U;
        try {
            create_segment(next_number);
            Manifest next = manifest_;
            next.generation += 1U;
            next.active_segment = next_number;
            next.active_size = 0U;
            store_manifest(next);
            manifest_ = next;
        } catch (...) {
            set_failure("OperationLedger分段轮转失败");
            throw;
        }
    }

    bool scope_is_blocked_by(const OperationRecord& record) const noexcept {
        if (record.kind == OperationKind::RuntimeGpioWrite) {
            if (record.state == OperationState::Rejected &&
                record.recovery == OperationRecovery::NotSent) {
                return false;
            }
            return record.recovery != OperationRecovery::SafeClosed &&
                   record.recovery !=
                       OperationRecovery::NodeRebootConfirmed;
        }
        return record.state == OperationState::Pending ||
               record.state == OperationState::Unknown ||
               record.recovery == OperationRecovery::ScopeBlocked ||
               record.recovery == OperationRecovery::AwaitingReboot;
    }

    void apply_scope(const OperationRecord& record) {
        const auto key = scope_key(record.scope);
        if (record.state == OperationState::Pending) {
            blocked_scopes_[key] = record.scope;
            if (blocking_operations_.find(key) ==
                blocking_operations_.end()) {
                blocking_operations_[key] = digest_key(record.operation_id);
            }
            return;
        }
        if ((record.kind == OperationKind::RuntimeControlRelease &&
             record.state == OperationState::Committed) ||
            record.recovery == OperationRecovery::SafeClosed ||
            record.recovery == OperationRecovery::NodeRebootConfirmed) {
            blocked_scopes_.erase(key);
            blocking_operations_.erase(key);
        } else if (scope_is_blocked_by(record)) {
            blocked_scopes_[key] = record.scope;
            blocking_operations_[key] = digest_key(record.operation_id);
        }
    }

    void rebuild_blocked_scopes() {
        blocked_scopes_.clear();
        blocking_operations_.clear();
        std::vector<const OperationRecord*> ordered;
        ordered.reserve(entries_.size());
        for (const auto& item : entries_) {
            ordered.push_back(&item.second.record);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* left,
                                                     const auto* right) {
            return left->sequence < right->sequence;
        });
        for (const auto* record : ordered) {
            apply_scope(*record);
        }
    }

    bool elapsed_at_least(std::uint64_t now, std::uint64_t then,
                          std::uint64_t duration) const noexcept {
        return now >= then && now - then >= duration;
    }

    void compact_locked(std::uint64_t now) {
        struct Retained {
            StoredEntry entry;
            DiskRecordType type;
        };
        std::vector<Retained> retained;
        retained.reserve(entries_.size());
        bool changed = false;
        for (const auto& item : entries_) {
            auto entry = item.second;
            const auto scope = scope_key(entry.record.scope);
            const auto blocking = blocking_operations_.find(scope);
            const auto blocked = blocking != blocking_operations_.end() &&
                blocking->second == item.first;
            if (!entry.record.full_record) {
                if (!blocked && elapsed_at_least(
                        now, entry.record.recorded_at_ms,
                        options_.tombstone_retention_ms)) {
                    changed = true;
                    continue;
                }
                retained.push_back({entry, DiskRecordType::Tombstone});
                continue;
            }
            const auto unresolved =
                entry.record.state == OperationState::Pending ||
                entry.record.state == OperationState::Unknown || blocked;
            if (!unresolved && elapsed_at_least(
                    now, entry.record.recorded_at_ms,
                    options_.full_terminal_retention_ms)) {
                entry.record.full_record = false;
                entry.record.recorded_at_ms = now;
                entry.record.owner_key_id.clear();
                entry.record.idempotency_key.clear();
                entry.record.daemon_origin = {};
                entry.record.lease_id = {};
                entry.record.requested_value.reset();
                entry.record.result = {};
                retained.push_back({entry, DiskRecordType::Tombstone});
                changed = true;
            } else {
                retained.push_back({entry, DiskRecordType::CompactedFull});
            }
        }
        if (!changed) {
            return;
        }
        std::sort(retained.begin(), retained.end(), [](const Retained& left,
                                                       const Retained& right) {
            return left.entry.record.sequence < right.entry.record.sequence;
        });
        std::unordered_map<std::string, StoredEntry> next_entries;
        std::unordered_map<std::string, std::string> next_business_index;
        next_entries.reserve(retained.size());
        next_business_index.reserve(retained.size());
        for (const auto& item : retained) {
            const auto operation_key =
                digest_key(item.entry.record.operation_id);
            const auto business_key = digest_key(item.entry.business_key_hash);
            if (!next_entries.emplace(operation_key, item.entry).second ||
                !next_business_index.emplace(
                    business_key, operation_key).second) {
                throw OperationLedgerException(
                    OperationLedgerError::Corrupt,
                    "压缩前幂等索引出现重复项");
            }
        }
        if (manifest_.active_segment ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw OperationLedgerException(
                OperationLedgerError::CapacityExceeded,
                "OperationLedger分段编号耗尽");
        }
        struct NewSegment {
            std::uint64_t number{};
            std::string temporary;
            std::uint64_t size{};
            bool published{};
        };
        std::vector<NewSegment> new_segments;
        int fd = -1;
        std::uint64_t sequence = manifest_.last_sequence;
        auto previous = manifest_.last_record_hash;
        std::uint64_t total_size = 0U;

        const auto start_segment = [&] {
            if (manifest_.active_segment + new_segments.size() ==
                std::numeric_limits<std::uint64_t>::max()) {
                throw OperationLedgerException(
                    OperationLedgerError::CapacityExceeded,
                    "OperationLedger压缩分段编号耗尽");
            }
            NewSegment segment;
            segment.number = manifest_.active_segment +
                             new_segments.size() + 1U;
            segment.temporary = ".compact." + std::to_string(::getpid()) +
                "." + std::to_string(temp_counter_.fetch_add(1U));
            invoke_fault(LedgerIoPoint::SegmentCreate);
            fd = ::openat(directory_fd_, segment.temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          0600);
            if (fd < 0) {
                throw_io("创建压缩分段失败");
            }
            validate_regular_file(fd, segment.temporary);
            new_segments.push_back(std::move(segment));
        };

        const auto finish_segment = [&] {
            if (fd < 0) {
                return;
            }
            data_sync(fd, LedgerIoPoint::RecordDataSync);
            (void)::close(fd);
            fd = -1;
        };

        try {
            start_segment();
            for (auto& item : retained) {
                item.entry.record.sequence = ++sequence;
                next_entries.at(digest_key(
                    item.entry.record.operation_id)).record.sequence =
                        sequence;
                auto bytes = encode_record(
                    item.entry.record, item.type, previous,
                    item.type == DiskRecordType::Tombstone
                        ? &item.entry.owner_hash : nullptr,
                    item.type == DiskRecordType::Tombstone
                        ? &item.entry.business_key_hash : nullptr);
                if (new_segments.back().size != 0U &&
                    new_segments.back().size + bytes.size() >
                        options_.maximum_segment_bytes) {
                    finish_segment();
                    start_segment();
                }
                write_all(fd, bytes, LedgerIoPoint::RecordWrite);
                new_segments.back().size += bytes.size();
                total_size += bytes.size();
                previous = sha256(bytes);
            }
            finish_segment();
            for (auto& segment : new_segments) {
                const auto final_name = segment_name(segment.number);
                invoke_fault(LedgerIoPoint::SegmentPublish);
                if (::renameat(directory_fd_, segment.temporary.c_str(),
                               directory_fd_, final_name.c_str()) != 0) {
                    throw_io("发布压缩分段失败");
                }
                segment.published = true;
            }
            directory_sync();
        } catch (...) {
            if (fd >= 0) {
                (void)::close(fd);
            }
            for (const auto& segment : new_segments) {
                const auto& name = segment.published
                    ? segment_name(segment.number) : segment.temporary;
                (void)::unlinkat(directory_fd_, name.c_str(), 0);
            }
            set_failure("OperationLedger压缩分段失败");
            throw;
        }
        Manifest next = manifest_;
        next.generation += 1U;
        next.first_segment = new_segments.front().number;
        next.active_segment = new_segments.back().number;
        next.active_size = new_segments.back().size;
        next.first_sequence = manifest_.last_sequence + 1U;
        next.last_sequence = sequence;
        next.logical_total_bytes = total_size;
        next.operation_count = retained.size();
        next.first_previous_hash = manifest_.last_record_hash;
        next.last_record_hash = previous;
        try {
            store_manifest(next);
        } catch (...) {
            set_failure("OperationLedger压缩manifest提交失败");
            throw;
        }
        const auto old_first = manifest_.first_segment;
        const auto old_active = manifest_.active_segment;
        manifest_ = next;
        entries_.swap(next_entries);
        business_index_.swap(next_business_index);
        try {
            rebuild_blocked_scopes();
        } catch (...) {
            set_failure("OperationLedger压缩后作用域索引重建失败");
            throw;
        }
        try {
            for (auto number = old_first; number <= old_active; ++number) {
                invoke_fault(LedgerIoPoint::SegmentDelete);
                const auto name = segment_name(number);
                if (::unlinkat(directory_fd_, name.c_str(), 0) != 0 &&
                    errno != ENOENT) {
                    throw_io("删除旧账本分段失败");
                }
                if (number == std::numeric_limits<std::uint64_t>::max()) {
                    break;
                }
            }
            directory_sync();
        } catch (...) {
            set_failure("OperationLedger旧分段清理失败");
            throw;
        }
    }

    OperationLedgerOptions options_;
    int directory_fd_{-1};
    int lock_fd_{-1};
    mutable std::mutex mutex_;
    mutable std::mutex failure_mutex_;
    std::atomic<bool> available_{false};
    std::string failure_reason_;
    Manifest manifest_;
    std::unordered_map<std::string, StoredEntry> entries_;
    std::unordered_map<std::string, std::string> business_index_;
    std::unordered_map<std::string, OperationScope> blocked_scopes_;
    std::unordered_map<std::string, std::string> blocking_operations_;
    std::atomic<std::uint64_t> temp_counter_{1U};
};

OperationDigest OperationLedger::derive_operation_id(
    const RuntimeGpioWriteOperation& operation) {
    validate_common(operation.daemon_origin, operation.lease_id,
                    operation.owner_key_id);
    if (!valid_text(operation.idempotency_key)) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "GPIO幂等键无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(
        static_cast<std::uint8_t>(OperationKind::RuntimeGpioWrite));
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    append_text(canonical, operation.idempotency_key);
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_operation_id(
    const RuntimeControlReleaseOperation& operation) {
    validate_common(operation.daemon_origin, operation.lease_id,
                    operation.owner_key_id);
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(
        static_cast<std::uint8_t>(OperationKind::RuntimeControlRelease));
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    append_text(canonical, "release:v1");
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_operation_id(
    const RuntimePwmConfigureOperation& operation) {
    validate_common(operation.daemon_origin, operation.lease_id,
                    operation.owner_key_id);
    if (!valid_text(operation.idempotency_key)) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "PWM幂等键无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(static_cast<std::uint8_t>(OperationKind::RuntimePwmConfigure));
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    append_text(canonical, operation.idempotency_key);
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_operation_id(
    const RuntimePwmStopOperation& operation) {
    validate_common(operation.daemon_origin, operation.lease_id,
                    operation.owner_key_id);
    if (!valid_text(operation.idempotency_key)) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "PWM停止幂等键无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(static_cast<std::uint8_t>(OperationKind::RuntimePwmStop));
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    append_text(canonical, operation.idempotency_key);
    return sha256(canonical);
}

#define DEFINE_TIMED_OPERATION_ID(TYPE, KIND, LABEL)                         \
OperationDigest OperationLedger::derive_operation_id(const TYPE& operation) {\
    validate_common(operation.daemon_origin, operation.lease_id,             \
                    operation.owner_key_id);                                 \
    if (!valid_text(operation.idempotency_key))                              \
        throw OperationLedgerException(OperationLedgerError::InvalidRequest, \
                                       LABEL "幂等键无效");                  \
    std::vector<std::uint8_t> canonical;                                     \
    append_domain(canonical);                                                 \
    canonical.push_back(static_cast<std::uint8_t>(KIND));                    \
    put_array(canonical, operation.lease_id);                                \
    append_text(canonical, operation.owner_key_id);                          \
    append_text(canonical, operation.idempotency_key);                       \
    return sha256(canonical);                                                 \
}

DEFINE_TIMED_OPERATION_ID(RuntimeTimedBitstreamConfigureOperation,
                          OperationKind::RuntimeTimedBitstreamConfigure,
                          "定时位流配置")
DEFINE_TIMED_OPERATION_ID(RuntimeTimedBitstreamFrameOperation,
                          OperationKind::RuntimeTimedBitstreamFrame,
                          "定时位流帧")
DEFINE_TIMED_OPERATION_ID(RuntimeTimedBitstreamStopOperation,
                          OperationKind::RuntimeTimedBitstreamStop,
                          "定时位流停止")
#undef DEFINE_TIMED_OPERATION_ID

namespace {
OperationDigest timed_request_digest(
    const OperationDigest& operation_id, const OperationIdentity& daemon_origin,
    const OperationIdentity& lease_id, const OperationIdentity& node_uuid,
    const std::string& owner, std::uint16_t permissions, std::uint32_t node_id,
    std::uint32_t resource_id, std::uint8_t domain,
    const OperationDigest* payload_digest) {
    if (is_zero(node_uuid) || node_id == 0U || node_id > 127U ||
        resource_id == 0U || permissions == 0U)
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "定时位流请求摘要字段无效");
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(domain);
    put_array(canonical, operation_id);
    put_array(canonical, daemon_origin);
    put_array(canonical, lease_id);
    append_text(canonical, owner);
    put_u16(canonical, permissions);
    put_array(canonical, node_uuid);
    put_u32(canonical, node_id);
    put_u32(canonical, resource_id);
    if (payload_digest != nullptr) put_array(canonical, *payload_digest);
    return sha256(canonical);
}
}  // namespace

OperationDigest OperationLedger::derive_request_digest(
    const RuntimeTimedBitstreamConfigureOperation& operation) {
    const auto operation_id = derive_operation_id(operation);
    const protocol::TimedBitstreamCreatePayload checked{
        0U, operation.bit_period_ns, operation.zero_high_ns,
        operation.one_high_ns, operation.reset_time_us};
    try { static_cast<void>(protocol::encode_timed_bitstream_create(checked)); }
    catch (const protocol::WaveformPayloadException&) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "定时位流配置请求摘要字段无效");
    }
    std::vector<std::uint8_t> parameters;
    put_u32(parameters, operation.bit_period_ns);
    put_u32(parameters, operation.zero_high_ns);
    put_u32(parameters, operation.one_high_ns);
    put_u32(parameters, operation.reset_time_us);
    const auto digest = sha256(parameters);
    return timed_request_digest(operation_id, operation.daemon_origin,
        operation.lease_id, operation.expected_node_uuid, operation.owner_key_id,
        operation.permissions, operation.node_id, operation.resource_id, 0x85U,
        &digest);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimeTimedBitstreamFrameOperation& operation) {
    const auto operation_id = derive_operation_id(operation);
    try { static_cast<void>(protocol::encode_timed_bitstream_write(
              {operation.bit_count, operation.data})); }
    catch (const protocol::WaveformPayloadException&) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "定时位流帧摘要字段无效");
    }
    std::vector<std::uint8_t> parameters;
    put_u16(parameters, operation.bit_count);
    parameters.insert(parameters.end(), operation.data.begin(), operation.data.end());
    const auto digest = sha256(parameters);
    return timed_request_digest(operation_id, operation.daemon_origin,
        operation.lease_id, operation.expected_node_uuid, operation.owner_key_id,
        operation.permissions, operation.node_id, operation.resource_id, 0x86U,
        &digest);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimeTimedBitstreamStopOperation& operation) {
    return timed_request_digest(derive_operation_id(operation),
        operation.daemon_origin, operation.lease_id, operation.expected_node_uuid,
        operation.owner_key_id, operation.permissions, operation.node_id,
        operation.resource_id, 0x87U, nullptr);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimePwmConfigureOperation& operation) {
    const auto operation_id = derive_operation_id(operation);
    if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
        operation.node_id > 127U || operation.resource_id == 0U ||
        operation.permissions == 0U || operation.frequency_hz == 0U ||
        operation.duty > 10000U) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "PWM请求摘要字段无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(0x83U);
    put_array(canonical, operation_id);
    put_array(canonical, operation.daemon_origin);
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    put_u16(canonical, operation.permissions);
    put_array(canonical, operation.expected_node_uuid);
    put_u32(canonical, operation.node_id);
    put_u32(canonical, operation.resource_id);
    put_u32(canonical, operation.frequency_hz);
    put_u16(canonical, operation.duty);
    canonical.push_back(operation.active_low ? 1U : 0U);
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimePwmStopOperation& operation) {
    const auto operation_id = derive_operation_id(operation);
    if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
        operation.node_id > 127U || operation.resource_id == 0U ||
        operation.permissions == 0U) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "PWM停止请求摘要字段无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(0x84U);
    put_array(canonical, operation_id);
    put_array(canonical, operation.daemon_origin);
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    put_u16(canonical, operation.permissions);
    put_array(canonical, operation.expected_node_uuid);
    put_u32(canonical, operation.node_id);
    put_u32(canonical, operation.resource_id);
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimeGpioWriteOperation& operation) {
    const auto operation_id = derive_operation_id(operation);
    if (is_zero(operation.expected_node_uuid) || operation.node_id == 0U ||
        operation.node_id > 127U || operation.resource_id == 0U ||
        operation.permissions == 0U) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "GPIO请求摘要字段无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(0x81U);
    put_array(canonical, operation_id);
    put_array(canonical, operation.daemon_origin);
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    put_u16(canonical, operation.permissions);
    put_array(canonical, operation.expected_node_uuid);
    put_u32(canonical, operation.node_id);
    put_u32(canonical, operation.resource_id);
    canonical.push_back(operation.value ? 1U : 0U);
    return sha256(canonical);
}

OperationDigest OperationLedger::derive_request_digest(
    const RuntimeControlReleaseOperation& operation,
    const ServerResolvedLease& lease) {
    const auto operation_id = derive_operation_id(operation);
    if (is_zero(lease.expected_node_uuid) || lease.node_id == 0U ||
        lease.node_id > 127U || lease.resource_id == 0U ||
        lease.permissions == 0U || lease.admission_id == 0U) {
        throw OperationLedgerException(OperationLedgerError::InvalidRequest,
                                       "Release租约摘要字段无效");
    }
    std::vector<std::uint8_t> canonical;
    append_domain(canonical);
    canonical.push_back(0x82U);
    put_array(canonical, operation_id);
    put_array(canonical, operation.daemon_origin);
    put_array(canonical, operation.lease_id);
    append_text(canonical, operation.owner_key_id);
    put_u16(canonical, lease.permissions);
    put_array(canonical, lease.expected_node_uuid);
    put_u32(canonical, lease.node_id);
    put_u32(canonical, lease.resource_id);
    put_u64(canonical, lease.admission_id);
    return sha256(canonical);
}

OperationLedger::OperationLedger(OperationLedgerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

OperationLedger::~OperationLedger() noexcept = default;

OperationBeginResult OperationLedger::begin_gpio_write(
    const RuntimeGpioWriteOperation& operation) {
    return impl_->begin_gpio_write(operation);
}

OperationBeginResult OperationLedger::begin_release(
    const RuntimeControlReleaseOperation& operation,
    const ServerResolvedLease& server_resolved_lease) {
    return impl_->begin_release(operation, server_resolved_lease);
}

OperationBeginResult OperationLedger::begin_pwm_configure(
    const RuntimePwmConfigureOperation& operation) {
    return impl_->begin_pwm_configure(operation);
}

OperationBeginResult OperationLedger::begin_pwm_stop(
    const RuntimePwmStopOperation& operation) {
    return impl_->begin_pwm_stop(operation);
}

OperationBeginResult OperationLedger::begin_timed_bitstream_configure(
    const RuntimeTimedBitstreamConfigureOperation& operation) {
    return impl_->begin_timed_bitstream_configure(operation);
}

OperationBeginResult OperationLedger::begin_timed_bitstream_frame(
    const RuntimeTimedBitstreamFrameOperation& operation) {
    return impl_->begin_timed_bitstream_frame(operation);
}

OperationBeginResult OperationLedger::begin_timed_bitstream_stop(
    const RuntimeTimedBitstreamStopOperation& operation) {
    return impl_->begin_timed_bitstream_stop(operation);
}

OperationRecord OperationLedger::finish(
    const OperationDigest& operation_id,
    const OperationDigest& request_digest,
    OperationState state,
    OperationRecovery recovery,
    const OperationTerminalResult& result) {
    return impl_->finish(operation_id, request_digest, state, recovery, result);
}

OperationRecord OperationLedger::update_unknown_recovery(
    const OperationDigest& operation_id,
    const OperationDigest& request_digest,
    OperationRecovery recovery) {
    return impl_->update_unknown_recovery(
        operation_id, request_digest, recovery);
}

OperationLookupResult OperationLedger::lookup(
    const OperationDigest& operation_id,
    const std::string& owner_key_id) const {
    return impl_->lookup(operation_id, owner_key_id);
}

std::vector<OperationScope> OperationLedger::blocked_scopes() const {
    return impl_->blocked_scopes();
}

bool OperationLedger::mutation_available() const noexcept {
    return impl_->mutation_available();
}

std::string OperationLedger::failure_reason() const {
    return impl_->failure_reason();
}

std::size_t OperationLedger::operation_count() const {
    return impl_->operation_count();
}

void OperationLedger::compact() {
    impl_->compact();
}

}  // namespace remotebsp::toolbusd
