#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/protocol/gpio.hpp"
#include "remotebsp/protocol/waveform.hpp"
#include "remotebsp/toolbusd/bus_runtime.hpp"
#include "remotebsp/toolbusd/clock_sync_manager.hpp"
#include "remotebsp/toolbusd/health_producer.hpp"
#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/link_recording.hpp"
#include "remotebsp/toolbusd/motion_group_service.hpp"
#include "remotebsp/toolbusd/motion_group_dispatch_gate.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"
#include "remotebsp/toolbusd/operation_ledger.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"
#include "remotebsp/toolbusd/runtime_control.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"
#include "remotebsp/transport/link_routes.hpp"
#include "remotebsp/transport/link_transport.hpp"
#include "remotebsp/transport/libusb_transport.hpp"
#include "remotebsp/transport/mock_usb_transport.hpp"
#include "remotebsp/transport/socketcan_transport.hpp"

#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr auto kBroadcastRequestRoute =
    remotebsp::transport::kDiscoveryRoute;
constexpr auto kNodeRequestBaseRoute =
    remotebsp::transport::kNodeRequestBaseRoute;
constexpr auto kNodeResponseBaseRoute =
    remotebsp::transport::kNodeResponseBaseRoute;
constexpr auto kNodeEventBaseRoute =
    remotebsp::transport::kNodeEventBaseRoute;
constexpr auto kProvisionalResponseBaseRoute =
    remotebsp::transport::kProvisionalResponseBaseRoute;
constexpr auto kMaximumNodeId = remotebsp::transport::kMaximumNodeId;
constexpr std::size_t kMaximumQueuedEventsPerNode = 256;
constexpr std::size_t kUartStreamBufferCapacity = 64U * 1024U;
constexpr auto kDiscoveryInterval = std::chrono::milliseconds(2000);

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) { stop_requested = 1; }

remotebsp::transport::CanMode parse_mode(const std::string& text) {
    if (text == "classical") {
        return remotebsp::transport::CanMode::Classical;
    }
    if (text == "fd") {
        return remotebsp::transport::CanMode::FlexibleDataRate;
    }
    throw std::invalid_argument("模式必须是 classical 或 fd");
}

std::uint64_t request_key(std::uint32_t session_id,
                          std::uint32_t request_id) {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

std::uint64_t uart_stream_key(std::uint32_t node_id,
                              std::uint32_t object_id) {
    return (static_cast<std::uint64_t>(node_id) << 32U) | object_id;
}

std::uint64_t steady_time_ns(
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
}

struct UartStreamBuffer {
    std::deque<std::uint8_t> bytes;
    std::uint64_t dropped_bytes{};
    std::uint64_t lost_events{};
    std::uint32_t last_sequence{};
    bool sequence_initialized{};
};

std::uint32_t parse_positive_u32(const std::string& text,
                                 const char* name) {
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> text_body(const std::string& text) {
    return {text.begin(), text.end()};
}

class RuntimeTargetException : public std::runtime_error {
public:
    RuntimeTargetException(remotebsp::toolbusd::IpcErrorCode code,
                           bool possibly_committed,
                           const char* message)
        : std::runtime_error(message), code_(code),
          possibly_committed_(possibly_committed) {}
    remotebsp::toolbusd::IpcErrorCode code() const noexcept { return code_; }
    bool possibly_committed() const noexcept {
        return possibly_committed_;
    }
private:
    remotebsp::toolbusd::IpcErrorCode code_;
    bool possibly_committed_;
};

class OperationReplayException : public std::exception {
public:
    const char* what() const noexcept override {
        return "Runtime 操作由持久账本回放";
    }
};

class OperationLedgerAfterPendingException : public std::runtime_error {
public:
    explicit OperationLedgerAfterPendingException(const std::string& message)
        : std::runtime_error(message) {}
};

bool uses_structured_error(remotebsp::toolbusd::IpcRequestKind kind) {
    using Kind = remotebsp::toolbusd::IpcRequestKind;
    return kind == Kind::RuntimeControlAcquire ||
           kind == Kind::RuntimeGpioWrite ||
           kind == Kind::RuntimeControlRelease ||
           kind == Kind::RuntimeGpioWriteOperation ||
           kind == Kind::RuntimeControlReleaseOperation ||
           kind == Kind::RuntimePwmConfigureOperation ||
           kind == Kind::RuntimePwmStopOperation ||
           kind == Kind::RuntimeTimedBitstreamConfigureOperation ||
           kind == Kind::RuntimeTimedBitstreamFrameOperation ||
           kind == Kind::RuntimeTimedBitstreamStopOperation ||
           kind == Kind::RuntimeBusResourceResetOperation ||
           kind == Kind::RuntimeOperationQuery ||
           kind == Kind::RuntimeOperationLookup ||
           kind == Kind::HealthSnapshot;
}

remotebsp::toolbusd::IpcErrorEnvelope ledger_error_envelope(
    const remotebsp::toolbusd::OperationLedgerException& error) {
    using Category = remotebsp::toolbusd::IpcErrorCategory;
    using Code = remotebsp::toolbusd::IpcErrorCode;
    using LedgerCode = remotebsp::toolbusd::OperationLedgerError;
    switch (error.code()) {
        case LedgerCode::InvalidRequest:
            return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                    Code::InvalidRequest, Category::Request, false, false,
                    "Runtime 操作账本请求字段无效"};
        case LedgerCode::PermissionDenied:
            return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                    Code::PermissionDenied, Category::Authorization,
                    false, false, "无权查询该 Runtime 操作"};
        case LedgerCode::IdempotencyConflict:
        case LedgerCode::InvalidTransition:
            return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                    Code::IdempotencyConflict, Category::Conflict,
                    false, false, "Runtime 操作幂等状态冲突"};
        case LedgerCode::CapacityExceeded:
            return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                    Code::CapacityExceeded, Category::Unavailable,
                    true, false, "Runtime 操作账本容量已满"};
        case LedgerCode::DirectoryUnsafe:
        case LedgerCode::AlreadyLocked:
        case LedgerCode::IoFailure:
        case LedgerCode::Corrupt:
        case LedgerCode::MutationUnavailable:
            return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                    Code::BackendUnavailable, Category::Unavailable,
                    true, false, "Runtime 操作账本不可用"};
    }
    return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
            Code::InternalFailure, Category::Internal, false, false,
            "Runtime 操作账本内部失败"};
}

struct ToolbusDaemonTestOptions {
#ifdef REMOTEBSP_TEST_HOOKS
    std::uint32_t fail_terminal_record_sync_ordinal{};
    std::uint32_t gpio_post_lookup_barrier_participants{};
    std::uint32_t drop_stream_credit_ipc_response_ordinal{};
    std::uint32_t drop_pwm_acquire_response_ordinal{};
    std::uint32_t pwm_remote_lease_ttl_ms{};
    std::uint32_t drop_bus_reset_response_ordinal{};
#endif
};

#ifdef REMOTEBSP_TEST_HOOKS
class OneShotTestBarrier {
public:
    explicit OneShotTestBarrier(std::uint32_t participants)
        : participants_(participants) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        if (arrived_ == participants_) {
            released_ = true;
            condition_.notify_all();
            return;
        }
        if (!condition_.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return released_; })) {
            throw std::runtime_error("Runtime GPIO 测试屏障等待超时");
        }
    }

private:
    const std::uint32_t participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::uint32_t arrived_{};
    bool released_{};
};
#endif

remotebsp::toolbusd::OperationLedgerOptions operation_ledger_options(
    const std::string& directory,
    const ToolbusDaemonTestOptions& test_options) {
    remotebsp::toolbusd::OperationLedgerOptions options;
    options.directory = directory;
#ifdef REMOTEBSP_TEST_HOOKS
    if (test_options.fail_terminal_record_sync_ordinal != 0U) {
        const auto terminal_syncs =
            std::make_shared<std::atomic<std::uint32_t>>(0U);
        const auto fail_ordinal =
            test_options.fail_terminal_record_sync_ordinal;
        options.fault_hook = [terminal_syncs, fail_ordinal](
                                 remotebsp::toolbusd::LedgerIoPoint point) {
            if (point == remotebsp::toolbusd::LedgerIoPoint::
                             TerminalRecordDataSync &&
                terminal_syncs->fetch_add(1U) + 1U == fail_ordinal) {
                throw std::system_error(
                    ENOSPC, std::generic_category(),
                    "测试注入 terminal 账本同步失败");
            }
        };
    }
#else
    static_cast<void>(test_options);
#endif
    return options;
}

remotebsp::toolbusd::RuntimeOperationOutcome operation_outcome(
    const remotebsp::toolbusd::OperationRecord& record, bool replayed) {
    using LedgerKind = remotebsp::toolbusd::OperationKind;
    using LedgerRecovery = remotebsp::toolbusd::OperationRecovery;
    using LedgerState = remotebsp::toolbusd::OperationState;
    using IpcError = remotebsp::toolbusd::RuntimeOperationError;
    using IpcKind = remotebsp::toolbusd::RuntimeOperationKind;
    using IpcRecovery = remotebsp::toolbusd::RuntimeOperationRecovery;
    using IpcState = remotebsp::toolbusd::RuntimeOperationState;

    remotebsp::toolbusd::RuntimeOperationOutcome outcome;
    switch (record.kind) {
        case LedgerKind::RuntimeGpioWrite: outcome.kind = IpcKind::GpioWrite; break;
        case LedgerKind::RuntimeControlRelease: outcome.kind = IpcKind::ControlRelease; break;
        case LedgerKind::RuntimePwmConfigure: outcome.kind = IpcKind::PwmConfigure; break;
        case LedgerKind::RuntimePwmStop: outcome.kind = IpcKind::PwmStop; break;
        case LedgerKind::RuntimeTimedBitstreamConfigure:
            outcome.kind = IpcKind::TimedBitstreamConfigure; break;
        case LedgerKind::RuntimeTimedBitstreamFrame:
            outcome.kind = IpcKind::TimedBitstreamFrame; break;
        case LedgerKind::RuntimeTimedBitstreamStop:
            outcome.kind = IpcKind::TimedBitstreamStop; break;
        case LedgerKind::RuntimeBusResourceReset:
            outcome.kind = IpcKind::BusResourceReset; break;
    }
    switch (record.state) {
        case LedgerState::Pending: outcome.state = IpcState::Pending; break;
        case LedgerState::Committed:
            outcome.state = IpcState::Committed; break;
        case LedgerState::Rejected: outcome.state = IpcState::Rejected; break;
        case LedgerState::Unknown: outcome.state = IpcState::Unknown; break;
    }
    switch (record.recovery) {
        case LedgerRecovery::None: outcome.recovery = IpcRecovery::None; break;
        case LedgerRecovery::NotSent:
            outcome.recovery = IpcRecovery::NotSent; break;
        case LedgerRecovery::SafeClosed:
            outcome.recovery = IpcRecovery::SafeClosed; break;
        case LedgerRecovery::ScopeBlocked:
            outcome.recovery = IpcRecovery::ScopeBlocked; break;
        case LedgerRecovery::AwaitingReboot:
            outcome.recovery = IpcRecovery::AwaitingReboot; break;
        case LedgerRecovery::NodeRebootConfirmed:
            outcome.recovery = IpcRecovery::NodeRebootConfirmed; break;
    }
    outcome.replayed = replayed;
    outcome.operation_id = record.operation_id;
    outcome.lease_id = record.lease_id;
    outcome.expected_node_uuid = record.scope.expected_node_uuid;
    outcome.resource_id = record.scope.resource_id;
    outcome.object_id = record.result.object_id.value_or(0U);
    if (record.state == LedgerState::Committed &&
        record.kind == LedgerKind::RuntimeGpioWrite) {
        outcome.value = record.result.value.value_or(false);
    }
    if (record.state == LedgerState::Committed &&
        record.kind == LedgerKind::RuntimePwmConfigure) {
        outcome.frequency_hz = record.result.frequency_hz.value_or(0U);
        outcome.duty = record.result.duty.value_or(0U);
        outcome.active_low = record.result.active_low.value_or(false);
    }
    switch (record.result.stable_error_code) {
        case 0U: outcome.error = IpcError::None; break;
        case 1U: outcome.error = IpcError::Rejected; break;
        case 2U: outcome.error = IpcError::Deadline; break;
        case 4U: outcome.error = IpcError::Persistence; break;
        default: outcome.error = IpcError::Backend; break;
    }
    return outcome;
}

remotebsp::toolbusd::RuntimeOperationOutcome expired_operation_outcome(
    const remotebsp::toolbusd::OperationDigest& operation_id,
    remotebsp::toolbusd::RuntimeOperationKind kind =
        remotebsp::toolbusd::RuntimeOperationKind::Unknown) {
    remotebsp::toolbusd::RuntimeOperationOutcome outcome;
    outcome.kind = kind;
    outcome.state =
        remotebsp::toolbusd::RuntimeOperationState::ExpiredUnknown;
    outcome.error =
        remotebsp::toolbusd::RuntimeOperationError::HistoryExpired;
    outcome.operation_id = operation_id;
    outcome.replayed = true;
    return outcome;
}

remotebsp::toolbusd::IpcErrorEnvelope gate_error_envelope(
    const remotebsp::toolbusd::RuntimeControlException& error,
    remotebsp::toolbusd::IpcRequestKind kind) {
    using Category = remotebsp::toolbusd::IpcErrorCategory;
    using Code = remotebsp::toolbusd::IpcErrorCode;
    using GateCode = remotebsp::toolbusd::RuntimeControlError;
    Code code = Code::InternalFailure;
    Category category = Category::Internal;
    bool retryable = false;
    const char* message = "Runtime 控制内部失败";
    switch (error.code()) {
        case GateCode::InvalidRequest:
            code = Code::InvalidRequest; category = Category::Request;
            message = "Runtime 控制请求字段无效"; break;
        case GateCode::DaemonIdentityMismatch:
            code = Code::DaemonIdentityMismatch;
            category = Category::Authentication;
            message = "toolbusd 实例身份不匹配"; break;
        case GateCode::PermissionDenied:
            code = Code::PermissionDenied;
            category = Category::Authorization;
            message = "调用者无此 Runtime 控制权限"; break;
        case GateCode::LeaseConflict:
            code = Code::LeaseConflict; category = Category::Conflict;
            retryable = true; message = "目标资源已有互斥控制租约"; break;
        case GateCode::LeaseNotFound:
            code = Code::LeaseNotFound; category = Category::Conflict;
            message = "Runtime 控制租约不存在"; break;
        case GateCode::LeaseExpired:
            code = Code::LeaseExpired; category = Category::Conflict;
            message = "Runtime 控制租约已过期"; break;
        case GateCode::ContractRejected:
            code = Code::ContractRejected;
            category = Category::Authorization;
            message = "目标节点或资源合同校验失败"; break;
        case GateCode::CapacityExceeded:
            code = Code::CapacityExceeded; category = Category::Unavailable;
            retryable = true; message = "Runtime 控制容量已满"; break;
        case GateCode::IdempotencyConflict:
            code = Code::IdempotencyConflict;
            category = Category::Conflict;
            message = "幂等键与原命令参数冲突"; break;
        case GateCode::SafeStopFailed:
            code = Code::SafeStopFailed; category = Category::Internal;
            message = "GPIO 安全停机或对象关闭未确定成功"; break;
        case GateCode::ObjectRetired:
            code = Code::ObjectRetired; category = Category::Conflict;
            message = "GPIO 对象处于隔离或退休状态"; break;
    }
    const bool possibly_committed =
        error.code() == GateCode::SafeStopFailed &&
        (kind == remotebsp::toolbusd::IpcRequestKind::RuntimeGpioWrite ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimeControlRelease ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimePwmConfigureOperation ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimePwmStopOperation ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimeTimedBitstreamConfigureOperation ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimeTimedBitstreamFrameOperation ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimeTimedBitstreamStopOperation ||
         kind == remotebsp::toolbusd::IpcRequestKind::RuntimeBusResourceResetOperation);

    return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion, code, category,
            retryable && !possibly_committed, possibly_committed,
            message};
}

remotebsp::toolbusd::IpcErrorEnvelope generic_error_envelope(
    remotebsp::toolbusd::IpcRequestKind kind,
    const std::exception& error) {
    using Category = remotebsp::toolbusd::IpcErrorCategory;
    using Code = remotebsp::toolbusd::IpcErrorCode;
    if (dynamic_cast<const OperationLedgerAfterPendingException*>(&error) !=
        nullptr) {
        return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                Code::BackendUnavailable, Category::Unavailable,
                false, true,
                "Runtime 操作持久终态不可用，提交状态未知"};
    }
    if (const auto* target = dynamic_cast<const RuntimeTargetException*>(&error)) {
        const auto category = target->code() == Code::DeadlineExceeded
                                  ? Category::Timeout
                                  : Category::Unavailable;
        return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                target->code(), category,
                !target->possibly_committed(), target->possibly_committed(),
                target->what()};
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                Code::InvalidRequest, Category::Request, false, false,
                "本地 IPC 请求字段无效"};
    }
    if (kind == remotebsp::toolbusd::IpcRequestKind::HealthSnapshot) {
        return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                Code::HealthUnavailable, Category::Unavailable, true, false,
                "toolbusd 健康快照暂不可用"};
    }
    return {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
            Code::BackendUnavailable, Category::Unavailable, true, false,
            "Runtime 控制后端暂不可用"};
}

std::uint32_t make_session_id() noexcept {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t mixed =
        ticks ^ (static_cast<std::uint64_t>(::getpid()) << 32U);
    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    const auto value = static_cast<std::uint32_t>(mixed);
    return value == 0 ? 1U : value;
}

std::array<std::uint8_t, 16> make_daemon_instance_id(
    std::uint32_t session_id) {
    std::array<std::uint8_t, 16> result{};
    std::size_t received = 0U;
    while (received < result.size()) {
        const auto count = ::getrandom(result.data() + received,
                                       result.size() - received, 0U);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(
                count < 0 ? errno : EIO, std::generic_category(),
                "生成toolbusd实例身份失败");
        }
        received += static_cast<std::size_t>(count);
    }
    // 额外混入实际 Remote Packet 会话身份，使本地实例标识与本次链路
    // 会话显式相关；唯一性的根仍是内核提供的完整 128 位随机数。
    for (std::size_t index = 0U; index < sizeof(session_id); ++index) {
        result[index] ^= static_cast<std::uint8_t>(
            session_id >> (index * 8U));
    }
    if (std::all_of(result.begin(), result.end(),
                    [](std::uint8_t value) { return value == 0U; })) {
        result[0] = 1U;
    }
    return result;
}

std::uint64_t make_health_generation() {
    std::uint64_t generation = 0U;
    while (generation == 0U) {
        std::size_t received = 0U;
        auto* output = reinterpret_cast<std::uint8_t*>(&generation);
        while (received < sizeof(generation)) {
            const auto count = ::getrandom(output + received,
                                           sizeof(generation) - received, 0U);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::system_error(
                    count < 0 ? errno : EIO, std::generic_category(),
                    "生成toolbusd健康生产者代际失败");
            }
            received += static_cast<std::size_t>(count);
        }
    }
    return generation;
}

std::uint64_t steady_time_ms() noexcept {
    return steady_time_ns() / 1000000U;
}

