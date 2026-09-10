#pragma once

#include "remotebsp/protocol/health.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"
#include "remotebsp/toolbusd/clock_model.hpp"
#include "remotebsp/toolbusd/motion_group_service.hpp"
#include "remotebsp/toolbusd/runtime_control.hpp"

#include <cstdint>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace remotebsp::toolbusd {

enum class IpcStatus : std::uint8_t {
    Ok = 0,
    TimedOut = 1,
    Error = 2,
};

enum class IpcRequestKind : std::uint8_t {
    RemotePacket = 0,
    ListNodes = 1,
    NextEvent = 2,
    TrafficStatus = 3,
    UartStreamRead = 4,
    RuntimeSnapshot = 5,
    MotionGroupSubmit = 6,
    MotionGroupStatus = 7,
    MotionGroupCancel = 8,
    DaemonIdentity = 9,
    RuntimeControlAcquire = 10,
    RuntimeGpioWrite = 11,
    RuntimeControlRelease = 12,
    HealthSnapshot = 13,
    RuntimeGpioWriteOperation = 14,
    RuntimeControlReleaseOperation = 15,
    RuntimeOperationQuery = 16,
    RuntimeOperationLookup = 17,
    StreamRead = 18,
    LogicalRecordingStart = 19,
    LogicalRecordingStop = 20,
    LogicalRecordingStatus = 21,
};
constexpr std::uint16_t kLogicalRecordingIpcVersion = 1U;

constexpr std::uint16_t kDaemonIdentityIpcVersion = 1U;
constexpr std::uint16_t kHealthSnapshotIpcVersion = 1U;
constexpr std::uint16_t kRuntimeSnapshotIpcVersion = 2U;
constexpr std::uint16_t kMaximumRuntimeSnapshotResources = 128U;
constexpr std::uint32_t kMaximumRuntimeSnapshotTimeoutMs = 5000U;
constexpr std::uint16_t kMotionGroupIpcVersion = 1U;
constexpr std::uint16_t kMaximumIpcMotionGroupMembers = 32U;
constexpr std::uint16_t kIpcErrorEnvelopeVersion = 1U;
constexpr std::size_t kMaximumIpcErrorMessageBytes = 256U;
constexpr std::uint16_t kRuntimeOperationIpcVersion = 1U;
constexpr std::size_t kRuntimeOperationIdBytes = 32U;

enum class IpcErrorCategory : std::uint8_t {
    Request = 1U,
    Authentication = 2U,
    Authorization = 3U,
    Conflict = 4U,
    Unavailable = 5U,
    Timeout = 6U,
    Internal = 7U,
};

enum class IpcErrorCode : std::uint16_t {
    InvalidRequest = 1U,
    UnsupportedRequest = 2U,
    DaemonIdentityMismatch = 100U,
    PermissionDenied = 101U,
    LeaseConflict = 102U,
    LeaseNotFound = 103U,
    LeaseExpired = 104U,
    ContractRejected = 105U,
    CapacityExceeded = 106U,
    IdempotencyConflict = 107U,
    SafeStopFailed = 108U,
    ObjectRetired = 109U,
    NodeUnavailable = 200U,
    BackendUnavailable = 201U,
    DeadlineExceeded = 202U,
    HealthUnavailable = 203U,
    InternalFailure = 255U,
};

struct IpcErrorEnvelope {
    std::uint16_t version{kIpcErrorEnvelopeVersion};
    IpcErrorCode code{IpcErrorCode::InternalFailure};
    IpcErrorCategory category{IpcErrorCategory::Internal};
    bool retryable{};
    bool possibly_committed{};
    std::string message;
};

enum class RuntimeOperationKind : std::uint8_t {
    Unknown = 0U,
    GpioWrite = 1U,
    ControlRelease = 2U,
};

enum class RuntimeOperationState : std::uint8_t {
    Pending = 1U,
    Committed = 2U,
    Rejected = 3U,
    Unknown = 4U,
    ExpiredUnknown = 5U,
};

enum class RuntimeOperationRecovery : std::uint8_t {
    None = 0U,
    NotSent = 1U,
    SafeClosed = 2U,
    ScopeBlocked = 3U,
    AwaitingReboot = 4U,
    NodeRebootConfirmed = 5U,
};

enum class RuntimeOperationError : std::uint16_t {
    None = 0U,
    Rejected = 1U,
    Deadline = 2U,
    Backend = 3U,
    Persistence = 4U,
    HistoryExpired = 5U,
};

using RuntimeOperationId =
    std::array<std::uint8_t, kRuntimeOperationIdBytes>;

