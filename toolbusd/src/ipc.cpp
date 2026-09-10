#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/protocol/waveform.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <system_error>

namespace remotebsp::toolbusd {
namespace {

constexpr std::size_t kMaximumIpcBodySize = 64U * 1024U;
constexpr std::size_t kNodeInfoSize = 33;
constexpr std::size_t kTrafficStatusHeaderSize = 76;
constexpr std::size_t kTrafficClassCounterSize = 32;
constexpr std::size_t kRuntimeSnapshotHeaderSize = 28U;
constexpr std::size_t kRuntimeResourceSize = 50U;
constexpr std::size_t kRuntimeNodeIssueSize = 8U;
constexpr std::size_t kRuntimeClockQualitySize = 68U;
constexpr std::size_t kRuntimeBusHealthSize = 28U;
constexpr std::size_t kMotionGroupPlanHeaderSize = 64U;
constexpr std::size_t kMotionGroupMemberHeaderSize = 8U;
constexpr std::size_t kMotionGroupSnapshotSize = 32U;
constexpr std::size_t kDaemonIdentitySize = 20U;
constexpr std::size_t kHealthSnapshotHeaderSize = 24U;
constexpr std::size_t kRuntimeControlAcquireHeaderSize = 65U;
constexpr std::size_t kRuntimeGpioWriteHeaderSize = 63U;
constexpr std::size_t kRuntimePwmRequestHeaderSize = 70U;
constexpr std::size_t kRuntimePwmStopRequestHeaderSize = 64U;
constexpr std::size_t kRuntimeTimedBitstreamConfigureHeaderSize = 80U;
constexpr std::size_t kRuntimeTimedBitstreamFrameHeaderSize = 68U;
constexpr std::size_t kRuntimeControlReleaseHeaderSize = 35U;
constexpr std::size_t kRuntimeGpioWriteResultSize = 8U;
constexpr std::size_t kIpcErrorEnvelopeHeaderSize = 12U;
constexpr std::size_t kRuntimeOperationQueryHeaderSize = 56U;
constexpr std::size_t kRuntimeOperationLookupHeaderSize = 44U;
constexpr std::size_t kRuntimeOperationOutcomeSize = 88U;
constexpr std::size_t kRuntimePwmOperationOutcomeSize = 96U;
constexpr std::size_t kMaximumLogicalRecordingNameBytes = 128U;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_i32(std::vector<std::uint8_t>& output, std::int32_t value) {
    append_u32(output, static_cast<std::uint32_t>(value));
}

void put_u32(std::uint8_t* output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output[index] =
            static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::uint32_t get_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint16_t get_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint64_t get_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index])
                 << (index * 8U);
    }
    return value;
}

std::int32_t get_i32(const std::uint8_t* input) {
    const auto value = get_u32(input);
    if (value <= static_cast<std::uint32_t>(
                     std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(value);
    }
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(value) - (1LL << 32U));
}

std::optional<IpcErrorCategory> expected_error_category(
    IpcErrorCode code) noexcept {
    switch (code) {
        case IpcErrorCode::InvalidRequest:
        case IpcErrorCode::UnsupportedRequest:
            return IpcErrorCategory::Request;
        case IpcErrorCode::DaemonIdentityMismatch:
            return IpcErrorCategory::Authentication;
        case IpcErrorCode::PermissionDenied:
        case IpcErrorCode::ContractRejected:
            return IpcErrorCategory::Authorization;
        case IpcErrorCode::LeaseConflict:
        case IpcErrorCode::LeaseNotFound:
        case IpcErrorCode::LeaseExpired:
        case IpcErrorCode::IdempotencyConflict:
        case IpcErrorCode::ObjectRetired:
            return IpcErrorCategory::Conflict;
        case IpcErrorCode::CapacityExceeded:
        case IpcErrorCode::NodeUnavailable:
        case IpcErrorCode::BackendUnavailable:
        case IpcErrorCode::HealthUnavailable:
            return IpcErrorCategory::Unavailable;
        case IpcErrorCode::DeadlineExceeded:
            return IpcErrorCategory::Timeout;
        case IpcErrorCode::SafeStopFailed:
        case IpcErrorCode::InternalFailure:
            return IpcErrorCategory::Internal;
    }
    return std::nullopt;
}

bool safe_error_message(const std::string& message) noexcept {
    if (message.empty() || message.size() > kMaximumIpcErrorMessageBytes) {
        return false;
    }
    std::size_t offset = 0U;
    while (offset < message.size()) {
        const auto first = static_cast<std::uint8_t>(message[offset]);
        if (first < 0x80U) {
            if (first < 0x20U || first == 0x7FU) return false;
            ++offset;
            continue;
        }
        std::size_t width = 0U;
        std::uint32_t code_point = 0U;
        std::uint32_t minimum = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            width = 2U;
            code_point = first & 0x1FU;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            width = 3U;
            code_point = first & 0x0FU;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            width = 4U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (offset + width > message.size()) return false;
        for (std::size_t index = 1U; index < width; ++index) {
            const auto continuation =
                static_cast<std::uint8_t>(message[offset + index]);
            if ((continuation & 0xC0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool possibly_committed_allowed(IpcErrorCode code) noexcept {
    return code == IpcErrorCode::SafeStopFailed ||
           code == IpcErrorCode::BackendUnavailable ||
           code == IpcErrorCode::DeadlineExceeded ||
           code == IpcErrorCode::InternalFailure;
}

bool error_flags_allowed(IpcErrorCode code, bool retryable,
                         bool possibly_committed) noexcept {
    if (possibly_committed) {
        return !retryable && possibly_committed_allowed(code);
    }
    switch (code) {
        case IpcErrorCode::LeaseConflict:
        case IpcErrorCode::CapacityExceeded:
        case IpcErrorCode::NodeUnavailable:
        case IpcErrorCode::BackendUnavailable:
        case IpcErrorCode::DeadlineExceeded:
        case IpcErrorCode::HealthUnavailable:
            return retryable;
        case IpcErrorCode::InvalidRequest:
        case IpcErrorCode::UnsupportedRequest:
        case IpcErrorCode::DaemonIdentityMismatch:
        case IpcErrorCode::PermissionDenied:
        case IpcErrorCode::LeaseNotFound:
        case IpcErrorCode::LeaseExpired:
        case IpcErrorCode::ContractRejected:
        case IpcErrorCode::IdempotencyConflict:
        case IpcErrorCode::SafeStopFailed:
        case IpcErrorCode::ObjectRetired:
        case IpcErrorCode::InternalFailure:
            return !retryable;
    }
    return false;
}

template <std::size_t Size>
bool nonzero_id(const std::array<std::uint8_t, Size>& value) noexcept {
    return std::any_of(value.begin(), value.end(),
                       [](std::uint8_t byte) { return byte != 0U; });
}

bool valid_operation_text(const std::string& value, std::size_t maximum,
                          bool allow_colon) noexcept {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [allow_colon](char item) {
        const auto byte = static_cast<unsigned char>(item);
        return (byte >= 'A' && byte <= 'Z') ||
               (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || item == '.' || item == '_' ||
               item == '-' || (allow_colon && item == ':');
    });
}

bool valid_operation_kind(RuntimeOperationKind kind) noexcept {
    return kind == RuntimeOperationKind::GpioWrite ||
           kind == RuntimeOperationKind::ControlRelease ||
           kind == RuntimeOperationKind::PwmConfigure ||
           kind == RuntimeOperationKind::PwmStop ||
           kind == RuntimeOperationKind::TimedBitstreamConfigure ||
           kind == RuntimeOperationKind::TimedBitstreamFrame ||
           kind == RuntimeOperationKind::TimedBitstreamStop;
}

bool valid_operation_error(RuntimeOperationError error) noexcept {
    return static_cast<std::uint16_t>(error) <=
           static_cast<std::uint16_t>(
               RuntimeOperationError::HistoryExpired);
}

bool valid_operation_outcome(const RuntimeOperationOutcome& outcome) noexcept {
    if (outcome.version != kRuntimeOperationIpcVersion ||
        (outcome.kind != RuntimeOperationKind::Unknown &&
         !valid_operation_kind(outcome.kind)) ||
        !nonzero_id(outcome.operation_id) ||
        !valid_operation_error(outcome.error)) {
        return false;
    }
    const bool has_result = outcome.object_id != 0U;
    const bool has_error = outcome.error != RuntimeOperationError::None;
    const bool has_scope = nonzero_id(outcome.lease_id) &&
                           nonzero_id(outcome.expected_node_uuid) &&
                           outcome.resource_id != 0U;
    const bool empty_scope = !nonzero_id(outcome.lease_id) &&
                             !nonzero_id(outcome.expected_node_uuid) &&
                             outcome.resource_id == 0U;
    if (outcome.kind == RuntimeOperationKind::Unknown) {
        return outcome.state == RuntimeOperationState::ExpiredUnknown &&
               outcome.recovery == RuntimeOperationRecovery::None &&
               empty_scope &&
               !has_result &&
               outcome.error == RuntimeOperationError::HistoryExpired &&
               !outcome.value;
    }
    switch (outcome.state) {
        case RuntimeOperationState::Pending:
            return outcome.recovery == RuntimeOperationRecovery::None &&
                   has_scope && !has_result && !has_error && !outcome.value;
        case RuntimeOperationState::Committed:
            if (outcome.kind == RuntimeOperationKind::GpioWrite ||
                outcome.kind == RuntimeOperationKind::PwmConfigure ||
                outcome.kind == RuntimeOperationKind::TimedBitstreamConfigure ||
                outcome.kind == RuntimeOperationKind::TimedBitstreamFrame) {
                return outcome.recovery == RuntimeOperationRecovery::None &&
                       has_scope && has_result && !has_error;
            }
            if (outcome.kind == RuntimeOperationKind::PwmStop ||
                outcome.kind == RuntimeOperationKind::TimedBitstreamStop) {
                return outcome.recovery == RuntimeOperationRecovery::SafeClosed &&
                       has_scope && has_result && !has_error && !outcome.value;
            }
            return outcome.recovery == RuntimeOperationRecovery::SafeClosed &&
                   has_scope && !has_result && !has_error && !outcome.value;
        case RuntimeOperationState::Rejected:
            return (outcome.recovery == RuntimeOperationRecovery::NotSent ||
                    outcome.recovery == RuntimeOperationRecovery::SafeClosed) &&
                   has_scope && !has_result && has_error &&
                   outcome.error != RuntimeOperationError::HistoryExpired &&
                   !outcome.value;
        case RuntimeOperationState::Unknown:
            return (outcome.recovery == RuntimeOperationRecovery::SafeClosed ||
                    outcome.recovery == RuntimeOperationRecovery::ScopeBlocked ||
                    outcome.recovery == RuntimeOperationRecovery::AwaitingReboot ||
                    outcome.recovery ==
                        RuntimeOperationRecovery::NodeRebootConfirmed) &&
                   has_scope && !has_result &&
                   (outcome.error == RuntimeOperationError::Deadline ||
                    outcome.error == RuntimeOperationError::Backend ||
                    outcome.error == RuntimeOperationError::Persistence) &&
                   !outcome.value;
        case RuntimeOperationState::ExpiredUnknown:
            return outcome.recovery == RuntimeOperationRecovery::None &&
                   empty_scope && !has_result &&
                   outcome.error == RuntimeOperationError::HistoryExpired &&
                   !outcome.value;
    }
    return false;
}

void send_all(int socket, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t sent =
            ::send(socket, data + offset, size - offset, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "发送本地 IPC 数据失败");
        }
        if (sent == 0) {
            throw IpcException("本地 IPC 连接在发送时关闭");
        }
        offset += static_cast<std::size_t>(sent);
    }
}

void receive_all(int socket, std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t received =
            ::recv(socket, data + offset, size - offset, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "接收本地 IPC 数据失败");
        }
        if (received == 0) {
            throw IpcException("本地 IPC 对端提前关闭连接");
        }
        offset += static_cast<std::size_t>(received);
    }
}

std::vector<std::uint8_t> receive_body(int socket) {
    std::uint8_t length_bytes[4]{};
    receive_all(socket, length_bytes, sizeof(length_bytes));
    const std::size_t length = get_u32(length_bytes);
    if (length > kMaximumIpcBodySize) {
        throw IpcException("本地 IPC 消息超过最大长度");
    }
    std::vector<std::uint8_t> body(length);
    if (!body.empty()) {
        receive_all(socket, body.data(), body.size());
    }
    return body;
}

void send_body(int socket, const std::vector<std::uint8_t>& body) {
    if (body.size() > kMaximumIpcBodySize) {
        throw IpcException("本地 IPC 消息超过最大长度");
    }
    std::uint8_t length_bytes[4]{};
    put_u32(length_bytes, static_cast<std::uint32_t>(body.size()));
    send_all(socket, length_bytes, sizeof(length_bytes));
    if (!body.empty()) {
        send_all(socket, body.data(), body.size());
    }
}

}

IpcException::IpcException(const std::string& message)
    : std::runtime_error(message) {}

IpcException::IpcException(const std::string& message,
                           IpcRequestKind request_kind)
    : std::runtime_error(message), has_request_kind_(true),
      request_kind_(request_kind) {}

bool IpcException::has_request_kind() const noexcept {
    return has_request_kind_;
}

IpcRequestKind IpcException::request_kind() const noexcept {
    return request_kind_;
}

void write_ipc_request(int socket, const protocol::Packet& packet,
                       std::uint32_t node_id) {
    if (node_id == 0 || node_id > 127) {
        throw IpcException("目标节点 ID 必须位于 1～127");
    }
    auto encoded = protocol::encode(packet);
    std::vector<std::uint8_t> body;
    body.reserve(1 + sizeof(std::uint32_t) + encoded.size());
    body.push_back(static_cast<std::uint8_t>(
        IpcRequestKind::RemotePacket));
    for (unsigned index = 0; index < 4; ++index) {
        body.push_back(
            static_cast<std::uint8_t>(node_id >> (index * 8U)));
    }
    body.insert(body.end(), encoded.begin(), encoded.end());
    send_body(socket, body);
}