class ToolbusDaemon {
public:
    ToolbusDaemon(
                  std::unique_ptr<remotebsp::transport::LinkTransport> transport,
                  std::string socket_path,
                  remotebsp::toolbusd::TrafficConfig traffic_config,
                  remotebsp::toolbusd::ClockSyncManagerConfig
                      clock_sync_config,
                   remotebsp::toolbusd::MotionGroupServiceConfig
                       motion_group_config,
                  std::string operation_ledger_directory,
                  std::uint32_t maximum_ipc_clients,
                   const ToolbusDaemonTestOptions& test_options,
                   std::unique_ptr<remotebsp::toolbusd::LinkRecordingController>
                       recording)
        : transport_(std::move(transport)),
          fragmenter_(transport_->mtu()),
          reassembler_(transport_->mtu(), std::chrono::milliseconds(500)),
          traffic_(std::move(traffic_config)),
          clock_sync_(std::move(clock_sync_config)),
          motion_group_(std::move(motion_group_config)),
          operation_ledger_(operation_ledger_options(
              operation_ledger_directory, test_options)),
          recording_(std::move(recording)),
          maximum_ipc_clients_(maximum_ipc_clients),
#ifdef REMOTEBSP_TEST_HOOKS
          gpio_post_lookup_barrier_(
              test_options.gpio_post_lookup_barrier_participants == 0U
                  ? nullptr
                  : std::make_unique<OneShotTestBarrier>(
                        test_options.gpio_post_lookup_barrier_participants)),
          drop_stream_credit_ipc_response_ordinal_(
              test_options.drop_stream_credit_ipc_response_ordinal),
          drop_pwm_acquire_response_ordinal_(
              test_options.drop_pwm_acquire_response_ordinal),
          test_pwm_remote_lease_ttl_ms_(test_options.pwm_remote_lease_ttl_ms),
          drop_bus_reset_response_ordinal_(
              test_options.drop_bus_reset_response_ordinal),
#endif
          socket_path_(std::move(socket_path)) {}

    ~ToolbusDaemon() {
        stop();
        close_server();
    }

    void run() {
        open_server();
        running_ = true;
        worker_ = std::thread(&ToolbusDaemon::can_loop, this);
        std::cout << "toolbusd 已启动，本地套接字: " << socket_path_
                  << '\n';

        while (stop_requested == 0) {
            pollfd descriptor{server_socket_, POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "等待本地 IPC 连接失败");
            }
            if (result == 0 || (descriptor.revents & POLLIN) == 0) {
                continue;
            }
            const int client = ::accept(server_socket_, nullptr, nullptr);
            if (client < 0 && errno == EINTR) {
                continue;
            }
            if (client < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "接受本地 IPC 连接失败");
            }
            const timeval timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                // 在创建线程之前执行准入。超限连接立即关闭；已接纳的慢
                // 客户端仍受 2 秒收发期限约束，不能无限持有配额。
                if (active_clients_ >= maximum_ipc_clients_) {
                    increment_saturating(ipc_capacity_rejected_total_);
                    ::close(client);
                    continue;
                }
                ++active_clients_;
                increment_saturating(ipc_accepted_total_);
                ipc_peak_clients_ = std::max(ipc_peak_clients_, active_clients_);
            }
            try {
                std::thread([this, client] {
                    handle_client(client);
                    ::close(client);
                    {
                        std::lock_guard<std::mutex> lock(client_mutex_);
                        --active_clients_;
                    }
                    clients_finished_.notify_all();
                }).detach();
            } catch (...) {
                // 系统线程资源瞬时耗尽时撤销已记账的准入并关闭 fd，
                // 守护进程继续服务已有客户端，不因一个 accept 结果退出。
                ::close(client);
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    --active_clients_;
                    increment_saturating(ipc_thread_creation_failed_total_);
                }
                clients_finished_.notify_all();
            }
        }
        stop();
        std::cout << "toolbusd 已退出\n";
    }