struct RuntimeOperationQuery {
    std::uint16_t version{kRuntimeOperationIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    RuntimeOperationId operation_id{};
    std::string owner_key_id;
};

struct RuntimeOperationLookup {
    std::uint16_t version{kRuntimeOperationIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    RuntimeOperationKind kind{RuntimeOperationKind::GpioWrite};
    std::array<std::uint8_t, 16> lease_id{};
    std::string owner_key_id;
    std::string idempotency_key;
};

struct RuntimeOperationOutcome {
    std::uint16_t version{kRuntimeOperationIpcVersion};
    RuntimeOperationKind kind{RuntimeOperationKind::GpioWrite};
    RuntimeOperationState state{RuntimeOperationState::Pending};
    RuntimeOperationRecovery recovery{RuntimeOperationRecovery::None};
    bool replayed{};
    RuntimeOperationId operation_id{};
    std::array<std::uint8_t, 16> lease_id{};
    std::array<std::uint8_t, 16> expected_node_uuid{};
    std::uint32_t resource_id{};
    std::uint32_t object_id{};
    RuntimeOperationError error{RuntimeOperationError::None};
    bool value{};
};

struct IpcResponse {
    IpcStatus status{IpcStatus::Error};
    std::vector<std::uint8_t> body;
};

struct IpcDaemonIdentity {
    std::uint16_t version{kDaemonIdentityIpcVersion};
    std::array<std::uint8_t, 16> instance_id{};
};

struct IpcToolbusdHealthSnapshot {
    std::uint16_t version{kHealthSnapshotIpcVersion};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    protocol::HealthSnapshot health;
};

struct IpcRequest {
    IpcRequestKind kind{IpcRequestKind::RemotePacket};
    std::uint32_t node_id{};
    protocol::Packet packet;
    std::uint32_t object_id{};
    std::uint32_t maximum_length{};
    std::uint32_t timeout_ms{};
    std::uint32_t stream_id{};
    std::uint32_t expected_sequence{};
    MotionGroupPlan motion_group_plan;
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    RuntimeControlAcquireRequest runtime_control_acquire;
    RuntimeGpioWriteRequest runtime_gpio_write;
    RuntimeControlReleaseRequest runtime_control_release;
    RuntimeOperationQuery runtime_operation_query;
    RuntimeOperationLookup runtime_operation_lookup;
    std::string logical_recording_name;
};

struct IpcLogicalRecordingStatus {
    std::uint16_t version{kLogicalRecordingIpcVersion};
    bool configured{};
    bool active{};
    std::string evidence_scope{"logical-link-boundary-only"};
    std::string output_name;
    std::uint64_t event_count{};
    std::uint64_t maximum_events{};
    std::uint64_t maximum_file_bytes{};
};

struct UartStreamChunk {
    std::vector<std::uint8_t> data;
    std::uint64_t dropped_bytes{};
    std::uint64_t lost_events{};
};

struct IpcNodeInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::uint32_t node_id{};
    bool online{};
    bool ready{};
    std::uint16_t firmware_major{};
    std::uint16_t firmware_minor{};
    std::uint16_t firmware_patch{};
    std::uint32_t board_type{};
    std::uint8_t protocol_version{};
};

struct IpcRuntimeResource {
    std::uint32_t node_id{};
    bool status_valid{};
    protocol::ResourceDescriptor descriptor;
    protocol::ResourceStatusPayload status;
};

enum class IpcRuntimeNodeError : std::uint8_t {
    ResourceInventoryUnavailable = 1U,
};

struct IpcRuntimeNodeIssue {
    std::uint32_t node_id{};
    IpcRuntimeNodeError error{IpcRuntimeNodeError::ResourceInventoryUnavailable};
};

struct IpcRuntimeClockQuality {
    std::uint32_t node_id{};
    bool registered{};
    bool estimate_valid{};
    ClockSyncState state{ClockSyncState::Unsynced};
    std::uint64_t boot_epoch{};
    std::uint64_t model_generation{};
    std::uint16_t sample_count{};
    std::uint16_t selected_sample_count{};
    std::uint32_t drift_uncertainty_ppm{};
    std::int32_t rate_deviation_ppb{};
    std::uint64_t minimum_network_rtt_ns{};
    std::uint64_t error_bound_ns{};
    std::uint64_t sample_age_ns{};
    std::uint64_t last_sample_host_time_ns{};
};

struct IpcRuntimeSnapshot {
    std::uint16_t version{kRuntimeSnapshotIpcVersion};
    std::uint64_t sequence{};
    std::vector<IpcNodeInfo> nodes;
    TrafficSnapshot traffic;
    std::vector<IpcRuntimeResource> resources;
    std::vector<IpcRuntimeNodeIssue> node_issues;
    std::vector<IpcRuntimeClockQuality> clocks;
};

class IpcException : public std::runtime_error {
public:
    explicit IpcException(const std::string& message);
    IpcException(const std::string& message, IpcRequestKind request_kind);
    bool has_request_kind() const noexcept;
    IpcRequestKind request_kind() const noexcept;

private:
    bool has_request_kind_{};
    IpcRequestKind request_kind_{IpcRequestKind::RemotePacket};
};

void write_ipc_request(int socket, const protocol::Packet& packet,
                       std::uint32_t node_id);
void write_ipc_node_list_request(int socket);
void write_ipc_next_event_request(int socket, std::uint32_t node_id);
void write_ipc_traffic_status_request(int socket);
void write_ipc_uart_stream_read_request(
    int socket, std::uint32_t node_id, std::uint32_t object_id,
    std::uint32_t maximum_length, std::uint32_t timeout_ms);
void write_ipc_stream_read_request(
    int socket, std::uint32_t node_id, std::uint32_t stream_id,
    std::uint32_t expected_sequence, std::uint32_t timeout_ms);
void write_ipc_runtime_snapshot_request(
    int socket, std::uint16_t maximum_resources,
    std::uint32_t timeout_ms);
void write_ipc_motion_group_submit_request(
    int socket, const MotionGroupPlan& plan);
void write_ipc_motion_group_status_request(
    int socket, std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation);
void write_ipc_motion_group_cancel_request(
    int socket, std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation);
void write_ipc_daemon_identity_request(int socket);
void write_ipc_health_snapshot_request(int socket);
void write_ipc_runtime_control_acquire_request(
    int socket, const RuntimeControlAcquireRequest& request);
void write_ipc_runtime_gpio_write_request(
    int socket, const RuntimeGpioWriteRequest& request);
void write_ipc_runtime_control_release_request(
    int socket, const RuntimeControlReleaseRequest& request);
void write_ipc_runtime_gpio_write_operation_request(
    int socket, const RuntimeGpioWriteRequest& request);
void write_ipc_runtime_control_release_operation_request(
    int socket, const RuntimeControlReleaseRequest& request);
void write_ipc_runtime_operation_query_request(
    int socket, const RuntimeOperationQuery& request);
void write_ipc_runtime_operation_lookup_request(
    int socket, const RuntimeOperationLookup& request);
void write_ipc_logical_recording_start_request(int socket,
                                               const std::string& output_name);
void write_ipc_logical_recording_stop_request(int socket);
void write_ipc_logical_recording_status_request(int socket);
IpcRequest read_ipc_request(int socket);

std::vector<std::uint8_t> encode_ipc_node_list(
    const std::vector<IpcNodeInfo>& nodes);
std::vector<IpcNodeInfo> decode_ipc_node_list(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_traffic_status(
    const TrafficSnapshot& snapshot);
TrafficSnapshot decode_ipc_traffic_status(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_uart_stream_chunk(
    const UartStreamChunk& chunk);
UartStreamChunk decode_ipc_uart_stream_chunk(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_snapshot(
    const IpcRuntimeSnapshot& snapshot);
IpcRuntimeSnapshot decode_ipc_runtime_snapshot(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_motion_group_plan(
    const MotionGroupPlan& plan);
MotionGroupPlan decode_ipc_motion_group_plan(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_motion_group_snapshot(
    const MotionGroupServiceSnapshot& snapshot);
MotionGroupServiceSnapshot decode_ipc_motion_group_snapshot(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_daemon_identity(
    const IpcDaemonIdentity& identity);
IpcDaemonIdentity decode_ipc_daemon_identity(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_health_snapshot(
    const IpcToolbusdHealthSnapshot& snapshot);
IpcToolbusdHealthSnapshot decode_ipc_health_snapshot(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_control_acquire(
    const RuntimeControlAcquireRequest& request);
RuntimeControlAcquireRequest decode_ipc_runtime_control_acquire(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_gpio_write(
    const RuntimeGpioWriteRequest& request);
RuntimeGpioWriteRequest decode_ipc_runtime_gpio_write(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_control_release(
    const RuntimeControlReleaseRequest& request);
RuntimeControlReleaseRequest decode_ipc_runtime_control_release(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_gpio_write_result(
    const RuntimeGpioWriteResult& result);
RuntimeGpioWriteResult decode_ipc_runtime_gpio_write_result(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_operation_query(
    const RuntimeOperationQuery& query);
RuntimeOperationQuery decode_ipc_runtime_operation_query(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_operation_lookup(
    const RuntimeOperationLookup& lookup);
RuntimeOperationLookup decode_ipc_runtime_operation_lookup(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_runtime_operation_outcome(
    const RuntimeOperationOutcome& outcome);
RuntimeOperationOutcome decode_ipc_runtime_operation_outcome(
    const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> encode_ipc_logical_recording_status(
    const IpcLogicalRecordingStatus& status);
IpcLogicalRecordingStatus decode_ipc_logical_recording_status(
    const std::vector<std::uint8_t>& body);
const char* runtime_operation_kind_name(RuntimeOperationKind kind) noexcept;
const char* runtime_operation_state_name(RuntimeOperationState state) noexcept;
const char* runtime_operation_recovery_name(
    RuntimeOperationRecovery recovery) noexcept;
const char* runtime_operation_error_name(RuntimeOperationError error) noexcept;
std::vector<std::uint8_t> encode_ipc_error_envelope(
    const IpcErrorEnvelope& error);
IpcErrorEnvelope decode_ipc_error_envelope(
    const std::vector<std::uint8_t>& body);
const char* ipc_error_code_name(IpcErrorCode code) noexcept;
const char* ipc_error_category_name(IpcErrorCategory category) noexcept;

void write_ipc_response(int socket, IpcStatus status,
                        const std::vector<std::uint8_t>& body);
IpcResponse read_ipc_response(int socket);

}