void write_ipc_node_list_request(int socket) {
    send_body(socket,
              {static_cast<std::uint8_t>(IpcRequestKind::ListNodes)});
}

IpcRequest read_ipc_request(int socket) {
    const auto body = receive_body(socket);
    if (body.empty() ||
        body[0] > static_cast<std::uint8_t>(
                      IpcRequestKind::RuntimeTimedBitstreamStopOperation)) {
        throw IpcException("本地 IPC 请求类型无效");
    }
    const auto kind = static_cast<IpcRequestKind>(body[0]);
    if (kind == IpcRequestKind::ListNodes ||
        kind == IpcRequestKind::TrafficStatus ||
        kind == IpcRequestKind::DaemonIdentity ||
        kind == IpcRequestKind::LogicalRecordingStop ||
        kind == IpcRequestKind::LogicalRecordingStatus) {
        if (body.size() != 1) {
            throw IpcException("本地状态请求载荷无效");
        }
        IpcRequest request;
        request.kind = kind;
        return request;
    }
    if (kind == IpcRequestKind::LogicalRecordingStart) {
        if (body.size() < 2U ||
            body.size() > 1U + kMaximumLogicalRecordingNameBytes) {
            throw IpcException("逻辑链路录制文件名长度无效", kind);
        }
        IpcRequest request;
        request.kind = kind;
        request.logical_recording_name.assign(body.begin() + 1U, body.end());
        if (request.logical_recording_name.find('\0') != std::string::npos) {
            throw IpcException("逻辑链路录制文件名包含空字节", kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::HealthSnapshot) {
        if (body.size() != 5U ||
            get_u16(body.data() + 1U) != kHealthSnapshotIpcVersion ||
            get_u16(body.data() + 3U) != 0U) {
            throw IpcException("健康快照请求版本或长度无效", kind);
        }
        IpcRequest request;
        request.kind = kind;
        return request;
    }
    if (kind == IpcRequestKind::NextEvent) {
        if (body.size() != 5) {
            throw IpcException("事件等待请求载荷无效");
        }
        const auto node_id = get_u32(body.data() + 1);
        if (node_id == 0 || node_id > 127) {
            throw IpcException("事件目标节点 ID 必须位于 1～127");
        }
        IpcRequest request;
        request.kind = kind;
        request.node_id = node_id;
        return request;
    }
    if (kind == IpcRequestKind::UartStreamRead) {
        if (body.size() != 17) {
            throw IpcException("UART 流读取请求载荷无效");
        }
        const auto node_id = get_u32(body.data() + 1);
        const auto object_id = get_u32(body.data() + 5);
        const auto maximum_length = get_u32(body.data() + 9);
        const auto timeout_ms = get_u32(body.data() + 13);
        if (node_id == 0 || node_id > 127 || object_id == 0 ||
            maximum_length == 0 ||
            maximum_length > kMaximumIpcBodySize - 20U ||
            timeout_ms > 60000U) {
            throw IpcException("UART 流读取参数无效");
        }
        IpcRequest request;
        request.kind = kind;
        request.node_id = node_id;
        request.object_id = object_id;
        request.maximum_length = maximum_length;
        request.timeout_ms = timeout_ms;
        return request;
    }
    if (kind == IpcRequestKind::StreamRead) {
        if (body.size() != 17U) {
            throw IpcException("STREAM 读取请求载荷无效", kind);
        }
        IpcRequest request;
        request.kind = kind;
        request.node_id = get_u32(body.data() + 1U);
        request.stream_id = get_u32(body.data() + 5U);
        request.expected_sequence = get_u32(body.data() + 9U);
        request.timeout_ms = get_u32(body.data() + 13U);
        if (request.node_id == 0U || request.node_id > 127U ||
            request.stream_id == 0U || request.timeout_ms > 60000U) {
            throw IpcException("STREAM 读取参数无效", kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeSnapshot) {
        if (body.size() != 9U || get_u16(body.data() + 1U) !=
                                    kRuntimeSnapshotIpcVersion) {
            throw IpcException("Runtime 快照请求版本或长度无效");
        }
        const auto maximum_resources = get_u16(body.data() + 3U);
        const auto timeout_ms = get_u32(body.data() + 5U);
        if (maximum_resources == 0U ||
            maximum_resources > kMaximumRuntimeSnapshotResources ||
            timeout_ms == 0U ||
            timeout_ms > kMaximumRuntimeSnapshotTimeoutMs) {
            throw IpcException("Runtime 快照请求参数超出上限");
        }
        IpcRequest request;
        request.kind = kind;
        request.maximum_length = maximum_resources;
        request.timeout_ms = timeout_ms;
        return request;
    }
    if (kind == IpcRequestKind::MotionGroupSubmit) {
        IpcRequest request;
        request.kind = kind;
        request.motion_group_plan = decode_ipc_motion_group_plan(
            {body.begin() + 1, body.end()});
        return request;
    }
    if (kind == IpcRequestKind::MotionGroupStatus ||
        kind == IpcRequestKind::MotionGroupCancel) {
        if (body.size() != 21U ||
            get_u16(body.data() + 1U) != kMotionGroupIpcVersion ||
            get_u16(body.data() + 3U) != 0U) {
            throw IpcException("运动组状态请求版本或长度无效");
        }
        IpcRequest request;
        request.kind = kind;
        request.transaction_id = get_u64(body.data() + 5U);
        request.group_id = get_u32(body.data() + 13U);
        request.plan_generation = get_u32(body.data() + 17U);
        if (request.transaction_id == 0U || request.group_id == 0U ||
            request.plan_generation == 0U) {
            throw IpcException("运动组事务身份不能包含零值");
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeControlAcquire) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_control_acquire =
                decode_ipc_runtime_control_acquire(
                    {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeGpioWrite) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_gpio_write = decode_ipc_runtime_gpio_write(
                {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeControlRelease) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_control_release =
                decode_ipc_runtime_control_release(
                    {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeGpioWriteOperation) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_gpio_write = decode_ipc_runtime_gpio_write(
                {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeControlReleaseOperation) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_control_release =
                decode_ipc_runtime_control_release(
                    {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimePwmConfigureOperation) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_pwm_configure = decode_ipc_runtime_pwm_request(
                {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimePwmStopOperation) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_pwm_stop = decode_ipc_runtime_pwm_stop_request(
                {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeTimedBitstreamConfigureOperation) {
        IpcRequest request; request.kind = kind;
        try { request.runtime_timed_bitstream_configure =
            decode_ipc_runtime_timed_bitstream_configure({body.begin() + 1U, body.end()}); }
        catch (const IpcException& error) { throw IpcException(error.what(), kind); }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeTimedBitstreamFrameOperation) {
        IpcRequest request; request.kind = kind;
        try { request.runtime_timed_bitstream_frame =
            decode_ipc_runtime_timed_bitstream_frame({body.begin() + 1U, body.end()}); }
        catch (const IpcException& error) { throw IpcException(error.what(), kind); }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeTimedBitstreamStopOperation) {
        IpcRequest request; request.kind = kind;
        try { request.runtime_timed_bitstream_stop =
            decode_ipc_runtime_timed_bitstream_stop({body.begin() + 1U, body.end()}); }
        catch (const IpcException& error) { throw IpcException(error.what(), kind); }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeOperationQuery) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_operation_query =
                decode_ipc_runtime_operation_query(
                    {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (kind == IpcRequestKind::RuntimeOperationLookup) {
        IpcRequest request;
        request.kind = kind;
        try {
            request.runtime_operation_lookup =
                decode_ipc_runtime_operation_lookup(
                    {body.begin() + 1U, body.end()});
        } catch (const IpcException& error) {
            throw IpcException(error.what(), kind);
        }
        return request;
    }
    if (body.size() < 1 + sizeof(std::uint32_t)) {
        throw IpcException("本地 IPC 请求缺少目标节点 ID");
    }
    const auto node_id = get_u32(body.data() + 1);
    if (node_id == 0 || node_id > 127) {
        throw IpcException("目标节点 ID 必须位于 1～127");
    }
    const std::vector<std::uint8_t> packet(
        body.begin() + static_cast<std::ptrdiff_t>(
                           1 + sizeof(std::uint32_t)),
        body.end());
    IpcRequest request;
    request.kind = kind;
    request.node_id = node_id;
    request.packet = protocol::decode(packet);
    return request;
}

void write_ipc_daemon_identity_request(int socket) {
    send_body(socket,
              {static_cast<std::uint8_t>(IpcRequestKind::DaemonIdentity)});
}

void write_ipc_logical_recording_start_request(
    int socket, const std::string& output_name) {
    if (output_name.empty() ||
        output_name.size() > kMaximumLogicalRecordingNameBytes) {
        throw IpcException("逻辑链路录制文件名长度无效");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::LogicalRecordingStart)};
    body.insert(body.end(), output_name.begin(), output_name.end());
    send_body(socket, body);
}

void write_ipc_logical_recording_stop_request(int socket) {
    send_body(socket, {static_cast<std::uint8_t>(
                          IpcRequestKind::LogicalRecordingStop)});
}

void write_ipc_logical_recording_status_request(int socket) {
    send_body(socket, {static_cast<std::uint8_t>(
                          IpcRequestKind::LogicalRecordingStatus)});
}

void write_ipc_health_snapshot_request(int socket) {
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::HealthSnapshot)};
    append_u16(body, kHealthSnapshotIpcVersion);
    append_u16(body, 0U);
    send_body(socket, body);
}

void write_ipc_runtime_control_acquire_request(
    int socket, const RuntimeControlAcquireRequest& request) {
    auto body = encode_ipc_runtime_control_acquire(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeControlAcquire));
    send_body(socket, body);
}

void write_ipc_runtime_gpio_write_request(
    int socket, const RuntimeGpioWriteRequest& request) {
    auto body = encode_ipc_runtime_gpio_write(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeGpioWrite));
    send_body(socket, body);
}

void write_ipc_runtime_control_release_request(
    int socket, const RuntimeControlReleaseRequest& request) {
    auto body = encode_ipc_runtime_control_release(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeControlRelease));
    send_body(socket, body);
}

void write_ipc_runtime_gpio_write_operation_request(
    int socket, const RuntimeGpioWriteRequest& request) {
    auto body = encode_ipc_runtime_gpio_write(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeGpioWriteOperation));
    send_body(socket, body);
}

void write_ipc_runtime_control_release_operation_request(
    int socket, const RuntimeControlReleaseRequest& request) {
    auto body = encode_ipc_runtime_control_release(request);
    body.insert(
        body.begin(),
        static_cast<std::uint8_t>(
            IpcRequestKind::RuntimeControlReleaseOperation));
    send_body(socket, body);
}

void write_ipc_runtime_pwm_configure_operation_request(
    int socket, const RuntimePwmConfigureRequest& request) {
    auto body = encode_ipc_runtime_pwm_request(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimePwmConfigureOperation));
    send_body(socket, body);
}

void write_ipc_runtime_pwm_stop_operation_request(
    int socket, const RuntimePwmStopRequest& request) {
    auto body = encode_ipc_runtime_pwm_stop_request(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimePwmStopOperation));
    send_body(socket, body);
}

void write_ipc_runtime_timed_bitstream_configure_operation_request(
    int socket, const RuntimeTimedBitstreamConfigureRequest& request) {
    auto body = encode_ipc_runtime_timed_bitstream_configure(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
        IpcRequestKind::RuntimeTimedBitstreamConfigureOperation));
    send_body(socket, body);
}

void write_ipc_runtime_timed_bitstream_frame_operation_request(
    int socket, const RuntimeTimedBitstreamFrameRequest& request) {
    auto body = encode_ipc_runtime_timed_bitstream_frame(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
        IpcRequestKind::RuntimeTimedBitstreamFrameOperation));
    send_body(socket, body);
}

void write_ipc_runtime_timed_bitstream_stop_operation_request(
    int socket, const RuntimeTimedBitstreamStopRequest& request) {
    auto body = encode_ipc_runtime_timed_bitstream_stop(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
        IpcRequestKind::RuntimeTimedBitstreamStopOperation));
    send_body(socket, body);
}

void write_ipc_runtime_operation_query_request(
    int socket, const RuntimeOperationQuery& request) {
    auto body = encode_ipc_runtime_operation_query(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeOperationQuery));
    send_body(socket, body);
}

void write_ipc_runtime_operation_lookup_request(
    int socket, const RuntimeOperationLookup& request) {
    auto body = encode_ipc_runtime_operation_lookup(request);
    body.insert(body.begin(), static_cast<std::uint8_t>(
                                  IpcRequestKind::RuntimeOperationLookup));
    send_body(socket, body);
}

void write_ipc_traffic_status_request(int socket) {
    send_body(
        socket,
        {static_cast<std::uint8_t>(IpcRequestKind::TrafficStatus)});
}

void write_ipc_next_event_request(int socket, std::uint32_t node_id) {
    if (node_id == 0 || node_id > 127) {
        throw IpcException("事件目标节点 ID 必须位于 1～127");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::NextEvent)};
    for (unsigned index = 0; index < 4; ++index) {
        body.push_back(
            static_cast<std::uint8_t>(node_id >> (index * 8U)));
    }
    send_body(socket, body);
}