private:
    bool send_motion_dispatches(
        const std::vector<remotebsp::toolbusd::MotionGroupDispatch>&
            dispatches) {
        bool non_abort_failed = false;
        for (const auto& dispatch : dispatches) {
            bool sent = false;
            try {
                sent = send_packet(
                    dispatch.submission.packet,
                    kNodeRequestBaseRoute + dispatch.node_id,
                    dispatch.phase == remotebsp::toolbusd::
                                          MotionGroupActionPhase::Abort
                        ? remotebsp::toolbusd::AdmissionPolicy::Guaranteed
                        : remotebsp::toolbusd::AdmissionPolicy::Enforce);
            } catch (const std::exception& error) {
                std::cerr << "运动组动作发送失败: " << error.what()
                          << '\n';
            }
            if (!sent &&
                dispatch.phase != remotebsp::toolbusd::
                                      MotionGroupActionPhase::Abort) {
                non_abort_failed = true;
            }
        }
        if (!non_abort_failed) {
            return true;
        }

        std::vector<remotebsp::toolbusd::MotionGroupDispatch> aborts;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto state = motion_group_.state();
            if (state == remotebsp::toolbusd::MotionGroupState::Preparing ||
                state == remotebsp::toolbusd::MotionGroupState::Ready ||
                state == remotebsp::toolbusd::MotionGroupState::Committing) {
                aborts = motion_group_.cancel(requests_).dispatches;
                state_changed_.notify_all();
            }
        }
        // ABORT 使用安全业务保证通道。首次发送异常时请求仍留在
        // RequestManager 中，由主循环有界重试，不能阻塞接收线程。
        for (const auto& abort : aborts) {
            try {
                static_cast<void>(send_packet(
                    abort.submission.packet,
                    kNodeRequestBaseRoute + abort.node_id,
                    remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
            } catch (const std::exception& error) {
                std::cerr << "运动组 ABORT 首次发送失败，将等待重试: "
                          << error.what() << '\n';
            }
        }
        return false;
    }

    static bool motion_group_identity_matches(
        const remotebsp::toolbusd::IpcRequest& request,
        const remotebsp::toolbusd::MotionGroupServiceSnapshot& snapshot) {
        return request.transaction_id == snapshot.transaction_id &&
               request.group_id == snapshot.group_id &&
               request.plan_generation == snapshot.plan_generation;
    }

    bool send_packet(
        const remotebsp::protocol::Packet& packet, std::uint32_t route,
        remotebsp::toolbusd::AdmissionPolicy policy =
            remotebsp::toolbusd::AdmissionPolicy::Enforce,
        const std::function<void()>& on_first_frame = {}) {
        /*
         * 同一把锁同时保护传输 ID 分配和整包发送。这样多个 IPC 客户端
         * 不会竞争 next_transfer_id_，不同远程包的分片也不会相互穿插。
         */
        std::lock_guard<std::mutex> lock(send_mutex_);
        const auto frames = fragmenter_.split(
            remotebsp::protocol::encode(packet), next_transfer_id_++);
        std::vector<std::size_t> frame_lengths;
        frame_lengths.reserve(frames.size());
        for (const auto& frame : frames) {
            frame_lengths.push_back(frame.size());
        }
        const auto traffic_class =
            remotebsp::toolbusd::classify_traffic(packet);
        if (traffic_class ==
            remotebsp::toolbusd::TrafficClass::Safety) {
            policy =
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed;
        }
        if (!traffic_.admit(traffic_class, frame_lengths, policy)) {
            return false;
        }
        bool first_frame = true;
        for (const auto& frame : frames) {
            if (first_frame && on_first_frame) {
                on_first_frame();
            }
            first_frame = false;
            transport_->send({route, frame});
        }
        return true;
    }

    void can_loop() {
        auto next_discovery = std::chrono::steady_clock::now();
        while (running_) {
            try {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_discovery) {
                    send_discovery();
                    next_discovery = now + kDiscoveryInterval;
                }

                const auto message =
                    transport_->receive(std::chrono::milliseconds(50));
                if (message.has_value() &&
                    ((message->route >
                          kNodeResponseBaseRoute &&
                      message->route <=
                          kNodeResponseBaseRoute + kMaximumNodeId) ||
                     (message->route >
                          kNodeEventBaseRoute &&
                      message->route <=
                          kNodeEventBaseRoute + kMaximumNodeId) ||
                     (message->route >
                          kProvisionalResponseBaseRoute &&
                      message->route <=
                          kProvisionalResponseBaseRoute +
                              kMaximumNodeId))) {
                    handle_link_frame(*message);
                }
                process_request_timers();
                process_clock_sync_schedule();
            } catch (const std::exception& error) {
                std::cerr << "CAN 事件循环错误: " << error.what() << '\n';
            }
        }
    }

    void send_discovery() {
        remotebsp::protocol::Packet request;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            request = nodes_.make_discovery_request(next_control_request_id_++);
        }
        static_cast<void>(
            send_packet(request, kBroadcastRequestRoute));
    }

    void handle_link_frame(
        const remotebsp::transport::LinkFrame& message) {
        const auto result =
            reassembler_.accept(message.route, message.data);
        if (result.status !=
                remotebsp::protocol::ReassemblyStatus::Complete ||
            !result.packet.has_value()) {
            return;
        }
        /* 完整响应形成后立即取时，避免解码和锁等待混入往返时延。 */
        const auto response_complete_at =
            remotebsp::toolbusd::ClockSyncManager::Clock::now();
        const auto packet = remotebsp::protocol::decode(*result.packet);
        const auto command =
            static_cast<remotebsp::protocol::Command>(packet.header.command);
        // 错误 command 也可能携带运动组 request_id，并会触发全组
        // fail_active。因此除发现/时钟同步外的所有普通响应均经过运动门，
        // 不能只按响应 command 判断。
        const bool needs_motion_dispatch_gate =
            packet.header.message_type ==
                remotebsp::protocol::MessageType::Response &&
            command != remotebsp::protocol::Command::DiscoveryResponse &&
            command != remotebsp::protocol::Command::TimeSync;
        std::optional<remotebsp::toolbusd::MotionGroupDispatchGate::Guard>
            motion_guard;
        if (needs_motion_dispatch_gate) {
            motion_guard.emplace(motion_dispatch_gate_.lock());
        }

        std::optional<remotebsp::protocol::Packet> assignment;
        std::vector<remotebsp::toolbusd::MotionGroupDispatch>
            motion_dispatches;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (packet.header.command == static_cast<std::uint16_t>(
                    remotebsp::protocol::Command::DiscoveryResponse)) {
                const auto update =
                    nodes_.accept_discovery_response(packet);
                const auto identity =
                    remotebsp::protocol::decode_node_identity(packet.payload);
                /*
                 * 已知节点若重新从临时响应 ID 发出发现响应，说明 MCU
                 * 已复位并丢失了 RAM 中的节点 ID。保留原 ID，但撤销
                 * 心跳确认，让下方逻辑重新发送 NODE_ASSIGN。
                 */
                if (message.route > kProvisionalResponseBaseRoute &&
                    message.route <=
                        kProvisionalResponseBaseRoute + kMaximumNodeId) {
                    const auto* previous = nodes_.find(identity.uuid);
                    if (previous != nullptr && previous->node_id != 0U) {
                        static_cast<void>(clock_sync_.cancel_node(
                            requests_, previous->node_id));
                        invalidate_bus_node_locked(previous->node_id);
                    }
                    nodes_.mark_assignment_unconfirmed(identity.uuid);
                }
                const auto* node = nodes_.find(identity.uuid);
                if (update == remotebsp::toolbusd::NodeUpdate::Added) {
                    if (next_node_id_ > kMaximumNodeId) {
                        throw std::runtime_error(
                            "可分配节点 ID 已耗尽");
                    }
                    assignment = nodes_.make_node_assignment(
                        identity.uuid, next_node_id_++,
                        next_control_request_id_++);
                } else if (node != nullptr && !node->assigned &&
                           node->node_id != 0) {
                    /*
                     * NODE_ASSIGN 或其响应可能丢失。节点在发出带 ID 的
                     * 心跳前都视为未确认；每次发现响应重发同一个 ID，
                     * 避免消耗新 ID，也保证重试幂等。
                     */
                    assignment = nodes_.make_node_assignment(
                        identity.uuid, node->node_id,
                        next_control_request_id_++);
                }
            } else if (packet.header.command ==
                       static_cast<std::uint16_t>(
                           remotebsp::protocol::Command::Heartbeat)) {
                nodes_.accept_heartbeat(packet);
            } else if (packet.header.message_type ==
                       remotebsp::protocol::MessageType::Event) {
                if (message.route <= kNodeEventBaseRoute ||
                    message.route >
                        kNodeEventBaseRoute + kMaximumNodeId) {
                    return;
                }
                const auto node_id =
                    message.route - kNodeEventBaseRoute;
                if (packet.header.command ==
                        static_cast<std::uint16_t>(
                            remotebsp::protocol::Command::UartRxEvent) &&
                    packet.header.object_id != 0U) {
                    auto& stream = uart_stream_buffers_[uart_stream_key(
                        node_id, packet.header.object_id)];
                    bool accept = true;
                    const auto sequence = packet.header.request_id;
                    if (sequence != 0U) {
                        if (!stream.sequence_initialized) {
                            stream.sequence_initialized = true;
                            stream.last_sequence = sequence;
                        } else {
                            const std::uint32_t distance =
                                sequence - stream.last_sequence;
                            if (distance == 0U ||
                                distance >= 0x80000000U) {
                                accept = false;
                            } else {
                                if (distance > 1U) {
                                    stream.lost_events += distance - 1U;
                                }
                                stream.last_sequence = sequence;
                            }
                        }
                    }
                    if (accept) {
                        for (const auto byte : packet.payload) {
                            if (stream.bytes.size() ==
                                kUartStreamBufferCapacity) {
                                stream.bytes.pop_front();
                                ++stream.dropped_bytes;
                            }
                            stream.bytes.push_back(byte);
                        }
                    }
                }
                auto& queue = event_queues_[node_id];
                if (queue.size() == kMaximumQueuedEventsPerNode) {
                    queue.pop_front();
                }
                queue.push_back(packet);
                state_changed_.notify_all();
            } else {
                if (message.route <= kNodeResponseBaseRoute ||
                    message.route >
                        kNodeResponseBaseRoute + kMaximumNodeId) {
                    return;
                }
                const auto key = request_key(
                    packet.header.session_id, packet.header.request_id);
                const auto response_node_id =
                    message.route - kNodeResponseBaseRoute;
                if (packet.header.command == static_cast<std::uint16_t>(
                        remotebsp::protocol::Command::TimeSync)) {
                    static_cast<void>(clock_sync_.accept_response(
                        requests_, nodes_, packet, response_node_id,
                        response_complete_at));
                    return;
                }
                const auto group = motion_group_.accept_response(
                    requests_, nodes_, response_node_id, packet,
                    steady_time_ns(response_complete_at),
                    response_complete_at);
                if (group.status != remotebsp::toolbusd::
                                        MotionGroupServiceStatus::Unrelated) {
                    motion_dispatches = group.dispatches;
                    state_changed_.notify_all();
                } else {
                    const auto target = request_routes_.find(key);
                    if (target == request_routes_.end() ||
                        target->second !=
                            kNodeRequestBaseRoute + response_node_id) {
                        return;
                    }
                    const auto response = requests_.accept_response(packet);
                    if (response.status ==
                            remotebsp::toolbusd::ResponseStatus::Matched &&
                        response.response.has_value()) {
                        // 经过 session/request/路由匹配的响应同样证明节点仍
                        // 在线。长快照会连续查询大量资源，不能只靠心跳更新
                        // 活性，否则合法响应流中也可能跨过离线窗口。
                        static_cast<void>(nodes_.observe_response(
                            response_node_id, response_complete_at));
                        responses_[key] = *response.response;
                        state_changed_.notify_all();
                    }
                }
            }
        }
        if (assignment.has_value()) {
            static_cast<void>(send_packet(
                *assignment, kBroadcastRequestRoute,
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
        }
        if (!motion_dispatches.empty()) {
            static_cast<void>(send_motion_dispatches(motion_dispatches));
        }
    }

    void process_request_timers() {
        auto motion_guard = motion_dispatch_gate_.lock();
        struct Retry {
            remotebsp::protocol::Packet packet;
            std::uint32_t route{};
        };
        std::vector<Retry> retries;
        std::vector<remotebsp::toolbusd::MotionGroupDispatch>
            motion_dispatches;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto request_events = requests_.poll(now);
            const auto offline = nodes_.expire(now);
            for (const auto& uuid : offline) {
                const auto* node = nodes_.find(uuid);
                if (node != nullptr && node->node_id != 0U) {
                    static_cast<void>(clock_sync_.cancel_node(
                        requests_, node->node_id));
                    invalidate_bus_node_locked(node->node_id);
                }
            }
            const auto group = motion_group_.poll(
                requests_, nodes_, request_events, steady_time_ns(now), now);
            motion_dispatches = group.dispatches;
            for (const auto& event : group.unhandled_events) {
                if (event.type ==
                        remotebsp::toolbusd::RequestEventType::Retry &&
                    event.packet.has_value()) {
                    const auto key =
                        request_key(event.session_id, event.request_id);
                    const auto target = request_routes_.find(key);
                    if (event.packet->header.command ==
                        static_cast<std::uint16_t>(
                            remotebsp::protocol::Command::TimeSync)) {
                        retries.push_back(
                            {*event.packet,
                             kNodeRequestBaseRoute +
                                 event.packet->header.object_id});
                    } else if (target != request_routes_.end()) {
                        retries.push_back({*event.packet, target->second});
                    }
                } else if (event.type ==
                           remotebsp::toolbusd::RequestEventType::TimedOut) {
                    const bool clock_sync_timeout =
                        clock_sync_.handle_request_event(requests_, event);
                    if (!clock_sync_timeout) {
                        timed_out_.insert(request_key(
                            event.session_id, event.request_id));
                        request_routes_.erase(request_key(
                            event.session_id, event.request_id));
                        state_changed_.notify_all();
                    }
                }
            }
            if (!offline.empty() ||
                group.status != remotebsp::toolbusd::
                                    MotionGroupServiceStatus::Accepted) {
                state_changed_.notify_all();
            }
        }
        if (!motion_dispatches.empty()) {
            static_cast<void>(send_motion_dispatches(motion_dispatches));
        }
        motion_guard.unlock();
        for (const auto& retry : retries) {
            const bool is_clock_sync =
                retry.packet.header.command ==
                static_cast<std::uint16_t>(
                    remotebsp::protocol::Command::TimeSync);
            static_cast<void>(send_packet(
                retry.packet, retry.route,
                is_clock_sync
                    ? remotebsp::toolbusd::AdmissionPolicy::Enforce
                    : remotebsp::toolbusd::AdmissionPolicy::Guaranteed,
                is_clock_sync
                    ? std::function<void()>([this, &retry] {
                          std::lock_guard<std::mutex> lock(state_mutex_);
                          static_cast<void>(clock_sync_.mark_sent(
                              retry.packet.header.session_id,
                              retry.packet.header.request_id));
                      })
                    : std::function<void()>{}));
        }
    }

    void process_clock_sync_schedule() {
        std::vector<remotebsp::toolbusd::ClockSyncDispatch> dispatches;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            dispatches = clock_sync_.poll_schedule(
                requests_, nodes_, session_id_);
        }
        for (const auto& dispatch : dispatches) {
            const auto& packet = dispatch.submission.packet;
            const bool sent = send_packet(
                packet, kNodeRequestBaseRoute + dispatch.node_id,
                remotebsp::toolbusd::AdmissionPolicy::Enforce,
                [this, &packet] {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    static_cast<void>(clock_sync_.mark_sent(
                        packet.header.session_id,
                        packet.header.request_id));
                });
            if (!sent) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                static_cast<void>(clock_sync_.cancel(
                    requests_, packet.header.session_id,
                    packet.header.request_id));
            }
        }
    }

    std::vector<std::uint8_t> request_snapshot_resource(
        std::uint32_t node_id, remotebsp::protocol::Command command,
        std::vector<std::uint8_t> payload,
        std::chrono::steady_clock::time_point deadline) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Runtime 快照达到总时间上限");
        }
        remotebsp::protocol::Packet request;
        request.header.message_type =
            remotebsp::protocol::MessageType::Request;
        request.header.command = static_cast<std::uint16_t>(command);
        request.header.object_id = 0U;
        request.header.session_id = session_id_;
        request.payload = std::move(payload);

        remotebsp::toolbusd::Submission submission;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(node_id);
            if (node == nullptr || !node->online || !node->assigned) {
                throw std::runtime_error(
                    "Runtime 快照期间目标节点不可用");
            }
            submission = requests_.submit(std::move(request));
            request_routes_[request_key(session_id_, submission.request_id)] =
                kNodeRequestBaseRoute + node_id;
        }
        const auto key = request_key(session_id_, submission.request_id);
        if (!send_packet(submission.packet,
                         kNodeRequestBaseRoute + node_id)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            requests_.cancel(session_id_, submission.request_id);
            request_routes_.erase(key);
            throw std::runtime_error(
                "Runtime 快照查询被带宽准入拒绝");
        }

        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool completed = state_changed_.wait_until(lock, deadline, [&] {
            return !running_ || responses_.find(key) != responses_.end() ||
                   timed_out_.find(key) != timed_out_.end();
        });
        const auto response = responses_.find(key);
        if (completed && response != responses_.end()) {
            auto packet = std::move(response->second);
            responses_.erase(response);
            request_routes_.erase(key);
            lock.unlock();
            if (packet.payload.empty() || packet.payload.front() != 0U ||
                (packet.header.flags &
                 remotebsp::protocol::kErrorResponseFlag) != 0U) {
                throw std::runtime_error(
                    "Runtime 快照远端只读查询失败");
            }
            return {packet.payload.begin() + 1U, packet.payload.end()};
        }
        requests_.cancel(session_id_, submission.request_id);
        request_routes_.erase(key);
        timed_out_.erase(key);
        throw std::runtime_error(
            completed ? "Runtime 快照远端请求超时"
                      : "Runtime 快照达到总时间上限");
    }

    remotebsp::protocol::Packet request_runtime_control_packet(
        std::uint32_t node_id, std::uint64_t node_generation,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        remotebsp::protocol::Command command,
        std::vector<std::uint8_t> payload, std::uint32_t object_id,
        std::chrono::steady_clock::time_point deadline) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw RuntimeTargetException(
                remotebsp::toolbusd::IpcErrorCode::DeadlineExceeded,
                false, "Runtime GPIO 写入达到总时间上限");
        }
        remotebsp::protocol::Packet request;
        request.header.message_type =
            remotebsp::protocol::MessageType::Request;
        request.header.command = static_cast<std::uint16_t>(command);
        request.header.object_id = object_id;
        request.header.session_id = session_id_;
        request.payload = std::move(payload);

        remotebsp::toolbusd::Submission submission;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                node_id > kMaximumNodeId ||
                node->identity.uuid != expected_node_uuid ||
                bus_node_generations_[node_id] != node_generation) {
                throw RuntimeTargetException(
                    remotebsp::toolbusd::IpcErrorCode::NodeUnavailable,
                    false, "Runtime GPIO 写入前目标节点已离线或重启");
            }
            submission = requests_.submit(std::move(request));
            request_routes_[request_key(session_id_, submission.request_id)] =
                kNodeRequestBaseRoute + node_id;
        }
        const auto key = request_key(session_id_, submission.request_id);
        if (!send_packet(submission.packet,
                         kNodeRequestBaseRoute + node_id)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            requests_.cancel(session_id_, submission.request_id);
            request_routes_.erase(key);
            throw RuntimeTargetException(
                remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,
                false, "Runtime GPIO 写入被带宽准入拒绝");
        }

        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool completed = state_changed_.wait_until(lock, deadline, [&] {
            return !running_ || responses_.find(key) != responses_.end() ||
                   timed_out_.find(key) != timed_out_.end();
        });
        const auto response = responses_.find(key);
        if (completed && response != responses_.end()) {
            auto packet = std::move(response->second);
            responses_.erase(response);
            request_routes_.erase(key);
            lock.unlock();
            if (packet.header.message_type !=
                    remotebsp::protocol::MessageType::Response ||
                packet.header.command !=
                    static_cast<std::uint16_t>(command) ||
                packet.payload.empty()) {
                throw RuntimeTargetException(
                    remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,
                    true,
                    "Runtime GPIO 远端响应无效，提交状态未知");
            }
            if (packet.payload.front() != 0U ||
                (packet.header.flags &
                 remotebsp::protocol::kErrorResponseFlag) != 0U) {
                throw RuntimeTargetException(
                    remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,
                    false, "Runtime GPIO 远端写命令被确定拒绝");
            }
            return packet;
        }
        requests_.cancel(session_id_, submission.request_id);
        request_routes_.erase(key);
        timed_out_.erase(key);
        throw RuntimeTargetException(
            remotebsp::toolbusd::IpcErrorCode::DeadlineExceeded,
            true, completed ? "Runtime GPIO 远端请求超时，提交状态未知"
                            : "Runtime GPIO 写入达到总时间上限，提交状态未知");
    }

    void runtime_control_acquire(
        const remotebsp::toolbusd::RuntimeControlAcquireRequest& request) {
        if (request.daemon_instance_id != daemon_instance_id_) {
            throw remotebsp::toolbusd::RuntimeControlException(
                remotebsp::toolbusd::RuntimeControlError::
                    DaemonIdentityMismatch,
                "Runtime 控制租约绑定了其他 toolbusd 实例");
        }
        const auto blocked = operation_ledger_.blocked_scopes();
        const auto scope_blocked = std::any_of(
            blocked.begin(), blocked.end(), [&](const auto& scope) {
                return scope.expected_node_uuid ==
                           request.expected_node_uuid &&
                       scope.resource_id == request.resource_id;
            });
        if (scope_blocked) {
            throw remotebsp::toolbusd::RuntimeControlException(
                remotebsp::toolbusd::RuntimeControlError::LeaseConflict,
                "Runtime 操作账本仍阻塞该节点资源，拒绝登记新租约");
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1800);
        std::uint64_t node_generation = 0U;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                request.node_id > kMaximumNodeId ||
                node->identity.uuid != request.expected_node_uuid) {
                throw std::runtime_error(
                    "Runtime 控制租约目标节点尚未发现或已经离线");
            }
            node_generation = bus_node_generations_[request.node_id];
        }
        const auto runtime_scope =
            (static_cast<std::uint64_t>(request.node_id) << 32U) |
            request.resource_id;
        {
            std::lock_guard<std::mutex> policy_lock(
                runtime_operation_policy_mutex_);
            const auto quarantined = pwm_remote_lease_quarantine_.find(
                runtime_scope);
            if (quarantined != pwm_remote_lease_quarantine_.end()) {
                if (quarantined->second.first == node_generation &&
                    std::chrono::steady_clock::now() <
                        quarantined->second.second) {
                    throw remotebsp::toolbusd::RuntimeControlException(
                        remotebsp::toolbusd::RuntimeControlError::LeaseConflict,
                        "PWM 远端租约结果未知，TTL 安全届满前冻结作用域");
                }
                pwm_remote_lease_quarantine_.erase(quarantined);
            }
        }
        const auto descriptor = remotebsp::protocol::decode_resource_descriptor(
            request_snapshot_resource(
                request.node_id,
                remotebsp::protocol::Command::ResourceDescribe,
                remotebsp::protocol::encode_resource_id(request.resource_id),
                deadline));
        const auto contract = remotebsp::protocol::decode_resource_contract(
            request_snapshot_resource(
                request.node_id,
                remotebsp::protocol::Command::ResourceContract,
                remotebsp::protocol::encode_resource_id(request.resource_id),
                deadline));
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr ||
                node->identity.uuid != request.expected_node_uuid ||
                bus_node_generations_[request.node_id] != node_generation) {
                throw std::runtime_error(
                    "Runtime 控制租约登记期间节点代次变化");
            }
        }
        // 合同查询在锁外执行。查询期间其他操作的 terminal fsync 可能把
        // 账本切到不可用，因此在 Gate 最终登记前与账本写边界线性化地
        // 二次检查；策略锁不覆盖任何远端 I/O。
        {
            std::lock_guard<std::mutex> policy_lock(
                runtime_operation_policy_mutex_);
            const auto final_blocked = operation_ledger_.blocked_scopes();
            if (std::any_of(
                final_blocked.begin(), final_blocked.end(),
                [&](const auto& scope) {
                    return scope.expected_node_uuid ==
                               request.expected_node_uuid &&
                           scope.resource_id == request.resource_id;
                    })) {
                throw remotebsp::toolbusd::RuntimeControlException(
                    remotebsp::toolbusd::RuntimeControlError::LeaseConflict,
                    "Runtime 操作账本在合同查询期间阻塞了该节点资源");
            }
            runtime_control_.acquire(request, daemon_instance_id_,
                                     node_generation, descriptor, contract);
        }
        const bool bus_reset_requires_remote_lease =
            request.permissions ==
            remotebsp::toolbusd::kRuntimePermissionBusReset;
        if (bus_reset_requires_remote_lease ||
            ((request.permissions ==
                  remotebsp::toolbusd::kRuntimePermissionPwmWrite ||
              request.permissions ==
                  remotebsp::toolbusd::kRuntimePermissionTimedBitstreamWrite) &&
             (contract.access_flags &
              remotebsp::protocol::kResourceAccessLeaseRequired) != 0U)) {
            const auto remote_ttl = std::min<std::uint32_t>(
                60000U, request.ttl_ms > 55000U ? 60000U
                                                : request.ttl_ms + 5000U);
#ifdef REMOTEBSP_TEST_HOOKS
            const auto effective_remote_ttl = test_pwm_remote_lease_ttl_ms_ == 0U
                ? remote_ttl : test_pwm_remote_lease_ttl_ms_;
#else
            const auto effective_remote_ttl = remote_ttl;
#endif
            bool remote_acquire_response_ok = false;
            try {
                const auto response = request_runtime_control_packet(
                    request.node_id, node_generation,
                    request.expected_node_uuid,
                    remotebsp::protocol::Command::ResourceAcquire,
                    remotebsp::protocol::encode_resource_lease_request(
                        {request.resource_id, effective_remote_ttl,
                         remotebsp::protocol::ResourceLeaseMode::Exclusive}),
                    0U, deadline);
#ifdef REMOTEBSP_TEST_HOOKS
                const auto acquire_ordinal =
                    ++pwm_acquire_response_count_;
                if (drop_pwm_acquire_response_ordinal_ != 0U &&
                    acquire_ordinal == drop_pwm_acquire_response_ordinal_) {
                    throw RuntimeTargetException(
                        remotebsp::toolbusd::IpcErrorCode::DeadlineExceeded,
                        true, "测试故障：ResourceAcquire 已提交后丢失响应");
                }
#endif
                remote_acquire_response_ok = true;
                const auto info = remotebsp::protocol::decode_resource_lease_info(
                    {response.payload.begin() + 1U, response.payload.end()});
                if (info.resource_id != request.resource_id ||
                    info.owner_session_id != session_id_ ||
                    info.mode != remotebsp::protocol::ResourceLeaseMode::Exclusive ||
                    info.lease_id == 0U) {
                    throw std::runtime_error("PWM 远端独占租约响应不匹配");
                }
                runtime_control_.bind_pwm_remote_lease(
                    request.lease_id, info.lease_id,
                    [this](std::uint32_t node_id, std::uint64_t generation,
                           const std::array<std::uint8_t, 16>& uuid,
                           std::uint32_t resource_id, std::uint64_t lease_id) {
                        static_cast<void>(request_runtime_control_packet(
                            node_id, generation, uuid,
                            remotebsp::protocol::Command::ResourceRelease,
                            remotebsp::protocol::encode_resource_lease_token_request(
                                {resource_id, lease_id, 0U}), 0U,
                            std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(1800)));
                    });
            } catch (const RuntimeTargetException& error) {
                if (error.possibly_committed()) {
                    std::lock_guard<std::mutex> lock(
                        runtime_operation_policy_mutex_);
                    pwm_remote_lease_quarantine_[runtime_scope] = {
                        node_generation, std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(effective_remote_ttl)};
                }
                try {
                    remotebsp::toolbusd::RuntimeControlReleaseRequest rollback;
                    rollback.daemon_instance_id = request.daemon_instance_id;
                    rollback.lease_id = request.lease_id;
                    rollback.owner_key_id = request.owner_key_id;
                    runtime_control_.release(rollback, daemon_instance_id_);
                } catch (...) {
                }
                throw;
            } catch (...) {
                if (remote_acquire_response_ok) {
                    std::lock_guard<std::mutex> lock(
                        runtime_operation_policy_mutex_);
                    pwm_remote_lease_quarantine_[runtime_scope] = {
                        node_generation, std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(effective_remote_ttl)};
                }
                try {
                    remotebsp::toolbusd::RuntimeControlReleaseRequest rollback;
                    rollback.daemon_instance_id = request.daemon_instance_id;
                    rollback.lease_id = request.lease_id;
                    rollback.owner_key_id = request.owner_key_id;
                    runtime_control_.release(rollback, daemon_instance_id_);
                } catch (...) {
                }
                throw;
            }
        }
    }

    remotebsp::toolbusd::RuntimeGpioWriteResult runtime_gpio_write(
        const remotebsp::toolbusd::RuntimeGpioWriteRequest& request,
        const remotebsp::toolbusd::RuntimeControlGate::GpioDurability&
            durability) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1800);
        std::uint64_t node_generation = 0U;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                request.node_id > kMaximumNodeId ||
                node->identity.uuid != request.expected_node_uuid) {
                throw std::runtime_error(
                    "Runtime GPIO 目标节点尚未发现或已经离线");
            }
            node_generation = bus_node_generations_[request.node_id];
        }
        const auto descriptor_body = request_snapshot_resource(
            request.node_id,
            remotebsp::protocol::Command::ResourceDescribe,
            remotebsp::protocol::encode_resource_id(request.resource_id),
            deadline);
        const auto descriptor =
            remotebsp::protocol::decode_resource_descriptor(descriptor_body);
        const auto contract_body = request_snapshot_resource(
            request.node_id,
            remotebsp::protocol::Command::ResourceContract,
            remotebsp::protocol::encode_resource_id(request.resource_id),
            deadline);
        const auto contract =
            remotebsp::protocol::decode_resource_contract(contract_body);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr ||
                node->identity.uuid != request.expected_node_uuid ||
                bus_node_generations_[request.node_id] != node_generation) {
                throw std::runtime_error(
                    "Runtime GPIO 合同查询期间节点代次变化");
            }
        }
        return runtime_control_.gpio_write(
            request, daemon_instance_id_, node_generation,
            descriptor, contract,
            remotebsp::toolbusd::RuntimeControlGate::GpioIo{
                [&] {
                    const auto response = request_runtime_control_packet(
                        request.node_id, node_generation,
                        request.expected_node_uuid,
                        remotebsp::protocol::Command::GpioCreate,
                        {static_cast<std::uint8_t>(descriptor.instance),
                         static_cast<std::uint8_t>(descriptor.instance >> 8U),
                         1U, 0U},
                        0U, deadline);
                    if (response.header.object_id == 0U) {
                        throw RuntimeTargetException(
                            remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,
                            true,
                            "Runtime GPIO_CREATE 已响应但对象 ID 无效，提交状态未知");
                    }
                    return response.header.object_id;
                },
                [&](std::uint32_t object_id, bool value) {
                    static_cast<void>(request_runtime_control_packet(
                        request.node_id, node_generation,
                        request.expected_node_uuid,
                        remotebsp::protocol::Command::GpioWrite,
                        {static_cast<std::uint8_t>(value)}, object_id,
                        deadline));
                },
                [this](
                    std::uint32_t node_id, std::uint64_t node_generation,
                    const std::array<std::uint8_t, 16>& expected_node_uuid,
                    std::uint32_t object_id) {
                    const auto stop_deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1800);
                    static_cast<void>(request_runtime_control_packet(
                        node_id, node_generation, expected_node_uuid,
                        remotebsp::protocol::Command::GpioWrite, {0U},
                        object_id, stop_deadline));
                },
                [this](
                    std::uint32_t node_id, std::uint64_t node_generation,
                    const std::array<std::uint8_t, 16>& expected_node_uuid,
                    std::uint32_t object_id) {
                    const auto close_deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1800);
                    static_cast<void>(request_runtime_control_packet(
                        node_id, node_generation, expected_node_uuid,
                        remotebsp::protocol::Command::GpioClose,
                        remotebsp::protocol::encode_gpio_close(), object_id,
                        close_deadline));
                }
            }, durability);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome
    runtime_gpio_write_operation(
        const remotebsp::toolbusd::RuntimeGpioWriteRequest& request) {
        using namespace remotebsp::toolbusd;
        if (request.daemon_instance_id != daemon_instance_id_) {
            throw RuntimeControlException(
                RuntimeControlError::DaemonIdentityMismatch,
                "Runtime GPIO 操作绑定了其他 toolbusd 实例");
        }
        RuntimeGpioWriteOperation operation;
        operation.daemon_origin = request.daemon_instance_id;
        operation.lease_id = request.lease_id;
        operation.expected_node_uuid = request.expected_node_uuid;
        operation.owner_key_id = request.owner_key_id;
        operation.idempotency_key = request.idempotency_key;
        operation.permissions = request.permissions;
        operation.node_id = request.node_id;
        operation.resource_id = request.resource_id;
        operation.value = request.value;

        const auto operation_id = OperationLedger::derive_operation_id(
            operation);
        const auto request_digest = OperationLedger::derive_request_digest(
            operation);
        const auto existing = operation_ledger_.lookup(
            operation_id, request.owner_key_id);
        if (existing.disposition == OperationLookupDisposition::Found) {
            if (existing.record->request_digest != request_digest) {
                throw OperationLedgerException(
                    OperationLedgerError::IdempotencyConflict,
                    "相同 Runtime GPIO operation_id 绑定了不同请求");
            }
            return operation_outcome(*existing.record, true);
        }
#ifdef REMOTEBSP_TEST_HOOKS
        if (gpio_post_lookup_barrier_) {
            gpio_post_lookup_barrier_->arrive_and_wait();
        }
#endif

        std::optional<OperationBeginResult> begun;
        std::optional<OperationRecord> durable_terminal;
        const auto finish_durable = [&](OperationState state,
                                        OperationRecovery recovery,
                                        const OperationTerminalResult& result) {
            if (!begun.has_value() ||
                begun->disposition !=
                    OperationBeginDisposition::StartedDurablePending) {
                throw std::logic_error(
                    "Runtime GPIO 缺少持久 pending");
            }
            try {
                std::lock_guard<std::mutex> policy_lock(
                    runtime_operation_policy_mutex_);
                return operation_ledger_.finish(
                    begun->record.operation_id, begun->record.request_digest,
                    state, recovery, result);
            } catch (const OperationLedgerException& error) {
                throw OperationLedgerAfterPendingException(error.what());
            }
        };
        const auto finish_failed = [&](RuntimeControlGate::DurableRecovery recovery,
                                       RuntimeControlError) {
            if (!begun.has_value() ||
                begun->disposition !=
                    OperationBeginDisposition::StartedDurablePending) {
                return;
            }
            durable_terminal = finish_durable(
                recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                    ? OperationState::Rejected
                    : OperationState::Unknown,
                recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                    ? OperationRecovery::SafeClosed
                    : OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt,
                 static_cast<std::uint16_t>(RuntimeOperationError::Backend)});
        };
        try {
            const auto result = runtime_gpio_write(
                request,
                RuntimeControlGate::GpioDurability{
                    [&] {
                        std::lock_guard<std::mutex> policy_lock(
                            runtime_operation_policy_mutex_);
                        begun = operation_ledger_.begin_gpio_write(operation);
                        if (begun->disposition !=
                            OperationBeginDisposition::StartedDurablePending) {
                            throw OperationReplayException{};
                        }
                    },
                    [&](const RuntimeGpioWriteResult& committed) {
                        durable_terminal = finish_durable(
                            OperationState::Committed,
                            OperationRecovery::None,
                            {committed.object_id, committed.value, 0U});
                    },
                    finish_failed});
            if (!durable_terminal.has_value()) {
                const auto replay = operation_ledger_.lookup(
                    operation_id, request.owner_key_id);
                if (replay.disposition ==
                    OperationLookupDisposition::Found) {
                    if (replay.record->request_digest != request_digest) {
                        throw OperationLedgerException(
                            OperationLedgerError::IdempotencyConflict,
                            "Runtime GPIO Gate 回放与持久请求摘要不一致");
                    }
                    return operation_outcome(*replay.record, true);
                }
                throw std::logic_error(
                    "Runtime GPIO Gate 回放缺少持久账本记录");
            }
            static_cast<void>(result);
            return operation_outcome(*durable_terminal, false);
        } catch (const OperationReplayException&) {
            if (!begun.has_value()) {
                throw;
            }
            return operation_outcome(begun->record, true);
        } catch (const OperationLedgerAfterPendingException&) {
            throw;
        } catch (const RuntimeControlException& error) {
            if (durable_terminal.has_value()) {
                return operation_outcome(*durable_terminal, false);
            }
            if (!begun.has_value()) {
                throw;
            }
            const auto stable_error =
                error.code() == RuntimeControlError::SafeStopFailed
                    ? RuntimeOperationError::Backend
                    : RuntimeOperationError::Rejected;
            durable_terminal = finish_durable(
                OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt,
                 static_cast<std::uint16_t>(stable_error)});
            return operation_outcome(*durable_terminal, false);
        } catch (const RuntimeTargetException& error) {
            if (durable_terminal.has_value()) {
                return operation_outcome(*durable_terminal, false);
            }
            if (!begun.has_value()) {
                throw;
            }
            const auto stable_error =
                error.code() == IpcErrorCode::DeadlineExceeded
                    ? RuntimeOperationError::Deadline
                    : RuntimeOperationError::Backend;
            durable_terminal = finish_durable(
                OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt,
                 static_cast<std::uint16_t>(stable_error)});
            return operation_outcome(*durable_terminal, false);
        } catch (const OperationLedgerException&) {
            throw;
        } catch (const std::exception&) {
            if (durable_terminal.has_value()) {
                return operation_outcome(*durable_terminal, false);
            }
            if (!begun.has_value()) {
                throw;
            }
            durable_terminal = finish_durable(
                OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt,
                 static_cast<std::uint16_t>(
                     RuntimeOperationError::Backend)});
            return operation_outcome(*durable_terminal, false);
        }
    }

    template <typename Request>
    std::tuple<std::uint64_t, remotebsp::protocol::ResourceDescriptor,
               remotebsp::protocol::ResourceContract>
    resolve_runtime_pwm(const Request& request,
                        std::chrono::steady_clock::time_point deadline) {
        std::uint64_t generation{};
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                request.node_id > kMaximumNodeId ||
                node->identity.uuid != request.expected_node_uuid) {
                throw std::runtime_error("Runtime PWM 目标节点尚未发现或已经离线");
            }
            generation = bus_node_generations_[request.node_id];
        }
        const auto descriptor = remotebsp::protocol::decode_resource_descriptor(
            request_snapshot_resource(request.node_id,
                remotebsp::protocol::Command::ResourceDescribe,
                remotebsp::protocol::encode_resource_id(request.resource_id), deadline));
        const auto contract = remotebsp::protocol::decode_resource_contract(
            request_snapshot_resource(request.node_id,
                remotebsp::protocol::Command::ResourceContract,
                remotebsp::protocol::encode_resource_id(request.resource_id), deadline));
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr || node->identity.uuid != request.expected_node_uuid ||
                bus_node_generations_[request.node_id] != generation) {
                throw std::runtime_error("Runtime PWM 合同查询期间节点代次变化");
            }
        }
        return {generation, descriptor, contract};
    }

    remotebsp::toolbusd::RuntimeControlGate::PwmIo pwm_io(
        const remotebsp::toolbusd::RuntimePwmConfigureRequest& request,
        std::uint64_t generation,
        const remotebsp::protocol::ResourceDescriptor& descriptor,
        std::chrono::steady_clock::time_point deadline) {
        const auto node_id = request.node_id;
        const auto expected_uuid = request.expected_node_uuid;
        const auto instance = static_cast<std::uint8_t>(descriptor.instance);
        return {
            [this, node_id, expected_uuid, generation, instance, deadline](
                std::uint32_t frequency_hz, std::uint16_t duty, bool active_low) {
                const auto response = request_runtime_control_packet(
                    node_id, generation, expected_uuid,
                    remotebsp::protocol::Command::PwmCreate,
                    remotebsp::protocol::encode_pwm_create(
                        {instance, frequency_hz, duty, active_low}),
                    0U, deadline);
                if (response.header.object_id == 0U) {
                    throw RuntimeTargetException(
                        remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,
                        true, "Runtime PWM_CREATE 已响应但对象 ID 无效，提交状态未知");
                }
                return response.header.object_id;
            },
            [this](std::uint32_t node_id, std::uint64_t node_generation,
                   const std::array<std::uint8_t, 16>& uuid, std::uint32_t object_id) {
                const auto stop_deadline = std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(1800);
                static_cast<void>(request_runtime_control_packet(
                    node_id, node_generation, uuid,
                    remotebsp::protocol::Command::PwmStop, {}, object_id,
                    stop_deadline));
            }};
    }

    remotebsp::toolbusd::RuntimeControlGate::TimedBitstreamIo timed_bitstream_io(
        const remotebsp::toolbusd::RuntimeTimedBitstreamConfigureRequest& request,
        std::uint64_t generation,
        const remotebsp::protocol::ResourceDescriptor& descriptor,
        std::chrono::steady_clock::time_point deadline) {
        const auto node_id=request.node_id; const auto uuid=request.expected_node_uuid;
        const auto channel=static_cast<std::uint8_t>(descriptor.instance);
        return {
            [this,node_id,uuid,generation,channel,deadline](std::uint32_t period,
                std::uint32_t zero_high,std::uint32_t one_high,std::uint32_t reset_us) {
                const auto response=request_runtime_control_packet(node_id,generation,uuid,
                    remotebsp::protocol::Command::TimedBitstreamCreate,
                    remotebsp::protocol::encode_timed_bitstream_create(
                        {channel,period,zero_high,one_high,reset_us}),0U,deadline);
                if(response.header.object_id==0U) throw RuntimeTargetException(
                    remotebsp::toolbusd::IpcErrorCode::BackendUnavailable,true,
                    "TIMED_BITSTREAM_CREATE响应对象ID无效，提交状态未知");
                return response.header.object_id;
            },
            [this,node_id,uuid,generation,deadline](std::uint32_t object_id,
                std::uint16_t bit_count,const std::vector<std::uint8_t>& data) {
                static_cast<void>(request_runtime_control_packet(node_id,generation,uuid,
                    remotebsp::protocol::Command::TimedBitstreamWrite,
                    remotebsp::protocol::encode_timed_bitstream_write({bit_count,data}),
                    object_id,deadline));
            },
            [this](std::uint32_t node_id,std::uint64_t node_generation,
                const std::array<std::uint8_t,16>& uuid,std::uint32_t object_id) {
                static_cast<void>(request_runtime_control_packet(node_id,node_generation,uuid,
                    remotebsp::protocol::Command::TimedBitstreamAbort,{},object_id,
                    std::chrono::steady_clock::now()+std::chrono::milliseconds(1800)));
            }};
    }

    template <typename Request, typename Operation, typename Begin, typename Invoke>
    remotebsp::toolbusd::RuntimeOperationOutcome runtime_timed_operation(
        const Request& request, const Operation& operation, Begin begin_operation,
        Invoke invoke, bool closes_scope) {
        using namespace remotebsp::toolbusd;
        const auto operation_id=OperationLedger::derive_operation_id(operation);
        const auto digest=OperationLedger::derive_request_digest(operation);
        const auto old=operation_ledger_.lookup(operation_id,request.owner_key_id);
        if(old.disposition==OperationLookupDisposition::Found) {
            if(old.record->request_digest!=digest) throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict,
                "相同 timed-bitstream selector绑定了不同请求");
            return operation_outcome(*old.record,true);
        }
        std::optional<OperationBeginResult> begun; std::optional<OperationRecord> terminal;
        const auto finish=[&](OperationState state,OperationRecovery recovery,
                              const OperationTerminalResult& result) {
            try { std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                return operation_ledger_.finish(begun->record.operation_id,
                    begun->record.request_digest,state,recovery,result);
            } catch(const OperationLedgerException& error) {
                throw OperationLedgerAfterPendingException(error.what()); }
        };
        try {
            static_cast<void>(invoke(RuntimeControlGate::TimedBitstreamDurability{
                [&]{ std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                    begun=begin_operation();
                    if(begun->disposition!=OperationBeginDisposition::StartedDurablePending)
                        throw OperationReplayException{}; },
                [&](const RuntimeTimedBitstreamResult& result){ OperationTerminalResult durable;
                    durable.object_id=result.object_id; terminal=finish(OperationState::Committed,
                        closes_scope?OperationRecovery::SafeClosed:OperationRecovery::None,durable); },
                [&](RuntimeControlGate::DurableRecovery recovery,RuntimeControlError){
                    terminal=finish(recovery==RuntimeControlGate::DurableRecovery::SafeClosed
                        ?OperationState::Rejected:OperationState::Unknown,
                        recovery==RuntimeControlGate::DurableRecovery::SafeClosed
                        ?OperationRecovery::SafeClosed:OperationRecovery::ScopeBlocked,
                        {std::nullopt,std::nullopt,static_cast<std::uint16_t>(RuntimeOperationError::Backend)}); }}));
            if(!terminal) throw std::logic_error("timed-bitstream操作未形成持久终态");
            return operation_outcome(*terminal,false);
        } catch(const OperationReplayException&) { return operation_outcome(begun->record,true); }
        catch(const OperationLedgerAfterPendingException&) { throw; }
        catch(...) {
            if(!begun) throw;
            if(!terminal) terminal=finish(OperationState::Unknown,OperationRecovery::ScopeBlocked,
                {std::nullopt,std::nullopt,static_cast<std::uint16_t>(RuntimeOperationError::Backend)});
            return operation_outcome(*terminal,false);
        }
    }

    remotebsp::toolbusd::RuntimePwmResult runtime_pwm_configure(
        const remotebsp::toolbusd::RuntimePwmConfigureRequest& request,
        const remotebsp::toolbusd::RuntimeControlGate::PwmDurability& durability) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1800);
        auto [generation, descriptor, contract] = resolve_runtime_pwm(request, deadline);
        return runtime_control_.pwm_configure(request, daemon_instance_id_, generation,
            descriptor, contract, pwm_io(request, generation, descriptor, deadline), durability);
    }

    remotebsp::toolbusd::RuntimePwmResult runtime_pwm_stop(
        const remotebsp::toolbusd::RuntimePwmStopRequest& request,
        const remotebsp::toolbusd::RuntimeControlGate::PwmDurability& durability) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1800);
        auto [generation, descriptor, contract] = resolve_runtime_pwm(request, deadline);
        remotebsp::toolbusd::RuntimePwmConfigureRequest io_request;
        io_request.node_id = request.node_id;
        io_request.expected_node_uuid = request.expected_node_uuid;
        return runtime_control_.pwm_stop(request, daemon_instance_id_, generation,
            descriptor, contract, pwm_io(io_request, generation, descriptor, deadline), durability);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_pwm_configure_operation(
        const remotebsp::toolbusd::RuntimePwmConfigureRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimePwmConfigureOperation operation{
            request.daemon_instance_id, request.lease_id, request.expected_node_uuid,
            request.owner_key_id, request.idempotency_key, request.permissions,
            request.node_id, request.resource_id, request.frequency_hz,
            request.duty, request.active_low};
        const auto operation_id = OperationLedger::derive_operation_id(operation);
        const auto digest = OperationLedger::derive_request_digest(operation);
        const auto old = operation_ledger_.lookup(operation_id, request.owner_key_id);
        if (old.disposition == OperationLookupDisposition::Found) {
            if (old.record->request_digest != digest) throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict, "相同 PWM configure selector 绑定了不同配置");
            return operation_outcome(*old.record, true);
        }
        std::optional<OperationBeginResult> begun;
        std::optional<OperationRecord> terminal;
        const auto finish = [&](OperationState state, OperationRecovery recovery,
                                const OperationTerminalResult& result) {
            try {
                std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                return operation_ledger_.finish(begun->record.operation_id,
                    begun->record.request_digest, state, recovery, result);
            } catch (const OperationLedgerException& error) {
                throw OperationLedgerAfterPendingException(error.what());
            }
        };
        try {
            static_cast<void>(runtime_pwm_configure(request, {
                [&] { std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                    begun = operation_ledger_.begin_pwm_configure(operation);
                    if (begun->disposition != OperationBeginDisposition::StartedDurablePending)
                        throw OperationReplayException{}; },
                [&](const RuntimePwmResult& result) {
                    OperationTerminalResult durable;
                    durable.object_id = result.object_id;
                    durable.frequency_hz = result.frequency_hz;
                    durable.duty = result.duty;
                    durable.active_low = result.active_low;
                    terminal = finish(OperationState::Committed, OperationRecovery::None, durable); },
                [&](RuntimeControlGate::DurableRecovery recovery, RuntimeControlError) {
                    terminal = finish(recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                        ? OperationState::Rejected : OperationState::Unknown,
                        recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                        ? OperationRecovery::SafeClosed : OperationRecovery::ScopeBlocked,
                        {std::nullopt, std::nullopt, static_cast<std::uint16_t>(RuntimeOperationError::Backend)}); }}));
            if (!terminal) throw std::logic_error("PWM configure 未形成持久终态");
            return operation_outcome(*terminal, false);
        } catch (const OperationReplayException&) {
            return operation_outcome(begun->record, true);
        } catch (const OperationLedgerAfterPendingException&) { throw; }
        catch (...) {
            if (!begun) throw;
            if (!terminal) terminal = finish(OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt, static_cast<std::uint16_t>(RuntimeOperationError::Backend)});
            return operation_outcome(*terminal, false);
        }
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_pwm_stop_operation(
        const remotebsp::toolbusd::RuntimePwmStopRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimePwmStopOperation operation{request.daemon_instance_id, request.lease_id,
            request.expected_node_uuid, request.owner_key_id, request.idempotency_key,
            request.permissions, request.node_id, request.resource_id};
        const auto operation_id = OperationLedger::derive_operation_id(operation);
        const auto digest = OperationLedger::derive_request_digest(operation);
        const auto old = operation_ledger_.lookup(operation_id, request.owner_key_id);
        if (old.disposition == OperationLookupDisposition::Found) {
            if (old.record->request_digest != digest) throw OperationLedgerException(
                OperationLedgerError::IdempotencyConflict, "相同 PWM stop selector 绑定了不同请求");
            return operation_outcome(*old.record, true);
        }
        std::optional<OperationBeginResult> begun;
        std::optional<OperationRecord> terminal;
        const auto finish = [&](OperationState state, OperationRecovery recovery,
                                const OperationTerminalResult& result) {
            try { std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                return operation_ledger_.finish(begun->record.operation_id,
                    begun->record.request_digest, state, recovery, result);
            } catch (const OperationLedgerException& error) {
                throw OperationLedgerAfterPendingException(error.what()); }
        };
        try {
            static_cast<void>(runtime_pwm_stop(request, {
                [&] { std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
                    begun = operation_ledger_.begin_pwm_stop(operation);
                    if (begun->disposition != OperationBeginDisposition::StartedDurablePending)
                        throw OperationReplayException{}; },
                [&](const RuntimePwmResult& result) { OperationTerminalResult durable;
                    durable.object_id = result.object_id;
                    terminal = finish(OperationState::Committed, OperationRecovery::SafeClosed, durable); },
                [&](RuntimeControlGate::DurableRecovery recovery, RuntimeControlError) {
                    terminal = finish(recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                        ? OperationState::Rejected : OperationState::Unknown,
                        recovery == RuntimeControlGate::DurableRecovery::SafeClosed
                        ? OperationRecovery::SafeClosed : OperationRecovery::ScopeBlocked,
                        {std::nullopt, std::nullopt, static_cast<std::uint16_t>(RuntimeOperationError::Backend)}); }}));
            if (!terminal) throw std::logic_error("PWM stop 未形成持久终态");
            return operation_outcome(*terminal, false);
        } catch (const OperationReplayException&) { return operation_outcome(begun->record, true); }
        catch (const OperationLedgerAfterPendingException&) { throw; }
        catch (...) {
            if (!begun) throw;
            if (!terminal) terminal = finish(OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt, static_cast<std::uint16_t>(RuntimeOperationError::Backend)});
            return operation_outcome(*terminal, false);
        }
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_timed_configure_operation(
        const remotebsp::toolbusd::RuntimeTimedBitstreamConfigureRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimeTimedBitstreamConfigureOperation operation{
            request.daemon_instance_id,request.lease_id,request.expected_node_uuid,
            request.owner_key_id,request.idempotency_key,request.permissions,
            request.node_id,request.resource_id,request.bit_period_ns,
            request.zero_high_ns,request.one_high_ns,request.reset_time_us};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1800);
        auto [generation,descriptor,contract]=resolve_runtime_pwm(request,deadline);
        auto io=timed_bitstream_io(request,generation,descriptor,deadline);
        return runtime_timed_operation(request,operation,
            [&]{return operation_ledger_.begin_timed_bitstream_configure(operation);},
            [&](const RuntimeControlGate::TimedBitstreamDurability& durability){
                return runtime_control_.timed_bitstream_configure(request,daemon_instance_id_,
                    generation,descriptor,contract,io,durability);},false);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_timed_frame_operation(
        const remotebsp::toolbusd::RuntimeTimedBitstreamFrameRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimeTimedBitstreamFrameOperation operation{
            request.daemon_instance_id,request.lease_id,request.expected_node_uuid,
            request.owner_key_id,request.idempotency_key,request.permissions,
            request.node_id,request.resource_id,request.bit_count,request.data};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1800);
        auto [generation,descriptor,contract]=resolve_runtime_pwm(request,deadline);
        RuntimeTimedBitstreamConfigureRequest config; config.node_id=request.node_id;
        config.expected_node_uuid=request.expected_node_uuid;
        auto io=timed_bitstream_io(config,generation,descriptor,deadline);
        return runtime_timed_operation(request,operation,
            [&]{return operation_ledger_.begin_timed_bitstream_frame(operation);},
            [&](const RuntimeControlGate::TimedBitstreamDurability& durability){
                return runtime_control_.timed_bitstream_frame(request,daemon_instance_id_,
                    generation,descriptor,contract,io,durability);},false);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_timed_stop_operation(
        const remotebsp::toolbusd::RuntimeTimedBitstreamStopRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimeTimedBitstreamStopOperation operation{
            request.daemon_instance_id,request.lease_id,request.expected_node_uuid,
            request.owner_key_id,request.idempotency_key,request.permissions,
            request.node_id,request.resource_id};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1800);
        auto [generation,descriptor,contract]=resolve_runtime_pwm(request,deadline);
        RuntimeTimedBitstreamConfigureRequest config; config.node_id=request.node_id;
        config.expected_node_uuid=request.expected_node_uuid;
        auto io=timed_bitstream_io(config,generation,descriptor,deadline);
        return runtime_timed_operation(request,operation,
            [&]{return operation_ledger_.begin_timed_bitstream_stop(operation);},
            [&](const RuntimeControlGate::TimedBitstreamDurability& durability){
                return runtime_control_.timed_bitstream_stop(request,daemon_instance_id_,
                    generation,descriptor,contract,io,durability);},true);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome
    runtime_control_release_operation(
        const remotebsp::toolbusd::RuntimeControlReleaseRequest& request) {
        using namespace remotebsp::toolbusd;
        if (request.daemon_instance_id != daemon_instance_id_) {
            throw RuntimeControlException(
                RuntimeControlError::DaemonIdentityMismatch,
                "Runtime Release 操作绑定了其他 toolbusd 实例");
        }
        RuntimeControlReleaseOperation operation;
        operation.daemon_origin = request.daemon_instance_id;
        operation.lease_id = request.lease_id;
        operation.owner_key_id = request.owner_key_id;
        const auto operation_id =
            OperationLedger::derive_operation_id(operation);
        const auto resolved_release = runtime_control_.resolve_release_lease(
            request, daemon_instance_id_);
        const auto existing = operation_ledger_.lookup(
            operation_id, request.owner_key_id);
        if (existing.disposition == OperationLookupDisposition::Found) {
            if (existing.record->daemon_origin !=
                request.daemon_instance_id) {
                throw OperationLedgerException(
                    OperationLedgerError::IdempotencyConflict,
                    "相同 Runtime Release operation_id 来自其他 daemon");
            }
            if (resolved_release.has_value()) {
                const ServerResolvedLease lease{
                    resolved_release->expected_node_uuid,
                    resolved_release->node_id,
                    resolved_release->resource_id,
                    resolved_release->permissions,
                    resolved_release->admission_id};
                if (existing.record->request_digest !=
                    OperationLedger::derive_request_digest(operation, lease)) {
                    throw OperationLedgerException(
                        OperationLedgerError::IdempotencyConflict,
                        "Runtime Release 历史记录与当前租约范围不一致");
                }
            }
            return operation_outcome(*existing.record, true);
        }

        std::optional<OperationBeginResult> begun;
        std::optional<OperationRecord> durable_terminal;
        const auto finish_durable = [&](OperationState state,
                                        OperationRecovery recovery,
                                        const OperationTerminalResult& result =
                                            OperationTerminalResult{}) {
            if (!begun.has_value() ||
                begun->disposition !=
                    OperationBeginDisposition::StartedDurablePending) {
                throw std::logic_error(
                    "Runtime Release 缺少持久 pending");
            }
            try {
                std::lock_guard<std::mutex> policy_lock(
                    runtime_operation_policy_mutex_);
                return operation_ledger_.finish(
                    begun->record.operation_id, begun->record.request_digest,
                    state, recovery, result);
            } catch (const OperationLedgerException& error) {
                throw OperationLedgerAfterPendingException(error.what());
            }
        };
        try {
            runtime_control_.release(
                request, daemon_instance_id_,
                RuntimeControlGate::ReleaseDurability{
                    [&](const RuntimeControlGate::ResolvedReleaseLease& lease) {
                        std::lock_guard<std::mutex> policy_lock(
                            runtime_operation_policy_mutex_);
                        begun = operation_ledger_.begin_release(
                            operation,
                            {lease.expected_node_uuid, lease.node_id,
                             lease.resource_id, lease.permissions,
                             lease.admission_id});
                        if (begun->disposition !=
                            OperationBeginDisposition::StartedDurablePending) {
                            throw OperationReplayException{};
                        }
                    },
                    [&] {
                        durable_terminal = finish_durable(
                            OperationState::Committed,
                            OperationRecovery::SafeClosed);
                    },
                    [&](RuntimeControlGate::DurableRecovery recovery,
                        RuntimeControlError) {
                        if (!begun.has_value()) {
                            return;
                        }
                        durable_terminal = finish_durable(
                            recovery == RuntimeControlGate::DurableRecovery::
                                            SafeClosed
                                ? OperationState::Rejected
                                : OperationState::Unknown,
                            recovery == RuntimeControlGate::DurableRecovery::
                                            SafeClosed
                                ? OperationRecovery::SafeClosed
                                : OperationRecovery::ScopeBlocked,
                            {std::nullopt, std::nullopt,
                             static_cast<std::uint16_t>(
                                 RuntimeOperationError::Backend)});
                    }});
            if (!durable_terminal.has_value()) {
                throw std::logic_error(
                    "Runtime Release Gate 未提交持久终态");
            }
            return operation_outcome(*durable_terminal, false);
        } catch (const OperationReplayException&) {
            if (!begun.has_value()) {
                throw;
            }
            return operation_outcome(begun->record, true);
        } catch (const OperationLedgerAfterPendingException&) {
            throw;
        } catch (const RuntimeControlException&) {
            if (durable_terminal.has_value()) {
                return operation_outcome(*durable_terminal, false);
            }
            throw;
        } catch (const OperationLedgerException&) {
            throw;
        } catch (const std::exception&) {
            if (!begun.has_value()) {
                throw;
            }
            durable_terminal = finish_durable(
                OperationState::Unknown, OperationRecovery::ScopeBlocked,
                {std::nullopt, std::nullopt,
                 static_cast<std::uint16_t>(
                     RuntimeOperationError::Backend)});
            return operation_outcome(*durable_terminal, false);
        }
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_bus_reset_operation(
        const remotebsp::toolbusd::RuntimeBusResourceResetRequest& request) {
        using namespace remotebsp::toolbusd;
        RuntimeBusResourceResetOperation operation{
            request.daemon_instance_id, request.lease_id,
            request.expected_node_uuid, request.owner_key_id,
            request.idempotency_key, request.permissions,
            request.node_id, request.resource_id};
        const auto operation_id = OperationLedger::derive_operation_id(operation);
        const auto digest = OperationLedger::derive_request_digest(operation);
        const auto old = operation_ledger_.lookup(operation_id, request.owner_key_id);
        if (old.disposition == OperationLookupDisposition::Found) {
            if (old.record->request_digest != digest)
                throw OperationLedgerException(OperationLedgerError::IdempotencyConflict,
                                               "相同总线复位 selector 绑定了不同请求");
            return operation_outcome(*old.record, true);
        }
        RuntimeControlReleaseRequest lease_request;
        lease_request.daemon_instance_id = request.daemon_instance_id;
        lease_request.lease_id = request.lease_id;
        lease_request.owner_key_id = request.owner_key_id;
        const auto lease = runtime_control_.resolve_release_lease(
            lease_request, daemon_instance_id_);
        if (!lease.has_value() || lease->permissions != kRuntimePermissionBusReset ||
            lease->expected_node_uuid != request.expected_node_uuid ||
            lease->node_id != request.node_id ||
            lease->resource_id != request.resource_id) {
            throw RuntimeControlException(RuntimeControlError::LeaseConflict,
                                          "总线复位租约身份或范围不匹配");
        }
        std::uint64_t generation{};
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(request.node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                node->identity.uuid != request.expected_node_uuid)
                throw RuntimeTargetException(IpcErrorCode::NodeUnavailable, false,
                                             "总线复位发送前目标节点不可用");
            generation = bus_node_generations_[request.node_id];
        }
        OperationBeginResult begun;
        {
            std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
            begun = operation_ledger_.begin_bus_resource_reset(operation);
        }
        if (begun.disposition != OperationBeginDisposition::StartedDurablePending)
            return operation_outcome(begun.record, true);
        const auto finish = [&](OperationState state, OperationRecovery recovery,
                                RuntimeOperationError error) {
            std::lock_guard<std::mutex> lock(runtime_operation_policy_mutex_);
            return operation_ledger_.finish(operation_id, digest, state, recovery,
                {std::nullopt, std::nullopt, static_cast<std::uint16_t>(error)});
        };
        try {
            static_cast<void>(request_runtime_control_packet(
                request.node_id, generation, request.expected_node_uuid,
                remotebsp::protocol::Command::ResourceReset,
                remotebsp::protocol::encode_resource_id(request.resource_id),
                0U, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1800)));
#ifdef REMOTEBSP_TEST_HOOKS
            if (drop_bus_reset_response_ordinal_ != 0U &&
                ++bus_reset_response_count_ ==
                    drop_bus_reset_response_ordinal_) {
                throw RuntimeTargetException(IpcErrorCode::DeadlineExceeded,
                    true, "测试故障：总线复位已提交后丢失响应");
            }
#endif
            runtime_control_.forget_lease_after_remote_reset(
                lease_request, daemon_instance_id_);
            return operation_outcome(finish(OperationState::Committed,
                OperationRecovery::SafeClosed, RuntimeOperationError::None), false);
        } catch (const RuntimeTargetException& error) {
            return operation_outcome(finish(
                error.possibly_committed() ? OperationState::Unknown
                                           : OperationState::Rejected,
                error.possibly_committed() ? OperationRecovery::ScopeBlocked
                                           : OperationRecovery::NotSent,
                error.code() == IpcErrorCode::DeadlineExceeded
                    ? RuntimeOperationError::Deadline
                    : RuntimeOperationError::Backend), false);
        } catch (...) {
            return operation_outcome(finish(OperationState::Unknown,
                OperationRecovery::ScopeBlocked,
                RuntimeOperationError::Backend), false);
        }
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_operation_query(
        const remotebsp::toolbusd::RuntimeOperationQuery& request) {
        using namespace remotebsp::toolbusd;
        if (request.daemon_instance_id != daemon_instance_id_) {
            throw RuntimeControlException(
                RuntimeControlError::DaemonIdentityMismatch,
                "Runtime 操作查询绑定了其他 toolbusd 实例");
        }
        const auto found = operation_ledger_.lookup(
            request.operation_id, request.owner_key_id);
        return found.disposition == OperationLookupDisposition::Found
                   ? operation_outcome(*found.record, true)
                   : expired_operation_outcome(request.operation_id);
    }

    remotebsp::toolbusd::RuntimeOperationOutcome runtime_operation_lookup(
        const remotebsp::toolbusd::RuntimeOperationLookup& request) {
        using namespace remotebsp::toolbusd;
        if (request.daemon_instance_id != daemon_instance_id_) {
            throw RuntimeControlException(
                RuntimeControlError::DaemonIdentityMismatch,
                "Runtime 操作定位绑定了其他 toolbusd 实例");
        }
        OperationDigest operation_id{};
        if (request.kind == RuntimeOperationKind::GpioWrite) {
            RuntimeGpioWriteOperation operation;
            operation.daemon_origin = request.daemon_instance_id;
            operation.lease_id = request.lease_id;
            operation.owner_key_id = request.owner_key_id;
            operation.idempotency_key = request.idempotency_key;
            operation_id = OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::ControlRelease) {
            RuntimeControlReleaseOperation operation;
            operation.daemon_origin = request.daemon_instance_id;
            operation.lease_id = request.lease_id;
            operation.owner_key_id = request.owner_key_id;
            operation_id = OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::PwmConfigure) {
            RuntimePwmConfigureOperation operation;
            operation.daemon_origin = request.daemon_instance_id;
            operation.lease_id = request.lease_id;
            operation.owner_key_id = request.owner_key_id;
            operation.idempotency_key = request.idempotency_key;
            operation_id = OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::PwmStop) {
            RuntimePwmStopOperation operation;
            operation.daemon_origin = request.daemon_instance_id;
            operation.lease_id = request.lease_id;
            operation.owner_key_id = request.owner_key_id;
            operation.idempotency_key = request.idempotency_key;
            operation_id = OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::TimedBitstreamConfigure) {
            RuntimeTimedBitstreamConfigureOperation operation;
            operation.daemon_origin=request.daemon_instance_id;operation.lease_id=request.lease_id;
            operation.owner_key_id=request.owner_key_id;operation.idempotency_key=request.idempotency_key;
            operation_id=OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::TimedBitstreamFrame) {
            RuntimeTimedBitstreamFrameOperation operation;
            operation.daemon_origin=request.daemon_instance_id;operation.lease_id=request.lease_id;
            operation.owner_key_id=request.owner_key_id;operation.idempotency_key=request.idempotency_key;
            operation_id=OperationLedger::derive_operation_id(operation);
        } else if (request.kind == RuntimeOperationKind::TimedBitstreamStop) {
            RuntimeTimedBitstreamStopOperation operation;
            operation.daemon_origin=request.daemon_instance_id;operation.lease_id=request.lease_id;
            operation.owner_key_id=request.owner_key_id;operation.idempotency_key=request.idempotency_key;
            operation_id=OperationLedger::derive_operation_id(operation);
        } else {
            RuntimeBusResourceResetOperation operation;
            operation.daemon_origin=request.daemon_instance_id;operation.lease_id=request.lease_id;
            operation.owner_key_id=request.owner_key_id;operation.idempotency_key=request.idempotency_key;
            operation_id=OperationLedger::derive_operation_id(operation);
        }
        const auto found = operation_ledger_.lookup(
            operation_id, request.owner_key_id);
        return found.disposition == OperationLookupDisposition::Found
                   ? operation_outcome(*found.record, true)
                   : expired_operation_outcome(operation_id, request.kind);
    }

    void invalidate_bus_node_locked(std::uint32_t node_id) {
        if (node_id == 0U || node_id > kMaximumNodeId) {
            return;
        }
        auto& generation = bus_node_generations_[node_id];
        ++generation;
        if (generation == 0U) {
            ++generation;
        }
        bus_runtime_.invalidate_node(node_id);
        const auto credit_prefix = std::to_string(node_id) + ":";
        for (auto entry = stream_credit_history_.begin();
             entry != stream_credit_history_.end();) {
            if (entry->rfind(credit_prefix, 0U) == 0U) {
                entry = stream_credit_history_.erase(entry);
            } else {
                ++entry;
            }
        }
        const auto route = kNodeRequestBaseRoute + node_id;
        for (auto entry = request_routes_.begin();
             entry != request_routes_.end();) {
            if (entry->second != route) {
                ++entry;
                continue;
            }
            const auto key = entry->first;
            const auto session_id = static_cast<std::uint32_t>(key >> 32U);
            const auto request_id = static_cast<std::uint32_t>(key);
            static_cast<void>(requests_.cancel(session_id, request_id));
            responses_.erase(key);
            timed_out_.insert(key);
            entry = request_routes_.erase(entry);
        }
        state_changed_.notify_all();
    }

    std::optional<std::uint64_t> ensure_bus_contract(
        std::uint32_t node_id, remotebsp::protocol::Command transfer_command,
        std::uint32_t resource_id) {
        std::uint64_t node_generation = 0U;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                node_id > kMaximumNodeId) {
                throw std::runtime_error(
                    "目标节点尚未发现或已经离线");
            }
            node_generation = bus_node_generations_[node_id];
        }
        const auto expected_kind =
            transfer_command == remotebsp::protocol::Command::I2cTransfer
                ? remotebsp::protocol::BusResourceKind::I2cDevice
                : remotebsp::protocol::BusResourceKind::SpiDevice;
        const auto expected_resource_type =
            transfer_command == remotebsp::protocol::Command::I2cTransfer
                ? remotebsp::protocol::ResourceType::I2cDevice
                : remotebsp::protocol::ResourceType::SpiDevice;
        if (static_cast<std::uint8_t>(resource_id >> 24U) !=
            static_cast<std::uint8_t>(expected_resource_type)) {
            throw std::invalid_argument(
                "总线事务的设备资源 ID 命名空间与命令不匹配");
        }
        const auto cached = bus_runtime_.find_contract(node_id, resource_id);
        if (cached.has_value() && cached->kind == expected_kind) {
            return node_generation;
        }
        auto load = bus_runtime_.begin_contract_load(node_id, resource_id);
        if (!load) {
            return std::nullopt;
        }
        // 首次检查和取得单飞令牌之间，另一线程可能刚完成加载。
        const auto loaded = bus_runtime_.find_contract(node_id, resource_id);
        if (loaded.has_value() && loaded->kind == expected_kind) {
            return node_generation;
        }
        const auto contract_command =
            transfer_command == remotebsp::protocol::Command::I2cTransfer
                ? remotebsp::protocol::Command::I2cContract
                : remotebsp::protocol::Command::SpiContract;
        const auto body = request_snapshot_resource(
            node_id, contract_command,
            remotebsp::protocol::encode_resource_id(resource_id),
            std::chrono::steady_clock::now() +
                std::chrono::milliseconds(2500));
        const auto contract =
            remotebsp::protocol::decode_bus_resource_contract(body);
        if (contract.resource_id != resource_id) {
            throw std::runtime_error(
                "远端总线设备合同的资源 ID 与查询目标不一致");
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto* node = nodes_.find_by_node_id(node_id);
        if (node == nullptr || !node->online || !node->assigned ||
            node_id > kMaximumNodeId ||
            bus_node_generations_[node_id] != node_generation) {
            throw std::runtime_error(
                "合同查询期间目标节点已离线或重新启动");
        }
        const auto update =
            bus_runtime_.remember_contract(node_id, expected_kind, contract);
        if (update == remotebsp::toolbusd::BusContractUpdate::CapacityReached) {
            throw std::runtime_error("toolbusd 总线合同缓存容量已满");
        }
        if (update == remotebsp::toolbusd::BusContractUpdate::Invalid) {
            throw std::runtime_error("远端返回的总线设备合同与请求不匹配");
        }
        return node_generation;
    }

    std::uint64_t ensure_stream_contract(
        std::uint32_t node_id, std::uint32_t resource_id) {
        std::uint64_t node_generation = 0U;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(node_id);
            if (node == nullptr || !node->online || !node->assigned ||
                node_id > kMaximumNodeId) {
                throw std::runtime_error("目标节点尚未发现或已经离线");
            }
            node_generation = bus_node_generations_[node_id];
        }
        if (static_cast<std::uint8_t>(resource_id >> 24U) !=
            static_cast<std::uint8_t>(
                remotebsp::protocol::ResourceType::Stream)) {
            throw std::invalid_argument(
                "STREAM 资源 ID 命名空间与命令不匹配");
        }
        const auto body = request_snapshot_resource(
            node_id, remotebsp::protocol::Command::StreamContract,
            remotebsp::protocol::encode_resource_id(resource_id),
            std::chrono::steady_clock::now() +
                std::chrono::milliseconds(2500));
        const auto contract =
            remotebsp::protocol::decode_stream_contract(body);
        if (contract.resource_id != resource_id) {
            throw std::runtime_error(
                "远端 STREAM 合同的资源 ID 与打开目标不一致");
        }
        if ((contract.transport_mask &
             remotebsp::protocol::kStreamTransportUsb) == 0U) {
            throw std::invalid_argument(
                "STREAM 静态合同未授权 USB，拒绝在当前链路打开");
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto* node = nodes_.find_by_node_id(node_id);
        if (node == nullptr || !node->online || !node->assigned ||
            bus_node_generations_[node_id] != node_generation) {
            throw std::runtime_error(
                "STREAM 合同查询期间目标节点已离线或重新启动");
        }
        return node_generation;
    }

    void remember_direct_bus_contract_locked(
        std::uint32_t node_id, std::uint64_t node_generation,
        const remotebsp::protocol::Packet& request,
        const remotebsp::protocol::Packet& response) noexcept {
        try {
            if (node_id == 0U || node_id > kMaximumNodeId ||
                bus_node_generations_[node_id] != node_generation ||
                request.header.object_id != 0U ||
                response.header.message_type !=
                    remotebsp::protocol::MessageType::Response ||
                response.payload.empty() || response.payload.front() != 0U ||
                (response.header.flags &
                 remotebsp::protocol::kErrorResponseFlag) != 0U) {
                return;
            }
            const auto command = static_cast<remotebsp::protocol::Command>(
                request.header.command);
            const auto expected_kind =
                command == remotebsp::protocol::Command::I2cContract
                    ? remotebsp::protocol::BusResourceKind::I2cDevice
                    : remotebsp::protocol::BusResourceKind::SpiDevice;
            if ((command != remotebsp::protocol::Command::I2cContract &&
                 command != remotebsp::protocol::Command::SpiContract) ||
                response.header.command != request.header.command) {
                return;
            }
            const auto queried_resource_id =
                remotebsp::protocol::decode_resource_id(request.payload);
            const std::vector<std::uint8_t> body(
                response.payload.begin() + 1U, response.payload.end());
            const auto contract =
                remotebsp::protocol::decode_bus_resource_contract(body);
            if (contract.resource_id != queried_resource_id) {
                return;
            }
            static_cast<void>(bus_runtime_.remember_contract(
                node_id, expected_kind, contract));
        } catch (const std::exception&) {
            // 查询目标、响应合同或边界不一致时绝不能污染缓存。
        }
    }

    static void write_local_bus_result(
        int client, const remotebsp::protocol::Packet& request,
        remotebsp::protocol::BusTransactionStatus status) {
        remotebsp::protocol::Packet response;
        response.header.message_type =
            remotebsp::protocol::MessageType::Response;
        response.header.command = request.header.command;
        response.header.session_id = request.header.session_id;
        response.header.request_id = request.header.request_id;
        response.header.object_id = request.header.object_id;
        response.payload.push_back(0U);
        const auto result = remotebsp::protocol::encode_bus_transfer_result(
            {status, 0U, 0U, {}});
        response.payload.insert(response.payload.end(), result.begin(),
                                result.end());
        remotebsp::toolbusd::write_ipc_response(
            client, remotebsp::toolbusd::IpcStatus::Ok,
            remotebsp::protocol::encode(std::move(response)));
    }

    remotebsp::toolbusd::IpcRuntimeSnapshot build_runtime_snapshot(
        std::uint16_t maximum_resources,
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        remotebsp::toolbusd::IpcRuntimeSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (const auto& node : nodes_.records()) {
                snapshot.nodes.push_back(
                    {node.identity.uuid, node.node_id, node.online,
                     node.assigned, node.identity.firmware_major,
                     node.identity.firmware_minor,
                     node.identity.firmware_patch,
                     node.identity.board_type,
                     node.identity.protocol_version});
            }
        }
        for (const auto& node : snapshot.nodes) {
            if (!node.online || !node.ready) {
                continue;
            }
            std::vector<std::uint8_t> descriptor_body;
            try {
                descriptor_body = request_snapshot_resource(
                    node.node_id,
                    remotebsp::protocol::Command::ResourceEnum, {},
                    deadline);
            } catch (const std::runtime_error&) {
                snapshot.node_issues.push_back(
                    {node.node_id,
                     remotebsp::toolbusd::IpcRuntimeNodeError::
                         ResourceInventoryUnavailable});
                continue;
            }
            const auto descriptors =
                remotebsp::protocol::decode_resource_list(descriptor_body);
            if (descriptors.size() >
                static_cast<std::size_t>(maximum_resources) -
                    snapshot.resources.size()) {
                throw std::runtime_error(
                    "Runtime 快照资源数量超过请求上限");
            }
            for (const auto& descriptor : descriptors) {
                std::vector<std::uint8_t> status_body;
                try {
                    status_body = request_snapshot_resource(
                        node.node_id,
                        remotebsp::protocol::Command::ResourceStatus,
                        remotebsp::protocol::encode_resource_id(
                            descriptor.resource_id),
                        deadline);
                } catch (const std::runtime_error&) {
                    remotebsp::protocol::ResourceStatusPayload unavailable;
                    unavailable.resource_id = descriptor.resource_id;
                    snapshot.resources.push_back(
                        {node.node_id, false, descriptor, unavailable});
                    continue;
                }
                const auto status =
                    remotebsp::protocol::decode_resource_status(status_body);
                if (status.resource_id != descriptor.resource_id) {
                    throw std::runtime_error(
                        "Runtime 快照资源状态 ID 不匹配");
                }
                snapshot.resources.push_back(
                    {node.node_id, true, descriptor, status});
            }
        }

        std::vector<remotebsp::toolbusd::IpcNodeInfo> final_nodes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto host_now_count =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (host_now_count < 0) {
                throw std::runtime_error(
                    "主机单调时钟不能转换为 Runtime 快照时间");
            }
            const auto host_now_ns =
                static_cast<std::uint64_t>(host_now_count);
            for (const auto& node : nodes_.records()) {
                final_nodes.push_back(
                    {node.identity.uuid, node.node_id, node.online,
                     node.assigned, node.identity.firmware_major,
                     node.identity.firmware_minor,
                     node.identity.firmware_patch,
                     node.identity.board_type,
                     node.identity.protocol_version});
                remotebsp::toolbusd::IpcRuntimeClockQuality clock;
                clock.node_id = node.node_id;
                const auto quality =
                    nodes_.clock_quality(node.node_id, host_now_ns);
                if (quality.has_value()) {
                    if (quality->estimate.sample_count >
                            std::numeric_limits<std::uint16_t>::max() ||
                        quality->estimate.selected_sample_count >
                            std::numeric_limits<std::uint16_t>::max()) {
                        throw std::runtime_error(
                            "Runtime 快照时钟样本数量超过 IPC 上限");
                    }
                    clock.registered = true;
                    clock.estimate_valid = quality->estimate.valid;
                    clock.state = quality->estimate.state;
                    clock.boot_epoch = quality->boot_epoch;
                    clock.model_generation = quality->model_generation;
                    clock.sample_count = static_cast<std::uint16_t>(
                        quality->estimate.sample_count);
                    clock.selected_sample_count =
                        static_cast<std::uint16_t>(
                            quality->estimate.selected_sample_count);
                    if (quality->estimate.valid) {
                        const auto rate_ppb = std::round(
                            quality->estimate.rate_deviation_ppm * 1000.0L);
                        if (!std::isfinite(rate_ppb) ||
                            rate_ppb < static_cast<long double>(
                                std::numeric_limits<std::int32_t>::min()) ||
                            rate_ppb > static_cast<long double>(
                                std::numeric_limits<std::int32_t>::max())) {
                            throw std::runtime_error(
                                "Runtime 快照时钟漂移超过 IPC 上限");
                        }
                        clock.rate_deviation_ppb =
                            static_cast<std::int32_t>(rate_ppb);
                        clock.drift_uncertainty_ppm =
                            quality->estimate.drift_uncertainty_ppm;
                        clock.minimum_network_rtt_ns =
                            quality->estimate.minimum_network_rtt_ns;
                        clock.error_bound_ns =
                            quality->estimate.error_bound_ns;
                        clock.sample_age_ns =
                            quality->estimate.sample_age_ns;
                        clock.last_sample_host_time_ns =
                            quality->estimate.last_sample_host_time_ns;
                    }
                }
                snapshot.clocks.push_back(clock);
            }
        }
        const auto same_node = [](const auto& left, const auto& right) {
            return left.uuid == right.uuid &&
                   left.node_id == right.node_id &&
                   left.online == right.online &&
                   left.ready == right.ready &&
                   left.firmware_major == right.firmware_major &&
                   left.firmware_minor == right.firmware_minor &&
                   left.firmware_patch == right.firmware_patch &&
                   left.board_type == right.board_type &&
                   left.protocol_version == right.protocol_version;
        };
        if (snapshot.nodes.size() != final_nodes.size() ||
            !std::equal(snapshot.nodes.begin(), snapshot.nodes.end(),
                        final_nodes.begin(), same_node)) {
            throw std::runtime_error(
                "Runtime 快照期间节点拓扑发生变化");
        }
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            snapshot.traffic = traffic_.snapshot();
        }
        for (const auto& item : bus_runtime_.telemetry_snapshot().resources) {
            snapshot.bus_health.push_back({
                item.node_id, item.resource_id,
                item.last_remote_status_valid, item.last_remote_status,
                item.consecutive_remote_failures,
                item.peak_consecutive_remote_failures,
                item.last_remote_result_time_us});
        }
        snapshot.sequence = next_runtime_snapshot_sequence_++;
        if (snapshot.sequence == 0U) {
            snapshot.sequence = next_runtime_snapshot_sequence_++;
        }
        return snapshot;
    }

    remotebsp::toolbusd::IpcToolbusdHealthSnapshot
    build_health_snapshot() {
        remotebsp::toolbusd::ToolbusdHealthObservation observation;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            observation.request_queue_depth = requests_.pending_count();
        }
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            observation.traffic = traffic_.snapshot();
        }
        observation.active_lease_count =
            runtime_control_.active_lease_count();
        observation.bus_telemetry = bus_runtime_.telemetry_snapshot();
        observation.operation_ledger_mutation_available =
            operation_ledger_.mutation_available();
        if (*observation.operation_ledger_mutation_available) {
            observation.operation_ledger_operation_count =
                operation_ledger_.operation_count();
        }
        observation.sample_time_ms = steady_time_ms();

        remotebsp::toolbusd::IpcToolbusdHealthSnapshot result;
        result.daemon_instance_id = daemon_instance_id_;
        result.health = health_producer_.capture(observation);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            result.ipc_active_clients = active_clients_;
            result.ipc_maximum_clients = maximum_ipc_clients_;
            result.ipc_peak_clients = ipc_peak_clients_;
            result.ipc_accepted_total = ipc_accepted_total_;
            result.ipc_capacity_rejected_total = ipc_capacity_rejected_total_;
            result.ipc_oversized_frame_total = ipc_oversized_frame_total_;
            result.ipc_timeout_total = ipc_timeout_total_;
            result.ipc_thread_creation_failed_total =
                ipc_thread_creation_failed_total_;
        }
        return result;
    }

    static void increment_saturating(std::uint64_t& counter) noexcept {
        if (counter != std::numeric_limits<std::uint64_t>::max()) {
            ++counter;
        }
    }

    void handle_client(int client) {
        std::optional<remotebsp::toolbusd::IpcRequestKind>
            structured_error_kind;
        try {
            auto ipc_request =
                remotebsp::toolbusd::read_ipc_request(client);
            if (uses_structured_error(ipc_request.kind)) {
                structured_error_kind = ipc_request.kind;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::ListNodes) {
                std::vector<remotebsp::toolbusd::IpcNodeInfo> result;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    for (const auto& node : nodes_.records()) {
                        result.push_back(
                            {node.identity.uuid, node.node_id, node.online,
                             node.assigned,
                             node.identity.firmware_major,
                             node.identity.firmware_minor,
                             node.identity.firmware_patch,
                             node.identity.board_type,
                             node.identity.protocol_version});
                    }
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_node_list(result));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::RuntimeControlAcquire) {
                runtime_control_acquire(ipc_request.runtime_control_acquire);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok, {});
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::TrafficStatus) {
                remotebsp::toolbusd::TrafficSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    snapshot = traffic_.snapshot();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_traffic_status(
                        snapshot));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::DaemonIdentity) {
                remotebsp::toolbusd::IpcDaemonIdentity identity;
                identity.instance_id = daemon_instance_id_;
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_daemon_identity(
                        identity));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::HealthSnapshot) {
                const auto snapshot = build_health_snapshot();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_health_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        LogicalRecordingStart) {
                if (!recording_) throw std::runtime_error(
                    "toolbusd 未配置逻辑链路录制固定目录");
                recording_->start(ipc_request.logical_recording_name);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok, {});
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        LogicalRecordingStop) {
                if (!recording_) throw std::runtime_error(
                    "toolbusd 未配置逻辑链路录制固定目录");
                const auto path = recording_->stop();
                const auto name = std::filesystem::path(path).filename().string();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    std::vector<std::uint8_t>(name.begin(), name.end()));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        LogicalRecordingStatus) {
                remotebsp::toolbusd::IpcLogicalRecordingStatus result;
                result.configured = static_cast<bool>(recording_);
                if (recording_) {
                    const auto source = recording_->status();
                    result.active = source.active;
                    result.output_name = source.output_name;
                    result.event_count = source.event_count;
                    result.maximum_events = source.maximum_events;
                    result.maximum_file_bytes = source.maximum_file_bytes;
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_logical_recording_status(result));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::RuntimeGpioWrite) {
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Error,
                    remotebsp::toolbusd::encode_ipc_error_envelope(
                        {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                         remotebsp::toolbusd::IpcErrorCode::UnsupportedRequest,
                         remotebsp::toolbusd::IpcErrorCategory::Request,
                         false, false,
                         "旧版 Runtime GPIO 写接口未接入持久账本，已拒绝"}));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::
                    RuntimeControlRelease) {
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Error,
                    remotebsp::toolbusd::encode_ipc_error_envelope(
                        {remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                         remotebsp::toolbusd::IpcErrorCode::UnsupportedRequest,
                         remotebsp::toolbusd::IpcErrorCategory::Request,
                         false, false,
                         "旧版 Runtime Release 接口未接入持久账本，已拒绝"}));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::
                    RuntimeGpioWriteOperation) {
                const auto outcome = runtime_gpio_write_operation(
                    ipc_request.runtime_gpio_write);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::
                        encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::
                    RuntimeControlReleaseOperation) {
                const auto outcome = runtime_control_release_operation(
                    ipc_request.runtime_control_release);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::
                        encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimePwmConfigureOperation) {
                const auto outcome = runtime_pwm_configure_operation(
                    ipc_request.runtime_pwm_configure);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimePwmStopOperation) {
                const auto outcome = runtime_pwm_stop_operation(
                    ipc_request.runtime_pwm_stop);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimeTimedBitstreamConfigureOperation) {
                const auto outcome=runtime_timed_configure_operation(
                    ipc_request.runtime_timed_bitstream_configure);
                remotebsp::toolbusd::write_ipc_response(client,
                    remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimeTimedBitstreamFrameOperation) {
                const auto outcome=runtime_timed_frame_operation(
                    ipc_request.runtime_timed_bitstream_frame);
                remotebsp::toolbusd::write_ipc_response(client,
                    remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimeTimedBitstreamStopOperation) {
                const auto outcome=runtime_timed_stop_operation(
                    ipc_request.runtime_timed_bitstream_stop);
                remotebsp::toolbusd::write_ipc_response(client,
                    remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::IpcRequestKind::
                                        RuntimeBusResourceResetOperation) {
                const auto outcome = runtime_bus_reset_operation(
                    ipc_request.runtime_bus_resource_reset);
                remotebsp::toolbusd::write_ipc_response(client,
                    remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::
                    RuntimeOperationQuery) {
                const auto outcome = runtime_operation_query(
                    ipc_request.runtime_operation_query);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::
                        encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::
                    RuntimeOperationLookup) {
                const auto outcome = runtime_operation_lookup(
                    ipc_request.runtime_operation_lookup);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::
                        encode_ipc_runtime_operation_outcome(outcome));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::RuntimeSnapshot) {
                const auto snapshot_deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ipc_request.timeout_ms);
                std::unique_lock<std::timed_mutex> snapshot_lock(
                    runtime_snapshot_mutex_, std::defer_lock);
                if (!snapshot_lock.try_lock_until(snapshot_deadline)) {
                    throw std::runtime_error(
                        "等待其他 Runtime 快照达到总时间上限");
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= snapshot_deadline) {
                    throw std::runtime_error(
                        "Runtime 快照达到总时间上限");
                }
                const auto remaining =
                    std::max(std::chrono::milliseconds(1),
                             std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 snapshot_deadline - now));
                const auto snapshot = build_runtime_snapshot(
                    static_cast<std::uint16_t>(
                        ipc_request.maximum_length),
                    remaining);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupSubmit) {
                auto motion_guard = motion_dispatch_gate_.lock();
                std::vector<remotebsp::toolbusd::MotionGroupDispatch>
                    dispatches;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    const auto state = motion_group_.state();
                    if (state ==
                            remotebsp::toolbusd::MotionGroupState::Committed ||
                        state ==
                            remotebsp::toolbusd::MotionGroupState::Aborted) {
                        motion_group_.reset();
                    } else if (state !=
                               remotebsp::toolbusd::MotionGroupState::Idle) {
                        throw std::runtime_error(
                            "已有运动组事务正在执行；全局一次只允许一个事务");
                    }
                    dispatches = motion_group_.start(
                        requests_, ipc_request.motion_group_plan, nodes_,
                        session_id_, steady_time_ns()).dispatches;
                }
                static_cast<void>(send_motion_dispatches(dispatches));
                motion_guard.unlock();
                remotebsp::toolbusd::MotionGroupServiceSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    snapshot = motion_group_.snapshot();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_motion_group_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupStatus ||
                ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupCancel) {
                std::vector<remotebsp::toolbusd::MotionGroupDispatch>
                    dispatches;
                remotebsp::toolbusd::MotionGroupServiceSnapshot snapshot;
                std::optional<
                    remotebsp::toolbusd::MotionGroupDispatchGate::Guard>
                    motion_guard;
                if (ipc_request.kind == remotebsp::toolbusd::
                                            IpcRequestKind::
                                                MotionGroupCancel) {
                    motion_guard.emplace(motion_dispatch_gate_.lock());
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    snapshot = motion_group_.snapshot();
                    if (!motion_group_identity_matches(
                            ipc_request, snapshot)) {
                        throw std::runtime_error(
                            "运动组事务身份与当前记录不匹配");
                    }
                    const auto state = motion_group_.state();
                    if (ipc_request.kind == remotebsp::toolbusd::
                                                IpcRequestKind::
                                                    MotionGroupCancel &&
                        (state == remotebsp::toolbusd::
                                      MotionGroupState::Preparing ||
                         state == remotebsp::toolbusd::
                                      MotionGroupState::Ready ||
                         state == remotebsp::toolbusd::
                                      MotionGroupState::Committing)) {
                        dispatches = motion_group_.cancel(requests_)
                                         .dispatches;
                        snapshot = motion_group_.snapshot();
                    }
                }
                if (!dispatches.empty()) {
                    static_cast<void>(send_motion_dispatches(dispatches));
                }
                if (motion_guard.has_value()) {
                    motion_guard->unlock();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_motion_group_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::NextEvent) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online ||
                    !node->assigned) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                const bool available = state_changed_.wait_for(
                    lock, std::chrono::milliseconds(1000), [&] {
                        const auto found =
                            event_queues_.find(ipc_request.node_id);
                        const auto* current =
                            nodes_.find_by_node_id(ipc_request.node_id);
                        return !running_ ||
                               (found != event_queues_.end() &&
                                !found->second.empty()) ||
                               current == nullptr || !current->online;
                    });
                auto found = event_queues_.find(ipc_request.node_id);
                if (available && found != event_queues_.end() &&
                    !found->second.empty()) {
                    auto event = std::move(found->second.front());
                    found->second.pop_front();
                    const auto encoded =
                        remotebsp::protocol::encode(std::move(event));
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Ok,
                        encoded);
                    return;
                }
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::TimedOut, {});
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::UartStreamRead) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online ||
                    !node->assigned) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                const auto key = uart_stream_key(
                    ipc_request.node_id, ipc_request.object_id);
                const bool available = state_changed_.wait_for(
                    lock,
                    std::chrono::milliseconds(ipc_request.timeout_ms),
                    [&] {
                        const auto found =
                            uart_stream_buffers_.find(key);
                        const auto* current =
                            nodes_.find_by_node_id(ipc_request.node_id);
                        return !running_ ||
                               (found != uart_stream_buffers_.end() &&
                                !found->second.bytes.empty()) ||
                               current == nullptr || !current->online;
                    });
                auto found = uart_stream_buffers_.find(key);
                if (!available || found == uart_stream_buffers_.end() ||
                    found->second.bytes.empty()) {
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::TimedOut,
                        {});
                    return;
                }
                remotebsp::toolbusd::UartStreamChunk chunk;
                chunk.dropped_bytes = found->second.dropped_bytes;
                chunk.lost_events = found->second.lost_events;
                const auto count = std::min<std::size_t>(
                    ipc_request.maximum_length,
                    found->second.bytes.size());
                chunk.data.reserve(count);
                for (std::size_t index = 0; index < count; ++index) {
                    chunk.data.push_back(found->second.bytes.front());
                    found->second.bytes.pop_front();
                }
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_uart_stream_chunk(
                        chunk));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::StreamRead) {
                if (remotebsp::transport::is_can_link(transport_->kind())) {
                    throw std::runtime_error(
                        "STREAM 数据面只允许显式 USB 链路，禁止自动降级到 CAN");
                }
                const auto status_body = request_snapshot_resource(
                    ipc_request.node_id,
                    remotebsp::protocol::Command::StreamStatus,
                    remotebsp::protocol::encode_resource_id(
                        ipc_request.stream_id),
                    std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(2500));
                const auto stream_status =
                    remotebsp::protocol::decode_stream_status(status_body);
                if (stream_status.stream_id != ipc_request.stream_id ||
                    stream_status.state ==
                        remotebsp::protocol::StreamState::Stopped ||
                    stream_status.state ==
                        remotebsp::protocol::StreamState::Failed) {
                    throw std::runtime_error(
                        "STREAM 会话已失效或不属于当前节点代次");
                }
                std::unique_lock<std::mutex> lock(state_mutex_);
                const auto* node = nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online || !node->assigned) {
                    throw std::runtime_error("目标节点尚未发现或已经离线");
                }
                const bool available = state_changed_.wait_for(
                    lock, std::chrono::milliseconds(ipc_request.timeout_ms), [&] {
                        const auto found = event_queues_.find(ipc_request.node_id);
                        if (found != event_queues_.end() &&
                            std::any_of(found->second.begin(), found->second.end(),
                                [&](const auto& packet) {
                                    if (packet.header.command != static_cast<std::uint16_t>(
                                            remotebsp::protocol::Command::StreamData)) return false;
                                    try { return remotebsp::protocol::decode_stream_data(packet.payload)
                                                     .stream_id == ipc_request.stream_id; }
                                    catch (const std::exception&) { return false; }
                                })) return true;
                        const auto* current = nodes_.find_by_node_id(ipc_request.node_id);
                        return !running_ || current == nullptr || !current->online;
                    });
                auto found = event_queues_.find(ipc_request.node_id);
                if (!available || found == event_queues_.end()) {
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::TimedOut, {});
                    return;
                }
                const auto event = std::find_if(found->second.begin(), found->second.end(),
                    [&](const auto& packet) {
                        if (packet.header.command != static_cast<std::uint16_t>(
                                remotebsp::protocol::Command::StreamData)) return false;
                        try { return remotebsp::protocol::decode_stream_data(packet.payload)
                                         .stream_id == ipc_request.stream_id; }
                        catch (const std::exception&) { return false; }
                    });
                if (event == found->second.end()) {
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::TimedOut, {});
                    return;
                }
                const auto data = remotebsp::protocol::decode_stream_data(event->payload);
                if (data.sequence != ipc_request.expected_sequence) {
                    throw std::runtime_error("STREAM 序号不连续，拒绝消费和归还信用");
                }
                const auto encoded = event->payload;
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok, encoded);
                return;
            }
            auto request = std::move(ipc_request.packet);
            if (request.header.message_type !=
                remotebsp::protocol::MessageType::Request) {
                throw std::invalid_argument("本地客户端只能提交请求消息");
            }
            const auto command = static_cast<remotebsp::protocol::Command>(
                request.header.command);
            const bool stream_command =
                command == remotebsp::protocol::Command::StreamContract ||
                command == remotebsp::protocol::Command::StreamOpen ||
                command == remotebsp::protocol::Command::StreamData ||
                command == remotebsp::protocol::Command::StreamCredit ||
                command == remotebsp::protocol::Command::StreamStatus ||
                command == remotebsp::protocol::Command::StreamStop;
            if (stream_command &&
                remotebsp::transport::is_can_link(transport_->kind())) {
                throw std::invalid_argument(
                    "STREAM 控制面和数据面必须显式绑定 USB 链路，禁止自动降级到 CAN");
            }
            if (command ==
                    remotebsp::protocol::Command::MotionGroupPrepare ||
                command ==
                    remotebsp::protocol::Command::MotionGroupCommit ||
                command ==
                    remotebsp::protocol::Command::MotionGroupAbort) {
                throw std::invalid_argument(
                    "跨板运动组命令必须通过事务 IPC 提交，不能直通节点");
            }
            // 会话由守护进程拥有，避免客户端伪造会话，也避免 toolbusd
            // 重启后请求 ID 从头计数时与 MCU 中的旧去重缓存冲突。
            request.header.session_id = session_id_;

            if (command == remotebsp::protocol::Command::StreamCredit) {
                const auto credit =
                    remotebsp::protocol::decode_stream_credit(request.payload);
                const auto credit_key = std::to_string(ipc_request.node_id) +
                    ":" + std::to_string(credit.stream_id) + ":" +
                    std::to_string(credit.acknowledged_sequence);
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (stream_credit_history_.find(credit_key) !=
                    stream_credit_history_.end()) {
                    remotebsp::protocol::Packet response;
                    response.header.message_type =
                        remotebsp::protocol::MessageType::Response;
                    response.header.command = request.header.command;
                    response.header.session_id = session_id_;
                    response.header.request_id = request.header.request_id;
                    response.payload = {0U};
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Ok,
                        remotebsp::protocol::encode(std::move(response)));
                    return;
                }
            }

            std::optional<remotebsp::toolbusd::BusRuntime::Reservation>
                bus_reservation;
            std::optional<std::uint64_t> bus_node_generation;
            std::optional<std::uint32_t> bus_device_resource_id;
            std::optional<std::uint32_t> bus_reset_resource_id;
            if (command == remotebsp::protocol::Command::ResourceReset &&
                request.payload.size() == 4U) {
                const auto resource_id =
                    remotebsp::protocol::decode_resource_id(request.payload);
                const auto type = static_cast<std::uint8_t>(resource_id >> 24U);
                if (type == static_cast<std::uint8_t>(
                                remotebsp::protocol::ResourceType::I2cDevice) ||
                    type == static_cast<std::uint8_t>(
                                remotebsp::protocol::ResourceType::SpiDevice)) {
                    bus_reset_resource_id = resource_id;
                }
            }
            if (command == remotebsp::protocol::Command::I2cContract ||
                command == remotebsp::protocol::Command::SpiContract) {
                if (request.header.object_id != 0U) {
                    throw std::invalid_argument(
                        "总线设备合同查询的对象 ID 必须为零");
                }
                const auto resource_id =
                    remotebsp::protocol::decode_resource_id(request.payload);
                const auto expected_type =
                    command == remotebsp::protocol::Command::I2cContract
                        ? remotebsp::protocol::ResourceType::I2cDevice
                        : remotebsp::protocol::ResourceType::SpiDevice;
                if (static_cast<std::uint8_t>(resource_id >> 24U) !=
                    static_cast<std::uint8_t>(expected_type)) {
                    throw std::invalid_argument(
                        "合同查询的资源 ID 命名空间与命令不匹配");
                }
            }
            if (command == remotebsp::protocol::Command::I2cTransfer) {
                if (request.header.object_id != 0U) {
                    throw std::invalid_argument(
                        "I2C 原子事务的对象 ID 必须为零");
                }
                const auto transfer =
                    remotebsp::protocol::decode_i2c_transfer_request(
                        request.payload);
                bus_device_resource_id = transfer.device_resource_id;
                bus_node_generation = ensure_bus_contract(
                    ipc_request.node_id, command,
                    transfer.device_resource_id);
                if (!bus_node_generation.has_value()) {
                    write_local_bus_result(
                        client, request,
                        remotebsp::protocol::BusTransactionStatus::Busy);
                    return;
                }
                auto admission = bus_runtime_.admit_i2c(
                    ipc_request.node_id, transfer);
                if (admission.status !=
                    remotebsp::toolbusd::BusAdmissionStatus::Accepted) {
                    if (admission.status == remotebsp::toolbusd::
                                                BusAdmissionStatus::
                                                    ContractMissing) {
                        throw std::runtime_error(
                            "I2C 合同已因节点状态变化失效，请重试");
                    }
                    write_local_bus_result(
                        client, request,
                        admission.status == remotebsp::toolbusd::
                                                BusAdmissionStatus::
                                                    ResourceBusy
                            ? remotebsp::protocol::BusTransactionStatus::Busy
                            : remotebsp::protocol::BusTransactionStatus::
                                  LimitExceeded);
                    return;
                }
                bus_reservation.emplace(
                    std::move(admission.reservation));
            } else if (command ==
                       remotebsp::protocol::Command::SpiTransfer) {
                if (request.header.object_id != 0U) {
                    throw std::invalid_argument(
                        "SPI 原子事务的对象 ID 必须为零");
                }
                const auto transfer =
                    remotebsp::protocol::decode_spi_transfer_request(
                        request.payload);
                bus_device_resource_id = transfer.device_resource_id;
                bus_node_generation = ensure_bus_contract(
                    ipc_request.node_id, command,
                    transfer.device_resource_id);
                if (!bus_node_generation.has_value()) {
                    write_local_bus_result(
                        client, request,
                        remotebsp::protocol::BusTransactionStatus::Busy);
                    return;
                }
                auto admission = bus_runtime_.admit_spi(
                    ipc_request.node_id, transfer);
                if (admission.status !=
                    remotebsp::toolbusd::BusAdmissionStatus::Accepted) {
                    if (admission.status == remotebsp::toolbusd::
                                                BusAdmissionStatus::
                                                    ContractMissing) {
                        throw std::runtime_error(
                            "SPI 合同已因节点状态变化失效，请重试");
                    }
                    write_local_bus_result(
                        client, request,
                        admission.status == remotebsp::toolbusd::
                                                BusAdmissionStatus::
                                                    ResourceBusy
                            ? remotebsp::protocol::BusTransactionStatus::Busy
                            : remotebsp::protocol::BusTransactionStatus::
                                  LimitExceeded);
                    return;
                }
                bus_reservation.emplace(
                    std::move(admission.reservation));
            } else if (command ==
                       remotebsp::protocol::Command::StreamOpen) {
                if (request.header.object_id != 0U) {
                    throw std::invalid_argument(
                        "STREAM 打开请求的对象 ID 必须为零");
                }
                const auto open =
                    remotebsp::protocol::decode_stream_open_request(
                        request.payload);
                bus_node_generation = ensure_stream_contract(
                    ipc_request.node_id, open.resource_id);
            }

            remotebsp::toolbusd::Submission submission;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (bus_reset_resource_id.has_value() &&
                    ipc_request.node_id <= kMaximumNodeId) {
                    bus_node_generation =
                        bus_node_generations_[ipc_request.node_id];
                }
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (ipc_request.node_id > kMaximumNodeId ||
                    node == nullptr || !node->online ||
                    !node->assigned ||
                    (bus_node_generation.has_value() &&
                     (bus_node_generations_[ipc_request.node_id] !=
                          *bus_node_generation))) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                if (!bus_node_generation.has_value() &&
                    (command ==
                         remotebsp::protocol::Command::I2cContract ||
                     command ==
                         remotebsp::protocol::Command::SpiContract)) {
                    bus_node_generation =
                        bus_node_generations_[ipc_request.node_id];
                }
                submission = requests_.submit(std::move(request));
                request_routes_[request_key(
                    submission.packet.header.session_id,
                    submission.request_id)] =
                    kNodeRequestBaseRoute + ipc_request.node_id;
            }
            const auto key = request_key(
                submission.packet.header.session_id, submission.request_id);
            const auto traffic_class =
                remotebsp::toolbusd::classify_traffic(
                    submission.packet);
            const auto policy =
                traffic_class ==
                        remotebsp::toolbusd::TrafficClass::Safety
                    ? remotebsp::toolbusd::AdmissionPolicy::Guaranteed
                    : remotebsp::toolbusd::AdmissionPolicy::Enforce;
            if (!send_packet(
                    submission.packet,
                    kNodeRequestBaseRoute + ipc_request.node_id,
                    policy)) {
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);
                    requests_.cancel(
                        submission.packet.header.session_id,
                        submission.request_id);
                    request_routes_.erase(key);
                }
                throw std::runtime_error(
                    "CAN 带宽准入拒绝：当前业务类别预算不足");
            }

            std::unique_lock<std::mutex> lock(state_mutex_);
            state_changed_.wait(lock, [&] {
                return !running_ || responses_.find(key) != responses_.end() ||
                       timed_out_.find(key) != timed_out_.end();
            });
            const auto response = responses_.find(key);
            if (response != responses_.end()) {
                if (command == remotebsp::protocol::Command::StreamCredit &&
                    !response->second.payload.empty() &&
                    response->second.payload.front() == 0U &&
                    (response->second.header.flags &
                     remotebsp::protocol::kErrorResponseFlag) == 0U) {
                    const auto credit =
                        remotebsp::protocol::decode_stream_credit(
                            submission.packet.payload);
                    const auto credit_key =
                        std::to_string(ipc_request.node_id) + ":" +
                        std::to_string(credit.stream_id) + ":" +
                        std::to_string(credit.acknowledged_sequence);
                    if (stream_credit_history_.size() >= 512U) {
                        stream_credit_history_.clear();
                    }
                    stream_credit_history_.insert(credit_key);
                    auto queued = event_queues_.find(ipc_request.node_id);
                    if (queued != event_queues_.end()) {
                        const auto event = std::find_if(
                            queued->second.begin(), queued->second.end(),
                            [&](const auto& packet) {
                                if (packet.header.command !=
                                    static_cast<std::uint16_t>(
                                        remotebsp::protocol::Command::
                                            StreamData)) return false;
                                try {
                                    const auto data =
                                        remotebsp::protocol::decode_stream_data(
                                            packet.payload);
                                    return data.stream_id == credit.stream_id &&
                                           data.sequence ==
                                               credit.acknowledged_sequence;
                                } catch (const std::exception&) {
                                    return false;
                                }
                            });
                        if (event != queued->second.end()) {
                            queued->second.erase(event);
                        }
                    }
                }
                if (bus_node_generation.has_value() &&
                    (command ==
                         remotebsp::protocol::Command::I2cContract ||
                     command ==
                         remotebsp::protocol::Command::SpiContract)) {
                    remember_direct_bus_contract_locked(
                        ipc_request.node_id, *bus_node_generation,
                        submission.packet, response->second);
                }
                if (bus_device_resource_id.has_value() &&
                    bus_node_generation.has_value() &&
                    bus_node_generations_[ipc_request.node_id] ==
                        *bus_node_generation &&
                    (response->second.header.flags &
                     remotebsp::protocol::kErrorResponseFlag) == 0U) {
                    // 严格解码成功才归类。未知新状态由协议层拒绝，绝不猜测为 Fault。
                    try {
                        const auto result =
                            remotebsp::protocol::decode_bus_transfer_result(
                                response->second.payload);
                        bus_runtime_.observe_remote_result(
                            ipc_request.node_id, *bus_device_resource_id,
                            result.status);
                    } catch (const std::exception&) {
                        // 保持透明转发：旧主机仍可接收未知固件响应，但健康统计不归类。
                    }
                }
                if (bus_reset_resource_id.has_value() &&
                    bus_node_generation.has_value() &&
                    bus_node_generations_[ipc_request.node_id] ==
                        *bus_node_generation &&
                    (response->second.header.flags &
                     remotebsp::protocol::kErrorResponseFlag) == 0U &&
                    response->second.payload.size() == 1U &&
                    response->second.payload.front() == 0U) {
                    bus_runtime_.observe_confirmed_reset(
                        ipc_request.node_id, *bus_reset_resource_id);
                }
                const auto encoded =
                    remotebsp::protocol::encode(response->second);
                responses_.erase(response);
                request_routes_.erase(key);
#ifdef REMOTEBSP_TEST_HOOKS
                // 只在测试构建中模拟“远端已提交信用，但本地 IPC 响应丢失”。
                // 故障点必须位于历史登记和事件出队之后，才能覆盖真实的不确定提交边界。
                if (command == remotebsp::protocol::Command::StreamCredit &&
                    drop_stream_credit_ipc_response_ordinal_ != 0U &&
                    stream_credit_ipc_response_count_.fetch_add(1U) + 1U ==
                        drop_stream_credit_ipc_response_ordinal_) {
                    lock.unlock();
                    return;
                }
#endif
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok, encoded);
                return;
            }
            timed_out_.erase(key);
            lock.unlock();
            remotebsp::toolbusd::write_ipc_response(
                client, remotebsp::toolbusd::IpcStatus::TimedOut,
                text_body("远端请求超时"));
        } catch (const remotebsp::toolbusd::IpcException& error) {
            if (std::string(error.what()) == "本地 IPC 消息超过最大长度") {
                std::lock_guard<std::mutex> lock(client_mutex_);
                increment_saturating(ipc_oversized_frame_total_);
            }
            try {
                const auto error_kind = error.has_request_kind()
                    ? std::optional<remotebsp::toolbusd::IpcRequestKind>(
                          error.request_kind())
                    : structured_error_kind;
                if (error_kind.has_value() &&
                    uses_structured_error(*error_kind)) {
                    const remotebsp::toolbusd::IpcErrorEnvelope envelope{
                        remotebsp::toolbusd::kIpcErrorEnvelopeVersion,
                        remotebsp::toolbusd::IpcErrorCode::InvalidRequest,
                        remotebsp::toolbusd::IpcErrorCategory::Request,
                        false, false, "本地 IPC 请求版本、长度或字段无效"};
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        remotebsp::toolbusd::encode_ipc_error_envelope(
                            envelope));
                } else {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        text_body(error.what()));
                }
            } catch (...) {
            }
        } catch (const remotebsp::toolbusd::RuntimeControlException& error) {
            try {
                if (structured_error_kind.has_value()) {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        remotebsp::toolbusd::encode_ipc_error_envelope(
                            gate_error_envelope(
                                error, *structured_error_kind)));
                } else {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        text_body(error.what()));
                }
            } catch (...) {
            }
        } catch (const remotebsp::toolbusd::OperationLedgerException& error) {
            try {
                if (structured_error_kind.has_value()) {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        remotebsp::toolbusd::encode_ipc_error_envelope(
                            ledger_error_envelope(error)));
                } else {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        text_body(error.what()));
                }
            } catch (...) {
            }
        } catch (const std::exception& error) {
            if (const auto* system_error =
                    dynamic_cast<const std::system_error*>(&error);
                system_error != nullptr &&
                (system_error->code().value() == EAGAIN ||
                 system_error->code().value() == EWOULDBLOCK)) {
                std::lock_guard<std::mutex> lock(client_mutex_);
                increment_saturating(ipc_timeout_total_);
            }
            try {
                if (structured_error_kind.has_value()) {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        remotebsp::toolbusd::encode_ipc_error_envelope(
                            generic_error_envelope(
                                *structured_error_kind, error)));
                } else {
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Error,
                        text_body(error.what()));
                }
            } catch (...) {
            }
        }
    }

    void open_server() {
        if (socket_path_.empty() ||
            socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::invalid_argument("本地套接字路径无效或过长");
        }
        struct stat existing {};
        if (::lstat(socket_path_.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) {
                throw std::runtime_error(
                    "本地套接字路径已存在且不是套接字");
            }
            if (existing.st_uid != ::geteuid()) {
                throw std::runtime_error(
                    "本地套接字路径属于其他用户，拒绝删除");
            }
            if (::unlink(socket_path_.c_str()) < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "清理旧本地套接字失败");
            }
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(),
                                    "检查本地套接字路径失败");
        }

        server_socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "创建本地 IPC 套接字失败");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(),
                    socket_path_.size() + 1U);
        if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "绑定本地 IPC 套接字失败");
        }
        struct stat created {};
        const int created_stat = ::lstat(socket_path_.c_str(), &created);
        if (created_stat < 0 ||
            !S_ISSOCK(created.st_mode) ||
            created.st_uid != ::geteuid()) {
            const int saved_errno = created_stat < 0 ? errno : EINVAL;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "确认本地 IPC 套接字身份失败");
        }
        socket_device_ = created.st_dev;
        socket_inode_ = created.st_ino;
        socket_identity_known_ = true;
        // 不依赖服务进程的 umask。套接字在开始监听前固定为 0660，
        // 由部署时的所有者/组决定哪些本地进程能够发出硬件命令。
        if (::chmod(socket_path_.c_str(),
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "设置本地 IPC 套接字权限失败");
        }
        if (::listen(server_socket_, 8) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "监听本地 IPC 套接字失败");
        }
    }

    void stop() {
        if (running_) {
            const auto failures = runtime_control_.shutdown();
            if (failures != 0U) {
                std::cerr << "Runtime GPIO 会话结束时有 " << failures
                          << " 个资源未能完成安全写低或 GPIO_CLOSE；"
                             "资源保持故障占位\n";
            }
        }
        if (running_.exchange(false)) {
            state_changed_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        std::unique_lock<std::mutex> lock(client_mutex_);
        clients_finished_.wait(lock,
                               [this] { return active_clients_ == 0; });
        lock.unlock();
        if (recording_ && recording_->status().active) {
            try {
                std::cout << "逻辑链路证据已原子保存: "
                          << recording_->stop() << '\n';
            } catch (const std::exception& error) {
                std::cerr << "逻辑链路录制收口失败: " << error.what() << '\n';
            }
        }
    }

    void close_server() noexcept {
        if (server_socket_ >= 0) {
            ::close(server_socket_);
            server_socket_ = -1;
        }
        if (!socket_path_.empty()) {
            struct stat existing {};
            if (::lstat(socket_path_.c_str(), &existing) == 0 &&
                S_ISSOCK(existing.st_mode) && socket_identity_known_ &&
                existing.st_dev == socket_device_ &&
                existing.st_ino == socket_inode_) {
                ::unlink(socket_path_.c_str());
            }
        }
        socket_identity_known_ = false;
        socket_device_ = 0;
        socket_inode_ = 0;
    }

    std::unique_ptr<remotebsp::transport::LinkTransport> transport_;
    remotebsp::protocol::Fragmenter fragmenter_;
    remotebsp::protocol::Reassembler reassembler_;
    remotebsp::toolbusd::TrafficController traffic_;
    remotebsp::toolbusd::RequestManager requests_;
    remotebsp::toolbusd::ClockSyncManager clock_sync_;
    remotebsp::toolbusd::MotionGroupService motion_group_;
    remotebsp::toolbusd::OperationLedger operation_ledger_;
    std::unique_ptr<remotebsp::toolbusd::LinkRecordingController> recording_;
    const std::size_t maximum_ipc_clients_;
#ifdef REMOTEBSP_TEST_HOOKS
    std::unique_ptr<OneShotTestBarrier> gpio_post_lookup_barrier_;
    const std::uint32_t drop_stream_credit_ipc_response_ordinal_{};
    const std::uint32_t drop_pwm_acquire_response_ordinal_{};
    const std::uint32_t test_pwm_remote_lease_ttl_ms_{};
    const std::uint32_t drop_bus_reset_response_ordinal_{};
    std::atomic<std::uint32_t> stream_credit_ipc_response_count_{0U};
    std::atomic<std::uint32_t> pwm_acquire_response_count_{0U};
    std::atomic<std::uint32_t> bus_reset_response_count_{0U};
#endif
    remotebsp::toolbusd::NodeRegistry nodes_;
    remotebsp::toolbusd::BusRuntime bus_runtime_;
    std::array<std::uint64_t, kMaximumNodeId + 1U>
        bus_node_generations_{};
    std::string socket_path_;
    int server_socket_{-1};
    dev_t socket_device_{};
    ino_t socket_inode_{};
    bool socket_identity_known_{};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex client_mutex_;
    std::condition_variable clients_finished_;
    std::size_t active_clients_{0};
    std::size_t ipc_peak_clients_{0};
    std::uint64_t ipc_accepted_total_{0};
    std::uint64_t ipc_capacity_rejected_total_{0};
    std::uint64_t ipc_oversized_frame_total_{0};
    std::uint64_t ipc_timeout_total_{0};
    std::uint64_t ipc_thread_creation_failed_total_{0};
    std::mutex send_mutex_;
    remotebsp::toolbusd::MotionGroupDispatchGate motion_dispatch_gate_;
    remotebsp::toolbusd::RuntimeControlGate runtime_control_;
    // 只串行化账本可用性/阻断检查与持久写边界，不得覆盖远端 I/O。
    std::mutex runtime_operation_policy_mutex_;
    std::unordered_map<std::uint64_t,
        std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>
        pwm_remote_lease_quarantine_;
    std::timed_mutex runtime_snapshot_mutex_;
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::unordered_map<std::uint64_t, remotebsp::protocol::Packet> responses_;
    std::unordered_set<std::uint64_t> timed_out_;
    std::unordered_map<std::uint64_t, std::uint32_t> request_routes_;
    std::unordered_map<
        std::uint32_t, std::deque<remotebsp::protocol::Packet>>
        event_queues_;
    // 已由远端确认的信用归还保持有界幂等窗口；用于客户端在 IPC
    // 响应丢失后重试，避免重复向 MCU 增发信用。
    std::unordered_set<std::string> stream_credit_history_;
    std::unordered_map<std::uint64_t, UartStreamBuffer>
        uart_stream_buffers_;
    std::atomic<std::uint16_t> next_transfer_id_{1};
    std::uint32_t next_control_request_id_{0x80000000U};
    std::uint32_t next_node_id_{1};
    std::uint64_t next_runtime_snapshot_sequence_{1U};
    const std::uint32_t session_id_{make_session_id()};
    const std::array<std::uint8_t, 16> daemon_instance_id_{
        make_daemon_instance_id(session_id_)};
    const std::uint64_t health_started_at_ms_{steady_time_ms()};
    const std::uint64_t health_generation_{make_health_generation()};
    remotebsp::toolbusd::ToolbusdHealthProducer health_producer_{
        health_generation_, health_started_at_ms_};
};

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法: toolbusd <链路端点> <classical|fd|usb|usb-mock> "
                     "[Unix套接字路径] "
                     "[--arbitration-bitrate bit/s] "
                     "[--data-bitrate bit/s] "
                     "[--max-utilization-permille 1..1000] "
                     "[--burst-window-ms 毫秒] "
                     "[--max-ipc-clients 1..1024] "
                     "[--motion-max-clock-error-ns 纳秒] "
                     "[--logical-recording-dir 固定目录 "
                     "--record-logical-link 文件名.rbsplog] "
                     "--runtime-operation-ledger-dir 账本目录"
#ifdef REMOTEBSP_TEST_HOOKS
                     " [--test-operation-ledger-fail-terminal-sync 序号]"
                     " [--test-runtime-gpio-post-lookup-barrier 参与数]"
                     " [--test-stream-credit-drop-ipc-response 序号]"
                     " [--test-pwm-acquire-drop-response 序号]"
                     " [--test-pwm-remote-lease-ttl-ms 毫秒]"
                     " [--test-bus-reset-drop-response 序号]"
#endif
                     "\n";
        return 2;
    }
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        const std::string mode_text = argv[2];
        const bool mock_usb = mode_text == "usb-mock";
        const bool usb = mode_text == "usb";
        const auto mode = (mock_usb || usb)
                              ? remotebsp::transport::CanMode::Classical
                              : parse_mode(mode_text);
        std::string socket_path = "/tmp/toolbusd.sock";
        remotebsp::toolbusd::TrafficConfig traffic_config;
        remotebsp::toolbusd::ClockSyncManagerConfig clock_sync_config;
        remotebsp::toolbusd::MotionGroupServiceConfig motion_group_config;
        std::optional<std::string> operation_ledger_directory;
        std::optional<std::string> logical_recording_directory;
        std::optional<std::string> logical_recording_name;
        ToolbusDaemonTestOptions test_options;
        std::uint32_t maximum_ipc_clients =
            remotebsp::toolbusd::kDefaultMaximumConcurrentIpcClients;
        traffic_config.mode = (mock_usb || usb)
                                  ? remotebsp::toolbusd::TrafficBusMode::Usb
                                  : mode == remotebsp::transport::CanMode::Classical
                                        ? remotebsp::toolbusd::TrafficBusMode::Classical
                                        : remotebsp::toolbusd::TrafficBusMode::CanFd;
        traffic_config.arbitration_bits_per_second =
            (mock_usb || usb) ? 12000000U
            : mode == remotebsp::transport::CanMode::Classical
                ? 1000000U
                : 500000U;
        traffic_config.data_bits_per_second =
            (mock_usb || usb) ? 12000000U
            : mode == remotebsp::transport::CanMode::Classical
                ? 1000000U
                : 2000000U;
        int index = 3;
        if (index < argc &&
            std::string(argv[index]).rfind("--", 0) != 0) {
            socket_path = argv[index++];
        }
        while (index < argc) {
            const std::string option = argv[index++];
            if (index >= argc) {
                throw std::invalid_argument(
                    "toolbusd 选项缺少参数");
            }
            if (option == "--runtime-operation-ledger-dir") {
                const std::string directory = argv[index++];
                if (directory.empty() ||
                    operation_ledger_directory.has_value()) {
                    throw std::invalid_argument(
                        "Runtime 操作账本目录必须显式且只能指定一次");
                }
                operation_ledger_directory = directory;
                continue;
            }
            if (option == "--logical-recording-dir") {
                if (logical_recording_directory.has_value()) {
                    throw std::invalid_argument("逻辑链路录制目录只能指定一次");
                }
                logical_recording_directory = argv[index++];
                continue;
            }
            if (option == "--record-logical-link") {
                if (logical_recording_name.has_value()) {
                    throw std::invalid_argument("逻辑链路录制文件只能指定一次");
                }
                logical_recording_name = argv[index++];
                continue;
            }
            const std::uint32_t value =
                parse_positive_u32(argv[index++], option.c_str());
            if (option == "--arbitration-bitrate") {
                traffic_config.arbitration_bits_per_second = value;
            } else if (option == "--data-bitrate") {
                traffic_config.data_bits_per_second = value;
            } else if (option == "--max-utilization-permille") {
                if (value > 1000U) {
                    throw std::invalid_argument(
                        "最大总线利用率必须位于 1～1000 千分比");
                }
                traffic_config.maximum_utilization_permille =
                    static_cast<std::uint16_t>(value);
            } else if (option == "--burst-window-ms") {
                if (value > 60000U) {
                    throw std::invalid_argument(
                        "突发窗口必须位于 1～60000 ms");
                }
                traffic_config.burst_window =
                    std::chrono::milliseconds(value);
            } else if (option == "--max-ipc-clients") {
                if (value > remotebsp::toolbusd::
                                kMaximumConfigurableConcurrentIpcClients) {
                    throw std::invalid_argument(
                        "IPC 并发客户端上限必须位于 1～1024");
                }
                maximum_ipc_clients = value;
            } else if (option == "--motion-max-clock-error-ns") {
                if (value > 10000000U) {
                    throw std::invalid_argument(
                        "跨板运动最大时钟误差必须位于 1～10000000 ns");
                }
                motion_group_config.coordinator.maximum_clock_error_ns =
                    value;
                // 协调器不能使用已被 ClockModel 标成 degraded 的模型。
                // 因此同一准入上限同时约束模型状态和事务冻结，避免把
                // 高于模型默认 250 us 的配置变成表面可配、实际无效。
                clock_sync_config.clock_model_config.maximum_error_bound_ns =
                    value;
#ifdef REMOTEBSP_TEST_HOOKS
            } else if (option ==
                       "--test-operation-ledger-fail-terminal-sync") {
                if (value > 1024U ||
                    test_options.fail_terminal_record_sync_ordinal != 0U) {
                    throw std::invalid_argument(
                        "terminal 账本测试故障序号必须为 1～1024 且只能指定一次");
                }
                test_options.fail_terminal_record_sync_ordinal = value;
            } else if (option ==
                       "--test-runtime-gpio-post-lookup-barrier") {
                if (value < 2U || value > 64U ||
                    test_options.gpio_post_lookup_barrier_participants != 0U) {
                    throw std::invalid_argument(
                        "Runtime GPIO 测试屏障参与数必须为 2～64 且只能指定一次");
                }
                test_options.gpio_post_lookup_barrier_participants = value;
            } else if (option ==
                       "--test-stream-credit-drop-ipc-response") {
                if (value > 1024U ||
                    test_options.drop_stream_credit_ipc_response_ordinal !=
                        0U) {
                    throw std::invalid_argument(
                        "STREAM 信用 IPC 响应丢失测试序号必须为 1～1024 且只能指定一次");
                }
                test_options.drop_stream_credit_ipc_response_ordinal = value;
            } else if (option == "--test-pwm-acquire-drop-response") {
                if (value > 1024U ||
                    test_options.drop_pwm_acquire_response_ordinal != 0U)
                    throw std::invalid_argument("PWM acquire响应丢失测试序号无效");
                test_options.drop_pwm_acquire_response_ordinal = value;
            } else if (option == "--test-pwm-remote-lease-ttl-ms") {
                if (value < 100U || value > 5000U ||
                    test_options.pwm_remote_lease_ttl_ms != 0U)
                    throw std::invalid_argument("PWM 测试远端租约TTL必须为100～5000ms");
                test_options.pwm_remote_lease_ttl_ms = value;
            } else if (option == "--test-bus-reset-drop-response") {
                if (value > 1024U ||
                    test_options.drop_bus_reset_response_ordinal != 0U)
                    throw std::invalid_argument("总线复位响应丢失测试序号无效");
                test_options.drop_bus_reset_response_ordinal = value;
#endif
            } else {
                throw std::invalid_argument("未知 toolbusd 选项");
            }
        }
        if (!operation_ledger_directory.has_value()) {
            throw std::invalid_argument(
                "必须显式指定 --runtime-operation-ledger-dir");
        }
        if (logical_recording_name.has_value() &&
            !logical_recording_directory.has_value()) {
            throw std::invalid_argument(
                "逻辑链路录制文件名要求先配置固定目录");
        }
        if (!mock_usb && !usb &&
            mode == remotebsp::transport::CanMode::Classical) {
            traffic_config.data_bits_per_second =
                traffic_config.arbitration_bits_per_second;
        }
        std::unique_ptr<remotebsp::transport::LinkTransport> transport;
        if (mock_usb) {
            transport =
                std::make_unique<remotebsp::transport::MockUsbTransport>(
                    argv[1], remotebsp::transport::MockUsbRole::Host);
        } else if (usb) {
            transport =
                std::make_unique<remotebsp::transport::LibusbTransport>(
                    remotebsp::transport::parse_usb_device_selector(
                        argv[1]));
        } else {
            transport =
                std::make_unique<remotebsp::transport::SocketCanTransport>(
                    argv[1], mode);
        }
        std::unique_ptr<remotebsp::toolbusd::LinkRecordingController>
            recording;
        if (logical_recording_directory.has_value()) {
            recording = std::make_unique<
                remotebsp::toolbusd::LinkRecordingController>(
                    *logical_recording_directory);
            transport = recording->wrap(std::move(transport));
            if (logical_recording_name.has_value()) {
                recording->start(*logical_recording_name);
            }
        }
        ToolbusDaemon daemon(std::move(transport), std::move(socket_path),
                             traffic_config, clock_sync_config,
                             motion_group_config,
                             std::move(*operation_ledger_directory),
                             maximum_ipc_clients,
                             test_options, std::move(recording));
        daemon.run();
    } catch (const std::exception& error) {
        std::cerr << "toolbusd 启动失败: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