void write_ipc_uart_stream_read_request(
    int socket, std::uint32_t node_id, std::uint32_t object_id,
    std::uint32_t maximum_length, std::uint32_t timeout_ms) {
    if (node_id == 0 || node_id > 127 || object_id == 0 ||
        maximum_length == 0 ||
        maximum_length > kMaximumIpcBodySize - 20U ||
        timeout_ms > 60000U) {
        throw IpcException("UART 流读取参数无效");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::UartStreamRead)};
    append_u32(body, node_id);
    append_u32(body, object_id);
    append_u32(body, maximum_length);
    append_u32(body, timeout_ms);
    send_body(socket, body);
}

void write_ipc_stream_read_request(
    int socket, std::uint32_t node_id, std::uint32_t stream_id,
    std::uint32_t expected_sequence, std::uint32_t timeout_ms) {
    if (node_id == 0U || node_id > 127U || stream_id == 0U ||
        timeout_ms > 60000U) {
        throw IpcException("STREAM 读取参数无效");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::StreamRead)};
    append_u32(body, node_id);
    append_u32(body, stream_id);
    append_u32(body, expected_sequence);
    append_u32(body, timeout_ms);
    send_body(socket, body);
}

void write_ipc_runtime_snapshot_request(
    int socket, std::uint16_t maximum_resources,
    std::uint32_t timeout_ms) {
    if (maximum_resources == 0U ||
        maximum_resources > kMaximumRuntimeSnapshotResources ||
        timeout_ms == 0U ||
        timeout_ms > kMaximumRuntimeSnapshotTimeoutMs) {
        throw IpcException("Runtime 快照请求参数超出上限");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(IpcRequestKind::RuntimeSnapshot)};
    append_u16(body, kRuntimeSnapshotIpcVersion);
    append_u16(body, maximum_resources);
    append_u32(body, timeout_ms);
    send_body(socket, body);
}

void write_ipc_motion_group_submit_request(
    int socket, const MotionGroupPlan& plan) {
    const auto encoded = encode_ipc_motion_group_plan(plan);
    std::vector<std::uint8_t> body;
    body.reserve(1U + encoded.size());
    body.push_back(static_cast<std::uint8_t>(
        IpcRequestKind::MotionGroupSubmit));
    body.insert(body.end(), encoded.begin(), encoded.end());
    send_body(socket, body);
}

void write_motion_group_identity_request(
    int socket, IpcRequestKind kind, std::uint64_t transaction_id,
    std::uint32_t group_id, std::uint32_t plan_generation) {
    if ((kind != IpcRequestKind::MotionGroupStatus &&
         kind != IpcRequestKind::MotionGroupCancel) ||
        transaction_id == 0U || group_id == 0U ||
        plan_generation == 0U) {
        throw IpcException("运动组状态请求身份无效");
    }
    std::vector<std::uint8_t> body{
        static_cast<std::uint8_t>(kind)};
    append_u16(body, kMotionGroupIpcVersion);
    append_u16(body, 0U);
    append_u64(body, transaction_id);
    append_u32(body, group_id);
    append_u32(body, plan_generation);
    send_body(socket, body);
}

void write_ipc_motion_group_status_request(
    int socket, std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation) {
    write_motion_group_identity_request(
        socket, IpcRequestKind::MotionGroupStatus, transaction_id,
        group_id, plan_generation);
}

void write_ipc_motion_group_cancel_request(
    int socket, std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation) {
    write_motion_group_identity_request(
        socket, IpcRequestKind::MotionGroupCancel, transaction_id,
        group_id, plan_generation);
}

std::vector<std::uint8_t> encode_ipc_motion_group_plan(
    const MotionGroupPlan& plan) {
    if (plan.members.empty() ||
        plan.members.size() > kMaximumIpcMotionGroupMembers) {
        throw IpcException("运动组 IPC 成员数量超出范围");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kMotionGroupPlanHeaderSize);
    append_u16(body, kMotionGroupIpcVersion);
    append_u16(body, 0U);
    append_u64(body, plan.transaction_id);
    append_u32(body, plan.group_id);
    append_u32(body, plan.plan_generation);
    append_u64(body, plan.host_start_time_ns);
    body.insert(body.end(), plan.content_digest.begin(),
                plan.content_digest.end());
    append_u16(body, static_cast<std::uint16_t>(plan.members.size()));
    append_u16(body, 0U);
    for (const auto& member : plan.members) {
        const auto segment = protocol::encode_motion_segment(member.segment);
        if (member.node_id == 0U || member.node_id > 127U ||
            segment.empty() ||
            segment.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw IpcException("运动组 IPC 成员节点或运动段无效");
        }
        append_u32(body, member.node_id);
        append_u16(body, static_cast<std::uint16_t>(segment.size()));
        append_u16(body, 0U);
        body.insert(body.end(), segment.begin(), segment.end());
        if (body.size() > kMaximumIpcBodySize - 1U) {
            throw IpcException("运动组计划超过本地 IPC 上限");
        }
    }
    return body;
}

MotionGroupPlan decode_ipc_motion_group_plan(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kMotionGroupPlanHeaderSize ||
        get_u16(body.data()) != kMotionGroupIpcVersion ||
        get_u16(body.data() + 2U) != 0U ||
        get_u16(body.data() + 62U) != 0U) {
        throw IpcException("运动组计划版本、长度或保留字段无效");
    }
    MotionGroupPlan plan;
    plan.transaction_id = get_u64(body.data() + 4U);
    plan.group_id = get_u32(body.data() + 12U);
    plan.plan_generation = get_u32(body.data() + 16U);
    plan.host_start_time_ns = get_u64(body.data() + 20U);
    std::copy_n(body.data() + 28U, plan.content_digest.size(),
                plan.content_digest.begin());
    const auto member_count = get_u16(body.data() + 60U);
    if (member_count == 0U ||
        member_count > kMaximumIpcMotionGroupMembers) {
        throw IpcException("运动组 IPC 成员数量超出范围");
    }
    std::size_t offset = kMotionGroupPlanHeaderSize;
    plan.members.reserve(member_count);
    for (std::uint16_t index = 0U; index < member_count; ++index) {
        if (body.size() - offset < kMotionGroupMemberHeaderSize) {
            throw IpcException("运动组 IPC 成员头被截断");
        }
        const auto node_id = get_u32(body.data() + offset);
        const auto segment_size = get_u16(body.data() + offset + 4U);
        const auto reserved = get_u16(body.data() + offset + 6U);
        offset += kMotionGroupMemberHeaderSize;
        if (node_id == 0U || node_id > 127U || segment_size == 0U ||
            reserved != 0U || body.size() - offset < segment_size) {
            throw IpcException("运动组 IPC 成员节点或长度无效");
        }
        const std::vector<std::uint8_t> segment(
            body.begin() + static_cast<std::ptrdiff_t>(offset),
            body.begin() + static_cast<std::ptrdiff_t>(offset + segment_size));
        plan.members.push_back(
            {node_id, protocol::decode_motion_segment(segment)});
        offset += segment_size;
    }
    if (offset != body.size()) {
        throw IpcException("运动组 IPC 计划包含尾随数据");
    }
    return plan;
}

std::vector<std::uint8_t> encode_ipc_motion_group_snapshot(
    const MotionGroupServiceSnapshot& snapshot) {
    if (snapshot.member_count > kMaximumIpcMotionGroupMembers ||
        snapshot.ready_count > snapshot.member_count ||
        snapshot.committed_count > snapshot.member_count ||
        snapshot.pending_request_count > kMaximumIpcMotionGroupMembers ||
        (snapshot.abort_is_best_effort && !snapshot.commit_dispatched)) {
        throw IpcException("运动组状态快照字段无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kMotionGroupSnapshotSize);
    append_u16(body, kMotionGroupIpcVersion);
    body.push_back(static_cast<std::uint8_t>(snapshot.state));
    body.push_back(snapshot.abort_reason.has_value()
                       ? static_cast<std::uint8_t>(*snapshot.abort_reason)
                       : 0U);
    append_u64(body, snapshot.transaction_id);
    append_u32(body, snapshot.group_id);
    append_u32(body, snapshot.plan_generation);
    append_u16(body, snapshot.member_count);
    append_u16(body, snapshot.ready_count);
    append_u16(body, snapshot.committed_count);
    append_u16(body, snapshot.pending_request_count);
    append_u16(body,
               static_cast<std::uint16_t>(snapshot.commit_dispatched) |
                   static_cast<std::uint16_t>(
                       snapshot.abort_is_best_effort
                           ? 2U
                           : 0U));
    append_u16(body, 0U);
    return body;
}

MotionGroupServiceSnapshot decode_ipc_motion_group_snapshot(
    const std::vector<std::uint8_t>& body) {
    if (body.size() != kMotionGroupSnapshotSize ||
        get_u16(body.data()) != kMotionGroupIpcVersion ||
        body[2U] > static_cast<std::uint8_t>(MotionGroupState::Aborted) ||
        body[3U] > static_cast<std::uint8_t>(
                       protocol::MotionGroupAbortReason::StartDeadlineMissed) ||
        (get_u16(body.data() + 28U) & ~3U) != 0U ||
        get_u16(body.data() + 30U) != 0U) {
        throw IpcException("运动组状态快照格式无效");
    }
    MotionGroupServiceSnapshot snapshot;
    snapshot.state = static_cast<MotionGroupState>(body[2U]);
    if (body[3U] != 0U) {
        snapshot.abort_reason =
            static_cast<protocol::MotionGroupAbortReason>(body[3U]);
    }
    snapshot.transaction_id = get_u64(body.data() + 4U);
    snapshot.group_id = get_u32(body.data() + 12U);
    snapshot.plan_generation = get_u32(body.data() + 16U);
    snapshot.member_count = get_u16(body.data() + 20U);
    snapshot.ready_count = get_u16(body.data() + 22U);
    snapshot.committed_count = get_u16(body.data() + 24U);
    snapshot.pending_request_count = get_u16(body.data() + 26U);
    const auto flags = get_u16(body.data() + 28U);
    snapshot.commit_dispatched = (flags & 1U) != 0U;
    snapshot.abort_is_best_effort = (flags & 2U) != 0U;
    if (snapshot.member_count > kMaximumIpcMotionGroupMembers ||
        snapshot.ready_count > snapshot.member_count ||
        snapshot.committed_count > snapshot.member_count ||
        snapshot.pending_request_count > kMaximumIpcMotionGroupMembers ||
        (snapshot.abort_is_best_effort && !snapshot.commit_dispatched)) {
        throw IpcException("运动组状态快照计数或标志无效");
    }
    return snapshot;
}

std::vector<std::uint8_t> encode_ipc_uart_stream_chunk(
    const UartStreamChunk& chunk) {
    if (chunk.data.size() > kMaximumIpcBodySize - 20U) {
        throw IpcException("UART 流数据超过本地 IPC 上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(20U + chunk.data.size());
    append_u16(body, 1U);
    append_u16(body, 0U);
    append_u64(body, chunk.dropped_bytes);
    append_u64(body, chunk.lost_events);
    body.insert(body.end(), chunk.data.begin(), chunk.data.end());
    return body;
}

UartStreamChunk decode_ipc_uart_stream_chunk(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < 20U || get_u16(body.data()) != 1U ||
        get_u16(body.data() + 2U) != 0U) {
        throw IpcException("UART 流响应格式无效");
    }
    UartStreamChunk chunk;
    chunk.dropped_bytes = get_u64(body.data() + 4U);
    chunk.lost_events = get_u64(body.data() + 12U);
    chunk.data.assign(body.begin() + 20, body.end());
    return chunk;
}

std::vector<std::uint8_t> encode_ipc_node_list(
    const std::vector<IpcNodeInfo>& nodes) {
    if (nodes.size() > 127) {
        throw IpcException("节点数量超过本地 IPC 范围");
    }
    std::vector<std::uint8_t> body;
    body.reserve(2 + nodes.size() * kNodeInfoSize);
    body.push_back(static_cast<std::uint8_t>(nodes.size()));
    body.push_back(0);
    for (const auto& node : nodes) {
        body.insert(body.end(), node.uuid.begin(), node.uuid.end());
        const auto offset = body.size();
        body.resize(offset + 4);
        put_u32(body.data() + offset, node.node_id);
        body.push_back(static_cast<std::uint8_t>(node.online));
        body.push_back(static_cast<std::uint8_t>(node.ready));
        body.push_back(static_cast<std::uint8_t>(node.firmware_major));
        body.push_back(static_cast<std::uint8_t>(node.firmware_major >> 8U));
        body.push_back(static_cast<std::uint8_t>(node.firmware_minor));
        body.push_back(static_cast<std::uint8_t>(node.firmware_minor >> 8U));
        body.push_back(static_cast<std::uint8_t>(node.firmware_patch));
        body.push_back(static_cast<std::uint8_t>(node.firmware_patch >> 8U));
        const auto board_offset = body.size();
        body.resize(board_offset + 4);
        put_u32(body.data() + board_offset, node.board_type);
        body.push_back(node.protocol_version);
    }
    return body;
}

std::vector<IpcNodeInfo> decode_ipc_node_list(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < 2 || body[1] != 0 ||
        body.size() != 2 + static_cast<std::size_t>(body[0]) *
                               kNodeInfoSize) {
        throw IpcException("节点列表响应长度无效");
    }
    std::vector<IpcNodeInfo> nodes;
    nodes.reserve(body[0]);
    for (std::size_t index = 0; index < body[0]; ++index) {
        const auto* input = body.data() + 2 + index * kNodeInfoSize;
        IpcNodeInfo node;
        std::copy_n(input, node.uuid.size(), node.uuid.begin());
        node.node_id = get_u32(input + 16);
        if (input[20] > 1 || input[21] > 1) {
            throw IpcException("节点在线或就绪状态无效");
        }
        node.online = input[20] != 0;
        node.ready = input[21] != 0;
        node.firmware_major = static_cast<std::uint16_t>(
            input[22] | (static_cast<std::uint16_t>(input[23]) << 8U));
        node.firmware_minor = static_cast<std::uint16_t>(
            input[24] | (static_cast<std::uint16_t>(input[25]) << 8U));
        node.firmware_patch = static_cast<std::uint16_t>(
            input[26] | (static_cast<std::uint16_t>(input[27]) << 8U));
        node.board_type = get_u32(input + 28);
        node.protocol_version = input[32];
        nodes.push_back(node);
    }
    return nodes;
}

std::vector<std::uint8_t> encode_ipc_traffic_status(
    const TrafficSnapshot& snapshot) {
    if (snapshot.version != TrafficSnapshot::kVersion ||
        snapshot.mode > TrafficBusMode::Usb) {
        throw IpcException("CAN 流量状态版本或模式无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kTrafficStatusHeaderSize +
                 kTrafficClassCount * kTrafficClassCounterSize);
    append_u16(body, snapshot.version);
    body.push_back(static_cast<std::uint8_t>(snapshot.mode));
    body.push_back(static_cast<std::uint8_t>(kTrafficClassCount));
    append_u32(body, snapshot.arbitration_bits_per_second);
    append_u32(body, snapshot.data_bits_per_second);
    append_u16(body, snapshot.maximum_utilization_permille);
    append_u16(body, 0);
    append_u32(body, snapshot.burst_window_ms);
    append_u64(body, snapshot.global_capacity_ns);
    append_u64(body, snapshot.global_available_ns);
    append_u64(body, snapshot.admitted_packets);
    append_u64(body, snapshot.rejected_packets);
    append_u64(body, snapshot.guaranteed_overruns);
    append_u64(body, snapshot.admitted_frames);
    append_u64(body, snapshot.estimated_wire_time_ns);
    for (const auto& counters : snapshot.classes) {
        append_u64(body, counters.admitted_packets);
        append_u64(body, counters.rejected_packets);
        append_u64(body, counters.admitted_frames);
        append_u64(body, counters.estimated_wire_time_ns);
    }
    return body;
}

TrafficSnapshot decode_ipc_traffic_status(
    const std::vector<std::uint8_t>& body) {
    const std::size_t expected =
        kTrafficStatusHeaderSize +
        kTrafficClassCount * kTrafficClassCounterSize;
    if (body.size() != expected ||
        get_u16(body.data()) != TrafficSnapshot::kVersion ||
        body[2] > static_cast<std::uint8_t>(TrafficBusMode::Usb) ||
        body[3] != kTrafficClassCount ||
        get_u16(body.data() + 14) != 0) {
        throw IpcException("CAN 流量状态响应长度或版本无效");
    }
    TrafficSnapshot snapshot;
    snapshot.version = get_u16(body.data());
    snapshot.mode = static_cast<TrafficBusMode>(body[2]);
    snapshot.arbitration_bits_per_second = get_u32(body.data() + 4);
    snapshot.data_bits_per_second = get_u32(body.data() + 8);
    snapshot.maximum_utilization_permille = get_u16(body.data() + 12);
    snapshot.burst_window_ms = get_u32(body.data() + 16);
    snapshot.global_capacity_ns = get_u64(body.data() + 20);
    snapshot.global_available_ns = get_u64(body.data() + 28);
    snapshot.admitted_packets = get_u64(body.data() + 36);
    snapshot.rejected_packets = get_u64(body.data() + 44);
    snapshot.guaranteed_overruns = get_u64(body.data() + 52);
    snapshot.admitted_frames = get_u64(body.data() + 60);
    snapshot.estimated_wire_time_ns = get_u64(body.data() + 68);
    for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
        const auto* input =
            body.data() + kTrafficStatusHeaderSize +
            index * kTrafficClassCounterSize;
        snapshot.classes[index].admitted_packets = get_u64(input);
        snapshot.classes[index].rejected_packets = get_u64(input + 8);
        snapshot.classes[index].admitted_frames = get_u64(input + 16);
        snapshot.classes[index].estimated_wire_time_ns =
            get_u64(input + 24);
    }
    return snapshot;
}

std::vector<std::uint8_t> encode_ipc_runtime_snapshot(
    const IpcRuntimeSnapshot& snapshot) {
    if (snapshot.version != kRuntimeSnapshotIpcVersion ||
        snapshot.sequence == 0U || snapshot.nodes.size() > 127U ||
        snapshot.resources.size() > kMaximumRuntimeSnapshotResources ||
        snapshot.node_issues.size() > 127U ||
        snapshot.bus_health.size() > kMaximumRuntimeSnapshotResources ||
        snapshot.clocks.size() != snapshot.nodes.size()) {
        throw IpcException("Runtime 快照版本、序号或条目数量无效");
    }
    const auto node_body = encode_ipc_node_list(snapshot.nodes);
    const auto traffic_body = encode_ipc_traffic_status(snapshot.traffic);
    std::vector<std::uint32_t> node_ids;
    node_ids.reserve(snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        if (node.node_id == 0U || node.node_id > 127U ||
            std::find(node_ids.begin(), node_ids.end(), node.node_id) !=
                node_ids.end()) {
            throw IpcException("Runtime 快照节点 ID 无效或重复");
        }
        node_ids.push_back(node.node_id);
    }
    const auto expected_size = kRuntimeSnapshotHeaderSize +
        node_body.size() + traffic_body.size() +
        snapshot.resources.size() * kRuntimeResourceSize +
        snapshot.node_issues.size() * kRuntimeNodeIssueSize;
    const auto total_size = expected_size +
        snapshot.clocks.size() * kRuntimeClockQualitySize +
        snapshot.bus_health.size() * kRuntimeBusHealthSize;
    if (total_size > kMaximumIpcBodySize) {
        throw IpcException("Runtime 快照超过本地 IPC 字节上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(total_size);
    append_u16(body, snapshot.version);
    append_u16(body, static_cast<std::uint16_t>(snapshot.bus_health.size()));
    append_u64(body, snapshot.sequence);
    append_u16(body, static_cast<std::uint16_t>(snapshot.nodes.size()));
    append_u16(body, static_cast<std::uint16_t>(snapshot.resources.size()));
    append_u16(body, static_cast<std::uint16_t>(snapshot.node_issues.size()));
    append_u16(body, static_cast<std::uint16_t>(snapshot.clocks.size()));
    append_u32(body, static_cast<std::uint32_t>(node_body.size()));
    append_u32(body, static_cast<std::uint32_t>(traffic_body.size()));
    body.insert(body.end(), node_body.begin(), node_body.end());
    body.insert(body.end(), traffic_body.begin(), traffic_body.end());
    std::vector<std::uint64_t> identities;
    identities.reserve(snapshot.resources.size());
    for (const auto& resource : snapshot.resources) {
        const auto identity =
            (static_cast<std::uint64_t>(resource.node_id) << 32U) |
            resource.descriptor.resource_id;
        if (resource.node_id == 0U || resource.node_id > 127U ||
            resource.descriptor.resource_id == 0U ||
            resource.status.resource_id != resource.descriptor.resource_id ||
            std::find(node_ids.begin(), node_ids.end(), resource.node_id) ==
                node_ids.end() ||
            std::find(identities.begin(), identities.end(), identity) !=
                identities.end()) {
            throw IpcException("Runtime 快照资源归属或状态 ID 无效");
        }
        identities.push_back(identity);
        append_u32(body, resource.node_id);
        body.push_back(static_cast<std::uint8_t>(resource.status_valid));
        body.push_back(0U);
        append_u16(body, 0U);
        const auto descriptor =
            protocol::encode_resource_descriptor(resource.descriptor);
        const auto status = protocol::encode_resource_status(resource.status);
        body.insert(body.end(), descriptor.begin(), descriptor.end());
        body.insert(body.end(), status.begin(), status.end());
    }
    std::vector<std::uint32_t> issue_nodes;
    issue_nodes.reserve(snapshot.node_issues.size());
    for (const auto& issue : snapshot.node_issues) {
        if (issue.node_id == 0U || issue.node_id > 127U ||
            issue.error != IpcRuntimeNodeError::ResourceInventoryUnavailable ||
            std::find(node_ids.begin(), node_ids.end(), issue.node_id) ==
                node_ids.end() ||
            std::find(issue_nodes.begin(), issue_nodes.end(), issue.node_id) !=
                issue_nodes.end()) {
            throw IpcException("Runtime 快照节点错误项无效");
        }
        issue_nodes.push_back(issue.node_id);
        append_u32(body, issue.node_id);
        body.push_back(static_cast<std::uint8_t>(issue.error));
        body.push_back(0U);
        append_u16(body, 0U);
    }
    std::vector<std::uint32_t> clock_nodes;
    clock_nodes.reserve(snapshot.clocks.size());
    for (const auto& clock : snapshot.clocks) {
        const auto state = static_cast<std::uint8_t>(clock.state);
        const bool node_known =
            std::find(node_ids.begin(), node_ids.end(), clock.node_id) !=
            node_ids.end();
        const bool duplicate =
            std::find(clock_nodes.begin(), clock_nodes.end(), clock.node_id) !=
            clock_nodes.end();
        const bool invalid_unregistered = !clock.registered &&
            (clock.estimate_valid || clock.state != ClockSyncState::Unsynced ||
             clock.boot_epoch != 0U || clock.model_generation != 0U ||
             clock.sample_count != 0U || clock.selected_sample_count != 0U ||
             clock.drift_uncertainty_ppm != 0U ||
             clock.rate_deviation_ppb != 0 ||
             clock.minimum_network_rtt_ns != 0U ||
             clock.error_bound_ns != 0U || clock.sample_age_ns != 0U ||
             clock.last_sample_host_time_ns != 0U);
        const bool invalid_unknown_estimate = !clock.estimate_valid &&
            (clock.drift_uncertainty_ppm != 0U ||
             clock.rate_deviation_ppb != 0 ||
             clock.minimum_network_rtt_ns != 0U ||
             clock.error_bound_ns != 0U || clock.sample_age_ns != 0U ||
             clock.last_sample_host_time_ns != 0U);
        if (!node_known || duplicate || state >
                static_cast<std::uint8_t>(ClockSyncState::Degraded) ||
            (clock.registered &&
             (clock.boot_epoch == 0U || clock.model_generation == 0U)) ||
            clock.selected_sample_count > clock.sample_count ||
            (!clock.estimate_valid && clock.state != ClockSyncState::Unsynced) ||
            invalid_unknown_estimate || invalid_unregistered) {
            throw IpcException("Runtime 快照时钟质量字段无效");
        }
        clock_nodes.push_back(clock.node_id);
        append_u32(body, clock.node_id);
        body.push_back(static_cast<std::uint8_t>(
            (clock.registered ? 1U : 0U) |
            (clock.estimate_valid ? 2U : 0U)));
        body.push_back(state);
        append_u16(body, 0U);
        append_u64(body, clock.boot_epoch);
        append_u64(body, clock.model_generation);
        append_u16(body, clock.sample_count);
        append_u16(body, clock.selected_sample_count);
        append_u32(body, clock.drift_uncertainty_ppm);
        append_i32(body, clock.rate_deviation_ppb);
        append_u64(body, clock.minimum_network_rtt_ns);
        append_u64(body, clock.error_bound_ns);
        append_u64(body, clock.sample_age_ns);
        append_u64(body, clock.last_sample_host_time_ns);
    }
    std::vector<std::uint64_t> bus_identities;
    for (const auto& health : snapshot.bus_health) {
        const auto identity = (static_cast<std::uint64_t>(health.node_id) << 32U) |
                              health.resource_id;
        const auto status = static_cast<std::uint8_t>(health.last_status);
        if (health.node_id == 0U || health.node_id > 127U ||
            health.resource_id == 0U || status > static_cast<std::uint8_t>(
                protocol::BusTransactionStatus::LimitExceeded) ||
            (!health.last_status_valid &&
             (status != 0U || health.consecutive_failures != 0U ||
              health.peak_consecutive_failures != 0U ||
              health.last_result_time_us != 0U)) ||
            health.consecutive_failures > health.peak_consecutive_failures ||
            std::find(node_ids.begin(), node_ids.end(), health.node_id) ==
                node_ids.end() ||
            std::find(bus_identities.begin(), bus_identities.end(), identity) !=
                bus_identities.end()) {
            throw IpcException("Runtime 总线健康项无效");
        }
        bus_identities.push_back(identity);
        append_u32(body, health.node_id);
        append_u32(body, health.resource_id);
        body.push_back(static_cast<std::uint8_t>(health.last_status_valid));
        body.push_back(status);
        append_u16(body, 0U);
        append_u32(body, health.consecutive_failures);
        append_u32(body, health.peak_consecutive_failures);
        append_u64(body, health.last_result_time_us);
    }
    return body;
}

IpcRuntimeSnapshot decode_ipc_runtime_snapshot(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeSnapshotHeaderSize ||
        get_u16(body.data()) != kRuntimeSnapshotIpcVersion) {
        throw IpcException("Runtime 快照响应版本或头部无效");
    }
    IpcRuntimeSnapshot snapshot;
    snapshot.version = get_u16(body.data());
    const auto bus_health_count = get_u16(body.data() + 2U);
    snapshot.sequence = get_u64(body.data() + 4U);
    const auto node_count = get_u16(body.data() + 12U);
    const auto resource_count = get_u16(body.data() + 14U);
    const auto node_issue_count = get_u16(body.data() + 16U);
    const auto clock_count = get_u16(body.data() + 18U);
    const std::size_t node_size = get_u32(body.data() + 20U);
    const std::size_t traffic_size = get_u32(body.data() + 24U);
    const auto expected_size = kRuntimeSnapshotHeaderSize + node_size +
        traffic_size + static_cast<std::size_t>(resource_count) *
                           kRuntimeResourceSize +
        static_cast<std::size_t>(node_issue_count) * kRuntimeNodeIssueSize;
    const auto total_size = expected_size +
        static_cast<std::size_t>(clock_count) * kRuntimeClockQualitySize +
        static_cast<std::size_t>(bus_health_count) * kRuntimeBusHealthSize;
    if (snapshot.sequence == 0U || node_count > 127U ||
        resource_count > kMaximumRuntimeSnapshotResources ||
        node_issue_count > 127U || clock_count != node_count ||
        bus_health_count > kMaximumRuntimeSnapshotResources ||
        total_size != body.size()) {
        throw IpcException("Runtime 快照响应长度或条目数量无效");
    }
    auto cursor = body.begin() +
                  static_cast<std::ptrdiff_t>(kRuntimeSnapshotHeaderSize);
    const std::vector<std::uint8_t> node_body(
        cursor, cursor + static_cast<std::ptrdiff_t>(node_size));
    cursor += static_cast<std::ptrdiff_t>(node_size);
    snapshot.nodes = decode_ipc_node_list(node_body);
    if (snapshot.nodes.size() != node_count) {
        throw IpcException("Runtime 快照节点数量不一致");
    }
    const std::vector<std::uint8_t> traffic_body(
        cursor, cursor + static_cast<std::ptrdiff_t>(traffic_size));
    cursor += static_cast<std::ptrdiff_t>(traffic_size);
    snapshot.traffic = decode_ipc_traffic_status(traffic_body);
    snapshot.resources.reserve(resource_count);
    std::vector<std::uint32_t> node_ids;
    node_ids.reserve(snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        if (node.node_id == 0U || node.node_id > 127U ||
            std::find(node_ids.begin(), node_ids.end(), node.node_id) !=
                node_ids.end()) {
            throw IpcException("Runtime 快照节点 ID 无效或重复");
        }
        node_ids.push_back(node.node_id);
    }
    std::vector<std::uint64_t> identities;
    identities.reserve(resource_count);
    for (std::size_t index = 0U; index < resource_count; ++index) {
        const auto node_id = get_u32(&*cursor);
        cursor += 4;
        if (cursor[0] > 1U || cursor[1] != 0U ||
            get_u16(&cursor[2]) != 0U) {
            throw IpcException("Runtime 快照资源状态标志无效");
        }
        const bool status_valid = cursor[0] != 0U;
        cursor += 4;
        const std::vector<std::uint8_t> descriptor_body(cursor, cursor + 17);
        cursor += 17;
        const std::vector<std::uint8_t> status_body(cursor, cursor + 25);
        cursor += 25;
        const auto descriptor =
            protocol::decode_resource_descriptor(descriptor_body);
        const auto status = protocol::decode_resource_status(status_body);
        if (node_id == 0U || node_id > 127U ||
            descriptor.resource_id == 0U ||
            status.resource_id != descriptor.resource_id ||
            std::find(node_ids.begin(), node_ids.end(), node_id) ==
                node_ids.end()) {
            throw IpcException("Runtime 快照资源归属或状态 ID 无效");
        }
        const auto identity =
            (static_cast<std::uint64_t>(node_id) << 32U) |
            descriptor.resource_id;
        if (std::find(identities.begin(), identities.end(), identity) !=
            identities.end()) {
            throw IpcException("Runtime 快照包含重复资源");
        }
        identities.push_back(identity);
        snapshot.resources.push_back(
            {node_id, status_valid, descriptor, status});
    }
    snapshot.node_issues.reserve(node_issue_count);
    std::vector<std::uint32_t> issue_nodes;
    for (std::size_t index = 0U; index < node_issue_count; ++index) {
        const auto node_id = get_u32(&*cursor);
        const auto error = static_cast<IpcRuntimeNodeError>(cursor[4]);
        if (node_id == 0U || node_id > 127U ||
            error != IpcRuntimeNodeError::ResourceInventoryUnavailable ||
            cursor[5] != 0U || get_u16(&cursor[6]) != 0U ||
            std::find(node_ids.begin(), node_ids.end(), node_id) ==
                node_ids.end() ||
            std::find(issue_nodes.begin(), issue_nodes.end(), node_id) !=
                issue_nodes.end()) {
            throw IpcException("Runtime 快照节点错误项无效");
        }
        snapshot.node_issues.push_back({node_id, error});
        issue_nodes.push_back(node_id);
        cursor += static_cast<std::ptrdiff_t>(kRuntimeNodeIssueSize);
    }
    snapshot.clocks.reserve(clock_count);
    std::vector<std::uint32_t> clock_nodes;
    for (std::size_t index = 0U; index < clock_count; ++index) {
        IpcRuntimeClockQuality clock;
        clock.node_id = get_u32(&*cursor);
        const auto flags = cursor[4];
        const auto state = cursor[5];
        if ((flags & 0xFCU) != 0U || state >
                static_cast<std::uint8_t>(ClockSyncState::Degraded) ||
            get_u16(&cursor[6]) != 0U) {
            throw IpcException("Runtime 快照时钟质量标志无效");
        }
        clock.registered = (flags & 1U) != 0U;
        clock.estimate_valid = (flags & 2U) != 0U;
        clock.state = static_cast<ClockSyncState>(state);
        clock.boot_epoch = get_u64(&cursor[8]);
        clock.model_generation = get_u64(&cursor[16]);
        clock.sample_count = get_u16(&cursor[24]);
        clock.selected_sample_count = get_u16(&cursor[26]);
        clock.drift_uncertainty_ppm = get_u32(&cursor[28]);
        clock.rate_deviation_ppb = get_i32(&cursor[32]);
        clock.minimum_network_rtt_ns = get_u64(&cursor[36]);
        clock.error_bound_ns = get_u64(&cursor[44]);
        clock.sample_age_ns = get_u64(&cursor[52]);
        clock.last_sample_host_time_ns = get_u64(&cursor[60]);
        const bool node_known =
            std::find(node_ids.begin(), node_ids.end(), clock.node_id) !=
            node_ids.end();
        const bool duplicate = std::find(
            clock_nodes.begin(), clock_nodes.end(), clock.node_id) !=
            clock_nodes.end();
        const bool invalid_unregistered = !clock.registered &&
            (clock.estimate_valid || clock.state != ClockSyncState::Unsynced ||
             clock.boot_epoch != 0U || clock.model_generation != 0U ||
             clock.sample_count != 0U || clock.selected_sample_count != 0U ||
             clock.drift_uncertainty_ppm != 0U ||
             clock.rate_deviation_ppb != 0 ||
             clock.minimum_network_rtt_ns != 0U ||
             clock.error_bound_ns != 0U || clock.sample_age_ns != 0U ||
             clock.last_sample_host_time_ns != 0U);
        const bool invalid_unknown_estimate = !clock.estimate_valid &&
            (clock.drift_uncertainty_ppm != 0U ||
             clock.rate_deviation_ppb != 0 ||
             clock.minimum_network_rtt_ns != 0U ||
             clock.error_bound_ns != 0U || clock.sample_age_ns != 0U ||
             clock.last_sample_host_time_ns != 0U);
        if (!node_known || duplicate ||
            (clock.registered &&
             (clock.boot_epoch == 0U || clock.model_generation == 0U)) ||
            clock.selected_sample_count > clock.sample_count ||
            (!clock.estimate_valid && clock.state != ClockSyncState::Unsynced) ||
            invalid_unknown_estimate || invalid_unregistered) {
            throw IpcException("Runtime 快照时钟质量字段无效");
        }
        clock_nodes.push_back(clock.node_id);
        snapshot.clocks.push_back(clock);
        cursor += static_cast<std::ptrdiff_t>(kRuntimeClockQualitySize);
    }
    std::vector<std::uint64_t> bus_identities;
    snapshot.bus_health.reserve(bus_health_count);
    for (std::size_t index = 0U; index < bus_health_count; ++index) {
        IpcRuntimeBusHealth health;
        health.node_id = get_u32(&*cursor);
        health.resource_id = get_u32(&cursor[4]);
        const auto valid = cursor[8];
        const auto status = cursor[9];
        health.last_status_valid = valid != 0U;
        health.last_status = static_cast<protocol::BusTransactionStatus>(status);
        health.consecutive_failures = get_u32(&cursor[12]);
        health.peak_consecutive_failures = get_u32(&cursor[16]);
        health.last_result_time_us = get_u64(&cursor[20]);
        const auto identity = (static_cast<std::uint64_t>(health.node_id) << 32U) |
                              health.resource_id;
        if (valid > 1U || get_u16(&cursor[10]) != 0U ||
            status > static_cast<std::uint8_t>(
                protocol::BusTransactionStatus::LimitExceeded) ||
            (!health.last_status_valid &&
             (status != 0U || health.consecutive_failures != 0U ||
              health.peak_consecutive_failures != 0U ||
              health.last_result_time_us != 0U)) ||
            health.consecutive_failures > health.peak_consecutive_failures ||
            std::find(node_ids.begin(), node_ids.end(), health.node_id) ==
                node_ids.end() ||
            std::find(bus_identities.begin(), bus_identities.end(), identity) !=
                bus_identities.end()) {
            throw IpcException("Runtime 总线健康项无效");
        }
        bus_identities.push_back(identity);
        snapshot.bus_health.push_back(health);
        cursor += static_cast<std::ptrdiff_t>(kRuntimeBusHealthSize);
    }
    return snapshot;
}

std::vector<std::uint8_t> encode_ipc_daemon_identity(
    const IpcDaemonIdentity& identity) {
    if (identity.version != kDaemonIdentityIpcVersion ||
        std::all_of(identity.instance_id.begin(), identity.instance_id.end(),
                    [](std::uint8_t value) { return value == 0U; })) {
        throw IpcException("toolbusd 实例身份无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kDaemonIdentitySize);
    append_u16(body, identity.version);
    append_u16(body, 0U);
    body.insert(body.end(), identity.instance_id.begin(),
                identity.instance_id.end());
    return body;
}

IpcDaemonIdentity decode_ipc_daemon_identity(
    const std::vector<std::uint8_t>& body) {
    if (body.size() != kDaemonIdentitySize ||
        get_u16(body.data()) != kDaemonIdentityIpcVersion ||
        get_u16(body.data() + 2U) != 0U) {
        throw IpcException("toolbusd 实例身份版本或长度无效");
    }
    IpcDaemonIdentity identity;
    identity.version = get_u16(body.data());
    std::copy_n(body.begin() + 4, identity.instance_id.size(),
                identity.instance_id.begin());
    if (std::all_of(identity.instance_id.begin(), identity.instance_id.end(),
                    [](std::uint8_t value) { return value == 0U; })) {
        throw IpcException("toolbusd 实例身份不能为零");
    }
    return identity;
}

std::vector<std::uint8_t> encode_ipc_health_snapshot(
    const IpcToolbusdHealthSnapshot& snapshot) {
    if (snapshot.version != kHealthSnapshotIpcVersion ||
        std::all_of(snapshot.daemon_instance_id.begin(),
                    snapshot.daemon_instance_id.end(),
                    [](std::uint8_t value) { return value == 0U; }) ||
        snapshot.health.source != protocol::HealthSource::Toolbusd ||
        snapshot.health.node_id != 0U) {
        throw IpcException("toolbusd 健康快照身份无效");
    }
    const auto health = protocol::encode_health_snapshot(snapshot.health);
    if (health.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw IpcException("toolbusd 健康快照超过 IPC 上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kHealthSnapshotHeaderSize + health.size());
    append_u16(body, snapshot.version);
    append_u16(body, 0U);
    body.insert(body.end(), snapshot.daemon_instance_id.begin(),
                snapshot.daemon_instance_id.end());
    append_u16(body, static_cast<std::uint16_t>(health.size()));
    append_u16(body, 0U);
    body.insert(body.end(), health.begin(), health.end());
    return body;
}

IpcToolbusdHealthSnapshot decode_ipc_health_snapshot(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kHealthSnapshotHeaderSize ||
        get_u16(body.data()) != kHealthSnapshotIpcVersion ||
        get_u16(body.data() + 2U) != 0U ||
        get_u16(body.data() + 22U) != 0U) {
        throw IpcException("toolbusd 健康快照 IPC 头部无效");
    }
    const auto health_size = get_u16(body.data() + 20U);
    if (health_size == 0U ||
        body.size() != kHealthSnapshotHeaderSize + health_size) {
        throw IpcException("toolbusd 健康快照 IPC 长度无效");
    }
    IpcToolbusdHealthSnapshot snapshot;
    snapshot.version = get_u16(body.data());
    std::copy_n(body.begin() + 4U, snapshot.daemon_instance_id.size(),
                snapshot.daemon_instance_id.begin());
    if (std::all_of(snapshot.daemon_instance_id.begin(),
                    snapshot.daemon_instance_id.end(),
                    [](std::uint8_t value) { return value == 0U; })) {
        throw IpcException("toolbusd 健康快照 daemon 身份不能为零");
    }
    snapshot.health = protocol::decode_health_snapshot(
        {body.begin() + static_cast<std::ptrdiff_t>(kHealthSnapshotHeaderSize),
         body.end()});
    if (snapshot.health.source != protocol::HealthSource::Toolbusd ||
        snapshot.health.node_id != 0U) {
        throw IpcException("toolbusd 健康快照来源或节点无效");
    }
    return snapshot;
}

std::vector<std::uint8_t> encode_ipc_runtime_control_acquire(
    const RuntimeControlAcquireRequest& request) {
    if (request.version != kRuntimeControlIpcVersion ||
        request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes) {
        throw IpcException("Runtime 控制租约登记 IPC 字段超出上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeControlAcquireHeaderSize +
                 request.owner_key_id.size());
    append_u16(body, request.version);
    append_u16(body, request.permissions);
    body.insert(body.end(), request.daemon_instance_id.begin(),
                request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.expected_node_uuid.begin(),
                request.expected_node_uuid.end());
    append_u32(body, request.node_id);
    append_u32(body, request.resource_id);
    append_u32(body, request.ttl_ms);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.insert(body.end(), request.owner_key_id.begin(),
                request.owner_key_id.end());
    return body;
}

RuntimeControlAcquireRequest decode_ipc_runtime_control_acquire(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeControlAcquireHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion) {
        throw IpcException("Runtime 控制租约登记 IPC 版本或长度无效");
    }
    const auto owner_length = static_cast<std::size_t>(body[64U]);
    if (owner_length == 0U ||
        owner_length > kMaximumRuntimeControlIdentityBytes ||
        body.size() != kRuntimeControlAcquireHeaderSize + owner_length) {
        throw IpcException("Runtime 控制租约登记 IPC 身份长度无效");
    }
    RuntimeControlAcquireRequest request;
    request.version = get_u16(body.data());
    request.permissions = get_u16(body.data() + 2U);
    std::copy_n(body.begin() + 4U, request.daemon_instance_id.size(),
                request.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, request.lease_id.size(),
                request.lease_id.begin());
    std::copy_n(body.begin() + 36U, request.expected_node_uuid.size(),
                request.expected_node_uuid.begin());
    request.node_id = get_u32(body.data() + 52U);
    request.resource_id = get_u32(body.data() + 56U);
    request.ttl_ms = get_u32(body.data() + 60U);
    request.owner_key_id.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeControlAcquireHeaderSize),
        body.end());
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_gpio_write(
    const RuntimeGpioWriteRequest& request) {
    if (request.version != kRuntimeControlIpcVersion ||
        request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes ||
        request.idempotency_key.empty() ||
        request.idempotency_key.size() >
            kMaximumRuntimeControlIdempotencyBytes) {
        throw IpcException("Runtime GPIO 写 IPC 字段超出上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeGpioWriteHeaderSize + request.owner_key_id.size() +
                 request.idempotency_key.size());
    append_u16(body, request.version);
    append_u16(body, request.permissions);
    body.insert(body.end(), request.daemon_instance_id.begin(),
                request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.expected_node_uuid.begin(),
                request.expected_node_uuid.end());
    append_u32(body, request.node_id);
    append_u32(body, request.resource_id);
    body.push_back(request.value ? 1U : 0U);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.push_back(static_cast<std::uint8_t>(
        request.idempotency_key.size()));
    body.insert(body.end(), request.owner_key_id.begin(),
                request.owner_key_id.end());
    body.insert(body.end(), request.idempotency_key.begin(),
                request.idempotency_key.end());
    return body;
}

RuntimeGpioWriteRequest decode_ipc_runtime_gpio_write(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeGpioWriteHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion ||
        body[60U] > 1U) {
        throw IpcException("Runtime GPIO 写 IPC 版本、长度或布尔值无效");
    }
    const auto owner_length = static_cast<std::size_t>(body[61U]);
    const auto idempotency_length = static_cast<std::size_t>(body[62U]);
    if (owner_length == 0U ||
        owner_length > kMaximumRuntimeControlIdentityBytes ||
        idempotency_length == 0U ||
        idempotency_length > kMaximumRuntimeControlIdempotencyBytes ||
        body.size() != kRuntimeGpioWriteHeaderSize + owner_length +
                           idempotency_length) {
        throw IpcException("Runtime GPIO 写 IPC 字符串长度无效");
    }
    RuntimeGpioWriteRequest request;
    request.version = get_u16(body.data());
    request.permissions = get_u16(body.data() + 2U);
    std::copy_n(body.begin() + 4U, request.daemon_instance_id.size(),
                request.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, request.lease_id.size(),
                request.lease_id.begin());
    std::copy_n(body.begin() + 36U, request.expected_node_uuid.size(),
                request.expected_node_uuid.begin());
    request.node_id = get_u32(body.data() + 52U);
    request.resource_id = get_u32(body.data() + 56U);
    request.value = body[60U] != 0U;
    request.owner_key_id.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeGpioWriteHeaderSize),
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeGpioWriteHeaderSize + owner_length));
    request.idempotency_key.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeGpioWriteHeaderSize + owner_length),
        body.end());
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_pwm_request(
    const RuntimePwmConfigureRequest& request) {
    if (request.version != kRuntimeControlIpcVersion ||
        request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes ||
        request.idempotency_key.empty() ||
        request.idempotency_key.size() > kMaximumRuntimeControlIdempotencyBytes ||
        request.frequency_hz == 0U || request.duty > 10000U) {
        throw IpcException("Runtime PWM IPC 字段无效或超出上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimePwmRequestHeaderSize + request.owner_key_id.size() +
                 request.idempotency_key.size());
    append_u16(body, request.version);
    append_u16(body, request.permissions);
    body.insert(body.end(), request.daemon_instance_id.begin(), request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.expected_node_uuid.begin(), request.expected_node_uuid.end());
    append_u32(body, request.node_id);
    append_u32(body, request.resource_id);
    append_u32(body, request.frequency_hz);
    append_u16(body, request.duty);
    body.push_back(request.active_low ? 1U : 0U);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.push_back(static_cast<std::uint8_t>(request.idempotency_key.size()));
    body.push_back(0U);
    body.insert(body.end(), request.owner_key_id.begin(), request.owner_key_id.end());
    body.insert(body.end(), request.idempotency_key.begin(), request.idempotency_key.end());
    return body;
}

RuntimePwmConfigureRequest decode_ipc_runtime_pwm_request(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimePwmRequestHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion ||
        get_u32(body.data() + 60U) == 0U || get_u16(body.data() + 64U) > 10000U ||
        body[66U] > 1U || body[69U] != 0U) {
        throw IpcException("Runtime PWM IPC 版本、长度或数值无效");
    }
    const auto owner_length = static_cast<std::size_t>(body[67U]);
    const auto idempotency_length = static_cast<std::size_t>(body[68U]);
    if (owner_length == 0U || owner_length > kMaximumRuntimeControlIdentityBytes ||
        idempotency_length == 0U || idempotency_length > kMaximumRuntimeControlIdempotencyBytes ||
        body.size() != kRuntimePwmRequestHeaderSize + owner_length + idempotency_length) {
        throw IpcException("Runtime PWM IPC 字符串长度无效");
    }
    RuntimePwmConfigureRequest request;
    request.version = get_u16(body.data());
    request.permissions = get_u16(body.data() + 2U);
    std::copy_n(body.begin() + 4U, 16U, request.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, 16U, request.lease_id.begin());
    std::copy_n(body.begin() + 36U, 16U, request.expected_node_uuid.begin());
    request.node_id = get_u32(body.data() + 52U);
    request.resource_id = get_u32(body.data() + 56U);
    request.frequency_hz = get_u32(body.data() + 60U);
    request.duty = get_u16(body.data() + 64U);
    request.active_low = body[66U] != 0U;
    request.owner_key_id.assign(body.begin() + 70U, body.begin() + 70U + owner_length);
    request.idempotency_key.assign(body.begin() + 70U + owner_length, body.end());
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_pwm_stop_request(
    const RuntimePwmStopRequest& request) {
    if (request.version != kRuntimeControlIpcVersion || request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes ||
        request.idempotency_key.empty() ||
        request.idempotency_key.size() > kMaximumRuntimeControlIdempotencyBytes) {
        throw IpcException("Runtime PWM_STOP IPC 字段无效或超出上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimePwmStopRequestHeaderSize + request.owner_key_id.size() + request.idempotency_key.size());
    append_u16(body, request.version);
    append_u16(body, request.permissions);
    body.insert(body.end(), request.daemon_instance_id.begin(), request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.expected_node_uuid.begin(), request.expected_node_uuid.end());
    append_u32(body, request.node_id);
    append_u32(body, request.resource_id);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.push_back(static_cast<std::uint8_t>(request.idempotency_key.size()));
    append_u16(body, 0U);
    body.insert(body.end(), request.owner_key_id.begin(), request.owner_key_id.end());
    body.insert(body.end(), request.idempotency_key.begin(), request.idempotency_key.end());
    return body;
}

RuntimePwmStopRequest decode_ipc_runtime_pwm_stop_request(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimePwmStopRequestHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion || get_u16(body.data() + 62U) != 0U) {
        throw IpcException("Runtime PWM_STOP IPC 版本、长度或保留位无效");
    }
    const auto owner_length = static_cast<std::size_t>(body[60U]);
    const auto idempotency_length = static_cast<std::size_t>(body[61U]);
    if (owner_length == 0U || owner_length > kMaximumRuntimeControlIdentityBytes ||
        idempotency_length == 0U || idempotency_length > kMaximumRuntimeControlIdempotencyBytes ||
        body.size() != kRuntimePwmStopRequestHeaderSize + owner_length + idempotency_length) {
        throw IpcException("Runtime PWM_STOP IPC 字符串长度无效");
    }
    RuntimePwmStopRequest request;
    request.version = get_u16(body.data());
    request.permissions = get_u16(body.data() + 2U);
    std::copy_n(body.begin() + 4U, 16U, request.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, 16U, request.lease_id.begin());
    std::copy_n(body.begin() + 36U, 16U, request.expected_node_uuid.begin());
    request.node_id = get_u32(body.data() + 52U);
    request.resource_id = get_u32(body.data() + 56U);
    request.owner_key_id.assign(body.begin() + 64U, body.begin() + 64U + owner_length);
    request.idempotency_key.assign(body.begin() + 64U + owner_length, body.end());
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_timed_bitstream_configure(
    const RuntimeTimedBitstreamConfigureRequest& request) {
    try { static_cast<void>(protocol::encode_timed_bitstream_create({0U,
        request.bit_period_ns, request.zero_high_ns, request.one_high_ns,
        request.reset_time_us})); }
    catch (const protocol::WaveformPayloadException&) {
        throw IpcException("Runtime定时位流配置数值无效");
    }
    if (request.version != kRuntimeControlIpcVersion || request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes ||
        request.idempotency_key.empty() ||
        request.idempotency_key.size() > kMaximumRuntimeControlIdempotencyBytes)
        throw IpcException("Runtime定时位流配置IPC字段无效");
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeTimedBitstreamConfigureHeaderSize + request.owner_key_id.size() +
                 request.idempotency_key.size());
    append_u16(body, request.version); append_u16(body, request.permissions);
    body.insert(body.end(), request.daemon_instance_id.begin(), request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.expected_node_uuid.begin(), request.expected_node_uuid.end());
    append_u32(body, request.node_id); append_u32(body, request.resource_id);
    append_u32(body, request.bit_period_ns); append_u32(body, request.zero_high_ns);
    append_u32(body, request.one_high_ns); append_u32(body, request.reset_time_us);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.push_back(static_cast<std::uint8_t>(request.idempotency_key.size())); append_u16(body, 0U);
    body.insert(body.end(), request.owner_key_id.begin(), request.owner_key_id.end());
    body.insert(body.end(), request.idempotency_key.begin(), request.idempotency_key.end());
    return body;
}

RuntimeTimedBitstreamConfigureRequest decode_ipc_runtime_timed_bitstream_configure(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeTimedBitstreamConfigureHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion || get_u16(body.data() + 78U) != 0U)
        throw IpcException("Runtime定时位流配置IPC版本、长度或保留位无效");
    const auto owner = body[76U], idem = body[77U];
    if (owner == 0U || owner > kMaximumRuntimeControlIdentityBytes || idem == 0U ||
        idem > kMaximumRuntimeControlIdempotencyBytes ||
        body.size() != kRuntimeTimedBitstreamConfigureHeaderSize + owner + idem)
        throw IpcException("Runtime定时位流配置IPC字符串长度无效");
    RuntimeTimedBitstreamConfigureRequest request;
    request.version = get_u16(body.data()); request.permissions = get_u16(body.data() + 2U);
    std::copy_n(body.begin()+4U,16U,request.daemon_instance_id.begin());
    std::copy_n(body.begin()+20U,16U,request.lease_id.begin());
    std::copy_n(body.begin()+36U,16U,request.expected_node_uuid.begin());
    request.node_id=get_u32(body.data()+52U); request.resource_id=get_u32(body.data()+56U);
    request.bit_period_ns=get_u32(body.data()+60U); request.zero_high_ns=get_u32(body.data()+64U);
    request.one_high_ns=get_u32(body.data()+68U); request.reset_time_us=get_u32(body.data()+72U);
    request.owner_key_id.assign(body.begin()+80U,body.begin()+80U+owner);
    request.idempotency_key.assign(body.begin()+80U+owner,body.end());
    try { static_cast<void>(protocol::encode_timed_bitstream_create({0U,
        request.bit_period_ns, request.zero_high_ns, request.one_high_ns, request.reset_time_us})); }
    catch (const protocol::WaveformPayloadException&) { throw IpcException("Runtime定时位流配置数值无效"); }
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_timed_bitstream_frame(
    const RuntimeTimedBitstreamFrameRequest& request) {
    try { static_cast<void>(protocol::encode_timed_bitstream_write({request.bit_count,request.data})); }
    catch (const protocol::WaveformPayloadException&) { throw IpcException("Runtime定时位流帧载荷无效"); }
    if (request.version != kRuntimeControlIpcVersion || request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes || request.idempotency_key.empty() ||
        request.idempotency_key.size() > kMaximumRuntimeControlIdempotencyBytes)
        throw IpcException("Runtime定时位流帧IPC字段无效");
    std::vector<std::uint8_t> body; body.reserve(kRuntimeTimedBitstreamFrameHeaderSize +
        request.owner_key_id.size()+request.idempotency_key.size()+request.data.size());
    append_u16(body,request.version); append_u16(body,request.permissions);
    body.insert(body.end(),request.daemon_instance_id.begin(),request.daemon_instance_id.end());
    body.insert(body.end(),request.lease_id.begin(),request.lease_id.end());
    body.insert(body.end(),request.expected_node_uuid.begin(),request.expected_node_uuid.end());
    append_u32(body,request.node_id); append_u32(body,request.resource_id);
    append_u16(body,request.bit_count); append_u16(body,static_cast<std::uint16_t>(request.data.size()));
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.push_back(static_cast<std::uint8_t>(request.idempotency_key.size())); append_u16(body,0U);
    body.insert(body.end(),request.owner_key_id.begin(),request.owner_key_id.end());
    body.insert(body.end(),request.idempotency_key.begin(),request.idempotency_key.end());
    body.insert(body.end(),request.data.begin(),request.data.end()); return body;
}

RuntimeTimedBitstreamFrameRequest decode_ipc_runtime_timed_bitstream_frame(
    const std::vector<std::uint8_t>& body) {
    if (body.size()<kRuntimeTimedBitstreamFrameHeaderSize || get_u16(body.data())!=kRuntimeControlIpcVersion ||
        get_u16(body.data()+66U)!=0U) throw IpcException("Runtime定时位流帧IPC头无效");
    const auto data_size=get_u16(body.data()+62U); const auto owner=body[64U],idem=body[65U];
    if(owner==0U||owner>kMaximumRuntimeControlIdentityBytes||idem==0U||idem>kMaximumRuntimeControlIdempotencyBytes||
       data_size>protocol::kMaximumTimedBitstreamDataBytes||body.size()!=68U+owner+idem+data_size)
        throw IpcException("Runtime定时位流帧IPC长度无效");
    RuntimeTimedBitstreamFrameRequest request; request.version=get_u16(body.data());request.permissions=get_u16(body.data()+2U);
    std::copy_n(body.begin()+4U,16U,request.daemon_instance_id.begin());std::copy_n(body.begin()+20U,16U,request.lease_id.begin());
    std::copy_n(body.begin()+36U,16U,request.expected_node_uuid.begin());request.node_id=get_u32(body.data()+52U);
    request.resource_id=get_u32(body.data()+56U);request.bit_count=get_u16(body.data()+60U);
    request.owner_key_id.assign(body.begin()+68U,body.begin()+68U+owner);
    request.idempotency_key.assign(body.begin()+68U+owner,body.begin()+68U+owner+idem);
    request.data.assign(body.end()-data_size,body.end());
    try { static_cast<void>(protocol::encode_timed_bitstream_write({request.bit_count,request.data})); }
    catch (const protocol::WaveformPayloadException&) { throw IpcException("Runtime定时位流帧正文无效"); }
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_timed_bitstream_stop(
    const RuntimeTimedBitstreamStopRequest& request) {
    RuntimePwmStopRequest common; common.version=request.version; common.daemon_instance_id=request.daemon_instance_id;
    common.lease_id=request.lease_id; common.expected_node_uuid=request.expected_node_uuid;
    common.owner_key_id=request.owner_key_id; common.permissions=request.permissions; common.node_id=request.node_id;
    common.resource_id=request.resource_id; common.idempotency_key=request.idempotency_key;
    return encode_ipc_runtime_pwm_stop_request(common);
}

RuntimeTimedBitstreamStopRequest decode_ipc_runtime_timed_bitstream_stop(
    const std::vector<std::uint8_t>& body) {
    const auto common=decode_ipc_runtime_pwm_stop_request(body); RuntimeTimedBitstreamStopRequest request;
    request.version=common.version;request.daemon_instance_id=common.daemon_instance_id;request.lease_id=common.lease_id;
    request.expected_node_uuid=common.expected_node_uuid;request.owner_key_id=common.owner_key_id;
    request.permissions=common.permissions;request.node_id=common.node_id;request.resource_id=common.resource_id;
    request.idempotency_key=common.idempotency_key;return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_control_release(
    const RuntimeControlReleaseRequest& request) {
    if (request.version != kRuntimeControlIpcVersion ||
        request.owner_key_id.empty() ||
        request.owner_key_id.size() > kMaximumRuntimeControlIdentityBytes) {
        throw IpcException("Runtime 控制租约释放 IPC 字段超出上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeControlReleaseHeaderSize +
                 request.owner_key_id.size());
    append_u16(body, request.version);
    body.push_back(static_cast<std::uint8_t>(request.owner_key_id.size()));
    body.insert(body.end(), request.daemon_instance_id.begin(),
                request.daemon_instance_id.end());
    body.insert(body.end(), request.lease_id.begin(), request.lease_id.end());
    body.insert(body.end(), request.owner_key_id.begin(),
                request.owner_key_id.end());
    return body;
}

RuntimeControlReleaseRequest decode_ipc_runtime_control_release(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeControlReleaseHeaderSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion) {
        throw IpcException("Runtime 控制租约释放 IPC 版本或长度无效");
    }
    const auto owner_length = static_cast<std::size_t>(body[2U]);
    if (owner_length == 0U ||
        owner_length > kMaximumRuntimeControlIdentityBytes ||
        body.size() != kRuntimeControlReleaseHeaderSize + owner_length) {
        throw IpcException("Runtime 控制租约释放 IPC 身份长度无效");
    }
    RuntimeControlReleaseRequest request;
    request.version = get_u16(body.data());
    std::copy_n(body.begin() + 3U, request.daemon_instance_id.size(),
                request.daemon_instance_id.begin());
    std::copy_n(body.begin() + 19U, request.lease_id.size(),
                request.lease_id.begin());
    request.owner_key_id.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeControlReleaseHeaderSize),
        body.end());
    return request;
}

std::vector<std::uint8_t> encode_ipc_runtime_gpio_write_result(
    const RuntimeGpioWriteResult& result) {
    if (result.version != kRuntimeControlIpcVersion ||
        result.object_id == 0U) {
        throw IpcException("Runtime GPIO 写结果字段无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeGpioWriteResultSize);
    append_u16(body, result.version);
    append_u32(body, result.object_id);
    body.push_back(result.value ? 1U : 0U);
    body.push_back(result.replayed ? 1U : 0U);
    return body;
}

RuntimeGpioWriteResult decode_ipc_runtime_gpio_write_result(
    const std::vector<std::uint8_t>& body) {
    if (body.size() != kRuntimeGpioWriteResultSize ||
        get_u16(body.data()) != kRuntimeControlIpcVersion ||
        get_u32(body.data() + 2U) == 0U || body[6U] > 1U || body[7U] > 1U) {
        throw IpcException("Runtime GPIO 写结果版本、长度或字段无效");
    }
    return {get_u16(body.data()), get_u32(body.data() + 2U),
            body[6U] != 0U, body[7U] != 0U};
}

std::vector<std::uint8_t> encode_ipc_runtime_operation_query(
    const RuntimeOperationQuery& query) {
    if (query.version != kRuntimeOperationIpcVersion ||
        !nonzero_id(query.daemon_instance_id) ||
        !nonzero_id(query.operation_id) ||
        !valid_operation_text(query.owner_key_id,
                              kMaximumRuntimeControlIdentityBytes, false)) {
        throw IpcException("Runtime 操作查询字段无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeOperationQueryHeaderSize + query.owner_key_id.size());
    append_u16(body, query.version);
    append_u16(body, static_cast<std::uint16_t>(
                         kRuntimeOperationQueryHeaderSize));
    body.insert(body.end(), query.daemon_instance_id.begin(),
                query.daemon_instance_id.end());
    body.insert(body.end(), query.operation_id.begin(),
                query.operation_id.end());
    append_u16(body, static_cast<std::uint16_t>(query.owner_key_id.size()));
    append_u16(body, 0U);
    body.insert(body.end(), query.owner_key_id.begin(),
                query.owner_key_id.end());
    return body;
}

RuntimeOperationQuery decode_ipc_runtime_operation_query(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeOperationQueryHeaderSize ||
        get_u16(body.data()) != kRuntimeOperationIpcVersion ||
        get_u16(body.data() + 2U) != kRuntimeOperationQueryHeaderSize ||
        get_u16(body.data() + 54U) != 0U) {
        throw IpcException("Runtime 操作查询版本、长度或保留位无效");
    }
    const auto owner_size = get_u16(body.data() + 52U);
    if (owner_size == 0U ||
        owner_size > kMaximumRuntimeControlIdentityBytes ||
        body.size() != kRuntimeOperationQueryHeaderSize + owner_size) {
        throw IpcException("Runtime 操作查询身份长度无效");
    }
    RuntimeOperationQuery query;
    std::copy_n(body.begin() + 4U, query.daemon_instance_id.size(),
                query.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, query.operation_id.size(),
                query.operation_id.begin());
    query.owner_key_id.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kRuntimeOperationQueryHeaderSize),
        body.end());
    if (!nonzero_id(query.daemon_instance_id) ||
        !nonzero_id(query.operation_id) ||
        !valid_operation_text(query.owner_key_id,
                              kMaximumRuntimeControlIdentityBytes, false)) {
        throw IpcException("Runtime 操作查询身份或ID无效");
    }
    return query;
}

std::vector<std::uint8_t> encode_ipc_runtime_operation_lookup(
    const RuntimeOperationLookup& lookup) {
    if (lookup.version != kRuntimeOperationIpcVersion ||
        !nonzero_id(lookup.daemon_instance_id) ||
        !nonzero_id(lookup.lease_id) || !valid_operation_kind(lookup.kind) ||
        !valid_operation_text(lookup.owner_key_id,
                              kMaximumRuntimeControlIdentityBytes, false) ||
        !valid_operation_text(lookup.idempotency_key,
                              kMaximumRuntimeControlIdempotencyBytes, true) ||
        (lookup.kind == RuntimeOperationKind::ControlRelease &&
         lookup.idempotency_key != "release:v1")) {
        throw IpcException("Runtime 操作定位字段无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kRuntimeOperationLookupHeaderSize +
                 lookup.owner_key_id.size() + lookup.idempotency_key.size());
    append_u16(body, lookup.version);
    append_u16(body, static_cast<std::uint16_t>(
                         kRuntimeOperationLookupHeaderSize));
    body.insert(body.end(), lookup.daemon_instance_id.begin(),
                lookup.daemon_instance_id.end());
    body.insert(body.end(), lookup.lease_id.begin(), lookup.lease_id.end());
    body.push_back(static_cast<std::uint8_t>(lookup.kind));
    body.push_back(0U);
    append_u16(body, static_cast<std::uint16_t>(lookup.owner_key_id.size()));
    append_u16(body, static_cast<std::uint16_t>(
                         lookup.idempotency_key.size()));
    append_u16(body, 0U);
    body.insert(body.end(), lookup.owner_key_id.begin(),
                lookup.owner_key_id.end());
    body.insert(body.end(), lookup.idempotency_key.begin(),
                lookup.idempotency_key.end());
    return body;
}

RuntimeOperationLookup decode_ipc_runtime_operation_lookup(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeOperationLookupHeaderSize ||
        get_u16(body.data()) != kRuntimeOperationIpcVersion ||
        get_u16(body.data() + 2U) != kRuntimeOperationLookupHeaderSize ||
        body[37U] != 0U || get_u16(body.data() + 42U) != 0U) {
        throw IpcException("Runtime 操作定位版本、长度或保留位无效");
    }
    const auto owner_size = get_u16(body.data() + 38U);
    const auto idempotency_size = get_u16(body.data() + 40U);
    if (owner_size == 0U ||
        owner_size > kMaximumRuntimeControlIdentityBytes ||
        idempotency_size == 0U ||
        idempotency_size > kMaximumRuntimeControlIdempotencyBytes ||
        body.size() != kRuntimeOperationLookupHeaderSize + owner_size +
                           idempotency_size) {
        throw IpcException("Runtime 操作定位字符串长度无效");
    }
    RuntimeOperationLookup lookup;
    lookup.kind = static_cast<RuntimeOperationKind>(body[36U]);
    std::copy_n(body.begin() + 4U, lookup.daemon_instance_id.size(),
                lookup.daemon_instance_id.begin());
    std::copy_n(body.begin() + 20U, lookup.lease_id.size(),
                lookup.lease_id.begin());
    const auto strings = body.begin() + static_cast<std::ptrdiff_t>(
                                      kRuntimeOperationLookupHeaderSize);
    lookup.owner_key_id.assign(
        strings, strings + static_cast<std::ptrdiff_t>(owner_size));
    lookup.idempotency_key.assign(
        strings + static_cast<std::ptrdiff_t>(owner_size), body.end());
    if (!nonzero_id(lookup.daemon_instance_id) ||
        !nonzero_id(lookup.lease_id) || !valid_operation_kind(lookup.kind) ||
        !valid_operation_text(lookup.owner_key_id,
                              kMaximumRuntimeControlIdentityBytes, false) ||
        !valid_operation_text(lookup.idempotency_key,
                              kMaximumRuntimeControlIdempotencyBytes, true) ||
        (lookup.kind == RuntimeOperationKind::ControlRelease &&
         lookup.idempotency_key != "release:v1")) {
        throw IpcException("Runtime 操作定位身份、选择器或ID无效");
    }
    return lookup;
}

std::vector<std::uint8_t> encode_ipc_runtime_operation_outcome(
    const RuntimeOperationOutcome& outcome) {
    if (!valid_operation_outcome(outcome)) {
        throw IpcException("Runtime 操作结果状态组合无效");
    }
    const bool has_result = outcome.object_id != 0U;
    const bool has_error = outcome.error != RuntimeOperationError::None;
    std::vector<std::uint8_t> body;
    const bool pwm = outcome.kind == RuntimeOperationKind::PwmConfigure ||
                     outcome.kind == RuntimeOperationKind::PwmStop;
    const auto encoded_size = pwm ? kRuntimePwmOperationOutcomeSize
                                  : kRuntimeOperationOutcomeSize;
    body.reserve(encoded_size);
    append_u16(body, outcome.version);
    append_u16(body, static_cast<std::uint16_t>(
                         encoded_size));
    body.push_back(static_cast<std::uint8_t>(outcome.kind));
    body.push_back(static_cast<std::uint8_t>(outcome.state));
    body.push_back(static_cast<std::uint8_t>(outcome.recovery));
    body.push_back(static_cast<std::uint8_t>(
        (outcome.replayed ? 0x01U : 0U) | (has_result ? 0x02U : 0U) |
        (has_error ? 0x04U : 0U)));
    body.insert(body.end(), outcome.operation_id.begin(),
                outcome.operation_id.end());
    body.insert(body.end(), outcome.lease_id.begin(), outcome.lease_id.end());
    body.insert(body.end(), outcome.expected_node_uuid.begin(),
                outcome.expected_node_uuid.end());
    append_u32(body, outcome.resource_id);
    append_u32(body, outcome.object_id);
    append_u16(body, static_cast<std::uint16_t>(outcome.error));
    body.push_back(outcome.value ? 1U : 0U);
    if (pwm) {
        append_u32(body, outcome.frequency_hz);
        append_u16(body, outcome.duty);
        body.push_back(outcome.active_low ? 1U : 0U);
        body.push_back(0U);
        body.insert(body.end(), 5U, 0U);
    } else {
        body.insert(body.end(), 5U, 0U);
    }
    return body;
}

RuntimeOperationOutcome decode_ipc_runtime_operation_outcome(
    const std::vector<std::uint8_t>& body) {
    if ((body.size() != kRuntimeOperationOutcomeSize &&
         body.size() != kRuntimePwmOperationOutcomeSize) ||
        get_u16(body.data()) != kRuntimeOperationIpcVersion ||
        get_u16(body.data() + 2U) != body.size() ||
        (body[7U] & ~0x07U) != 0U || body[82U] > 1U) {
        throw IpcException("Runtime 操作结果版本、长度或保留位无效");
    }
    RuntimeOperationOutcome outcome;
    outcome.kind = static_cast<RuntimeOperationKind>(body[4U]);
    outcome.state = static_cast<RuntimeOperationState>(body[5U]);
    outcome.recovery = static_cast<RuntimeOperationRecovery>(body[6U]);
    outcome.replayed = (body[7U] & 0x01U) != 0U;
    std::copy_n(body.begin() + 8U, outcome.operation_id.size(),
                outcome.operation_id.begin());
    std::copy_n(body.begin() + 40U, outcome.lease_id.size(),
                outcome.lease_id.begin());
    std::copy_n(body.begin() + 56U, outcome.expected_node_uuid.size(),
                outcome.expected_node_uuid.begin());
    outcome.resource_id = get_u32(body.data() + 72U);
    outcome.object_id = get_u32(body.data() + 76U);
    outcome.error = static_cast<RuntimeOperationError>(
        get_u16(body.data() + 80U));
    outcome.value = body[82U] != 0U;
    if (body.size() == kRuntimePwmOperationOutcomeSize) {
        if (body[89U] > 1U || body[90U] != 0U ||
            std::any_of(body.begin() + 91U, body.end(), [](std::uint8_t byte) { return byte != 0U; }))
            throw IpcException("Runtime PWM 操作结果保留位无效");
        outcome.frequency_hz = get_u32(body.data() + 83U);
        outcome.duty = get_u16(body.data() + 87U);
        outcome.active_low = body[89U] != 0U;
    } else if (std::any_of(body.begin() + 83U, body.end(), [](std::uint8_t byte) { return byte != 0U; })) {
        throw IpcException("Runtime 操作结果保留位无效");
    }
    const bool encoded_result = (body[7U] & 0x02U) != 0U;
    const bool encoded_error = (body[7U] & 0x04U) != 0U;
    if (encoded_result != (outcome.object_id != 0U) ||
        encoded_error != (outcome.error != RuntimeOperationError::None) ||
        !valid_operation_outcome(outcome)) {
        throw IpcException("Runtime 操作结果标志或状态组合无效");
    }
    return outcome;
}

const char* runtime_operation_kind_name(RuntimeOperationKind kind) noexcept {
    switch (kind) {
        case RuntimeOperationKind::Unknown: return "unknown";
        case RuntimeOperationKind::GpioWrite: return "gpio_write";
        case RuntimeOperationKind::ControlRelease: return "control_release";
        case RuntimeOperationKind::PwmConfigure: return "pwm_configure";
        case RuntimeOperationKind::PwmStop: return "pwm_stop";
        case RuntimeOperationKind::TimedBitstreamConfigure:
            return "timed_bitstream_configure";
        case RuntimeOperationKind::TimedBitstreamFrame:
            return "timed_bitstream_frame";
        case RuntimeOperationKind::TimedBitstreamStop:
            return "timed_bitstream_stop";
    }
    return "unknown";
}

const char* runtime_operation_state_name(RuntimeOperationState state) noexcept {
    switch (state) {
        case RuntimeOperationState::Pending: return "pending";
        case RuntimeOperationState::Committed: return "committed";
        case RuntimeOperationState::Rejected: return "rejected";
        case RuntimeOperationState::Unknown: return "unknown";
        case RuntimeOperationState::ExpiredUnknown: return "expired_unknown";
    }
    return "unknown_enum";
}

const char* runtime_operation_recovery_name(
    RuntimeOperationRecovery recovery) noexcept {
    switch (recovery) {
        case RuntimeOperationRecovery::None: return "none";
        case RuntimeOperationRecovery::NotSent: return "not_sent";
        case RuntimeOperationRecovery::SafeClosed: return "safe_closed";
        case RuntimeOperationRecovery::ScopeBlocked: return "scope_blocked";
        case RuntimeOperationRecovery::AwaitingReboot: return "awaiting_reboot";
        case RuntimeOperationRecovery::NodeRebootConfirmed:
            return "node_reboot_confirmed";
    }
    return "unknown";
}

const char* runtime_operation_error_name(RuntimeOperationError error) noexcept {
    switch (error) {
        case RuntimeOperationError::None: return "none";
        case RuntimeOperationError::Rejected: return "rejected";
        case RuntimeOperationError::Deadline: return "deadline";
        case RuntimeOperationError::Backend: return "backend";
        case RuntimeOperationError::Persistence: return "persistence";
        case RuntimeOperationError::HistoryExpired: return "history_expired";
    }
    return "unknown";
}

std::vector<std::uint8_t> encode_ipc_error_envelope(
    const IpcErrorEnvelope& error) {
    const auto expected = expected_error_category(error.code);
    if (error.version != kIpcErrorEnvelopeVersion ||
        !expected.has_value() || *expected != error.category ||
        !error_flags_allowed(error.code, error.retryable,
                             error.possibly_committed) ||
        !safe_error_message(error.message)) {
        throw IpcException("本地 IPC 错误信封字段无效");
    }
    std::vector<std::uint8_t> body;
    body.reserve(kIpcErrorEnvelopeHeaderSize + error.message.size());
    append_u16(body, error.version);
    append_u16(body, static_cast<std::uint16_t>(
                         kIpcErrorEnvelopeHeaderSize));
    append_u16(body, static_cast<std::uint16_t>(error.code));
    body.push_back(static_cast<std::uint8_t>(error.category));
    body.push_back(static_cast<std::uint8_t>(
        (error.retryable ? 0x01U : 0U) |
        (error.possibly_committed ? 0x02U : 0U)));
    append_u16(body, static_cast<std::uint16_t>(error.message.size()));
    append_u16(body, 0U);
    body.insert(body.end(), error.message.begin(), error.message.end());
    return body;
}

IpcErrorEnvelope decode_ipc_error_envelope(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kIpcErrorEnvelopeHeaderSize ||
        get_u16(body.data()) != kIpcErrorEnvelopeVersion ||
        get_u16(body.data() + 2U) != kIpcErrorEnvelopeHeaderSize ||
        (body[7U] & ~0x03U) != 0U || get_u16(body.data() + 10U) != 0U) {
        throw IpcException("本地 IPC 错误信封版本或头部无效");
    }
    const auto code = static_cast<IpcErrorCode>(get_u16(body.data() + 4U));
    const auto category = static_cast<IpcErrorCategory>(body[6U]);
    const auto expected = expected_error_category(code);
    const auto message_size = get_u16(body.data() + 8U);
    const bool possibly_committed = (body[7U] & 0x02U) != 0U;
    if (!expected.has_value() || *expected != category ||
        !error_flags_allowed(code, (body[7U] & 0x01U) != 0U,
                             possibly_committed) ||
        message_size == 0U || message_size > kMaximumIpcErrorMessageBytes ||
        body.size() != kIpcErrorEnvelopeHeaderSize + message_size) {
        throw IpcException("本地 IPC 错误信封代码、类别或长度无效");
    }
    IpcErrorEnvelope error;
    error.code = code;
    error.category = category;
    error.retryable = (body[7U] & 0x01U) != 0U;
    error.possibly_committed = possibly_committed;
    error.message.assign(
        body.begin() + static_cast<std::ptrdiff_t>(
                           kIpcErrorEnvelopeHeaderSize),
        body.end());
    if (!safe_error_message(error.message)) {
        throw IpcException("本地 IPC 错误信封消息无效");
    }
    return error;
}

const char* ipc_error_code_name(IpcErrorCode code) noexcept {
    switch (code) {
        case IpcErrorCode::InvalidRequest: return "invalid_request";
        case IpcErrorCode::UnsupportedRequest: return "unsupported_request";
        case IpcErrorCode::DaemonIdentityMismatch:
            return "daemon_identity_mismatch";
        case IpcErrorCode::PermissionDenied: return "permission_denied";
        case IpcErrorCode::LeaseConflict: return "lease_conflict";
        case IpcErrorCode::LeaseNotFound: return "lease_not_found";
        case IpcErrorCode::LeaseExpired: return "lease_expired";
        case IpcErrorCode::ContractRejected: return "contract_rejected";
        case IpcErrorCode::CapacityExceeded: return "capacity_exceeded";
        case IpcErrorCode::IdempotencyConflict:
            return "idempotency_conflict";
        case IpcErrorCode::SafeStopFailed: return "safe_stop_failed";
        case IpcErrorCode::ObjectRetired: return "object_retired";
        case IpcErrorCode::NodeUnavailable: return "node_unavailable";
        case IpcErrorCode::BackendUnavailable: return "backend_unavailable";
        case IpcErrorCode::DeadlineExceeded: return "deadline_exceeded";
        case IpcErrorCode::HealthUnavailable: return "health_unavailable";
        case IpcErrorCode::InternalFailure: return "internal_failure";
    }
    return "unknown";
}

const char* ipc_error_category_name(IpcErrorCategory category) noexcept {
    switch (category) {
        case IpcErrorCategory::Request: return "request";
        case IpcErrorCategory::Authentication: return "authentication";
        case IpcErrorCategory::Authorization: return "authorization";
        case IpcErrorCategory::Conflict: return "conflict";
        case IpcErrorCategory::Unavailable: return "unavailable";
        case IpcErrorCategory::Timeout: return "timeout";
        case IpcErrorCategory::Internal: return "internal";
    }
    return "unknown";
}

void write_ipc_response(int socket, IpcStatus status,
                        const std::vector<std::uint8_t>& body) {
    const std::uint8_t status_byte = static_cast<std::uint8_t>(status);
    send_all(socket, &status_byte, 1);
    send_body(socket, body);
}

std::vector<std::uint8_t> encode_ipc_logical_recording_status(
    const IpcLogicalRecordingStatus& status) {
    if (status.version != kLogicalRecordingIpcVersion ||
        status.evidence_scope != "logical-link-boundary-only" ||
        status.evidence_scope.size() > 255U ||
        status.output_name.size() > 255U) {
        throw IpcException("逻辑链路录制状态无效");
    }
    std::vector<std::uint8_t> out;
    append_u16(out, status.version);
    out.push_back(status.configured ? 1U : 0U);
    out.push_back(status.active ? 1U : 0U);
    append_u64(out, status.event_count);
    append_u64(out, status.maximum_events);
    append_u64(out, status.maximum_file_bytes);
    out.push_back(static_cast<std::uint8_t>(status.evidence_scope.size()));
    out.push_back(static_cast<std::uint8_t>(status.output_name.size()));
    out.insert(out.end(), status.evidence_scope.begin(),
               status.evidence_scope.end());
    out.insert(out.end(), status.output_name.begin(),
               status.output_name.end());
    return out;
}

IpcLogicalRecordingStatus decode_ipc_logical_recording_status(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < 30U ||
        get_u16(body.data()) != kLogicalRecordingIpcVersion ||
        body[2] > 1U || body[3] > 1U) {
        throw IpcException("逻辑链路录制状态载荷无效");
    }
    const auto scope_size = body[28];
    const auto name_size = body[29];
    if (body.size() != 30U + scope_size + name_size) {
        throw IpcException("逻辑链路录制状态长度无效");
    }
    IpcLogicalRecordingStatus result;
    result.configured = body[2] != 0U;
    result.active = body[3] != 0U;
    result.event_count = get_u64(body.data() + 4U);
    result.maximum_events = get_u64(body.data() + 12U);
    result.maximum_file_bytes = get_u64(body.data() + 20U);
    result.evidence_scope.assign(body.begin() + 30U,
                                 body.begin() + 30U + scope_size);
    result.output_name.assign(body.begin() + 30U + scope_size, body.end());
    if (result.evidence_scope != "logical-link-boundary-only") {
        throw IpcException("逻辑链路录制证据范围无效");
    }
    return result;
}

IpcResponse read_ipc_response(int socket) {
    std::uint8_t status_byte{};
    receive_all(socket, &status_byte, 1);
    if (status_byte > static_cast<std::uint8_t>(IpcStatus::Error)) {
        throw IpcException("本地 IPC 响应状态无效");
    }
    return {static_cast<IpcStatus>(status_byte), receive_body(socket)};
}

}
