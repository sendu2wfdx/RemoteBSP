#pragma once
#include "remotebsp/protocol/adc.hpp"
#include "remotebsp/protocol/storage.hpp"
#include "remotebsp/protocol/timer.hpp"

#include "remotebsp/protocol/device_parameters.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/gpio.hpp"
#include "remotebsp/protocol/health.hpp"
#include "remotebsp/protocol/firmware_identity.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/motion.hpp"
#include "remotebsp/protocol/motion_group.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/protocol/waveform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

namespace remotebsp {

struct NodeInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::uint16_t firmware_major{};
    std::uint16_t firmware_minor{};
    std::uint16_t firmware_patch{};
    std::uint32_t board_type{};
    std::uint8_t protocol_version{};
};

struct DiscoveredNode {
    NodeInfo identity;
    std::uint32_t node_id{};
    bool online{};
    bool ready{};
};

struct DaemonIdentity {
    std::uint16_t version{};
    std::array<std::uint8_t, 16> instance_id{};
};

struct ToolbusdHealthSnapshot {
    std::uint16_t ipc_version{};
    std::array<std::uint8_t, 16> daemon_instance_id{};
    protocol::HealthSnapshot health;
    std::uint64_t ipc_active_clients{}, ipc_maximum_clients{}, ipc_peak_clients{};
    std::uint64_t ipc_accepted_total{}, ipc_capacity_rejected_total{};
    std::uint64_t ipc_oversized_frame_total{}, ipc_timeout_total{};
    std::uint64_t ipc_thread_creation_failed_total{};
};

struct LogicalRecordingStatus {
    bool configured{};
    bool active{};
    std::string evidence_scope;
    std::string output_name;
    std::uint64_t event_count{};
    std::uint64_t maximum_events{};
    std::uint64_t maximum_file_bytes{};
};

struct RuntimeGpioWriteResult {
    std::uint32_t object_id{};
    bool value{};
    bool replayed{};
};

struct RuntimePwmResult {
    std::uint32_t object_id{};
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
    bool replayed{};
};

enum class RuntimeOperationKind : std::uint8_t {
    Unknown = 0U,
    GpioWrite = 1U,
    ControlRelease = 2U,
    PwmConfigure = 3U,
    PwmStop = 4U,
    TimedBitstreamConfigure = 5U,
    TimedBitstreamFrame = 6U,
    TimedBitstreamStop = 7U,
    BusResourceReset = 8U,
    MotionGroupCancel = 9U,
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

struct RuntimeOperationOutcome {
    std::array<std::uint8_t, 32> operation_id{};
    std::optional<std::array<std::uint8_t, 16>> lease_id;
    std::optional<std::array<std::uint8_t, 16>> expected_node_uuid;
    std::optional<std::uint32_t> resource_id;
    RuntimeOperationKind kind{RuntimeOperationKind::GpioWrite};
    RuntimeOperationState state{RuntimeOperationState::Pending};
    RuntimeOperationRecovery recovery{RuntimeOperationRecovery::None};
    bool replayed{};
    std::optional<std::uint32_t> object_id;
    std::optional<bool> value;
    std::optional<std::uint32_t> frequency_hz;
    std::optional<std::uint16_t> duty;
    std::optional<bool> active_low;
    std::optional<RuntimeOperationError> error;
};

enum class CanTrafficClass : std::uint8_t {
    Safety = 0,
    Motion = 1,
    System = 2,
    Interactive = 3,
    Streaming = 4,
    Bulk = 5,
};

struct CanTrafficClassCounters {
    std::uint64_t admitted_packets{};
    std::uint64_t rejected_packets{};
    std::uint64_t admitted_frames{};
    std::uint64_t estimated_wire_time_ns{};
};

enum class LinkTrafficMode : std::uint8_t {
    ClassicalCan = 0,
    CanFd = 1,
    Usb = 2,
};

struct CanTrafficStatus {
    LinkTrafficMode mode{LinkTrafficMode::ClassicalCan};
    /* 兼容旧客户端；新代码应读取 mode。 */
    bool can_fd{};
    std::uint32_t arbitration_bits_per_second{};
    std::uint32_t data_bits_per_second{};
    std::uint16_t maximum_utilization_permille{};
    std::uint32_t burst_window_ms{};
    std::uint64_t global_capacity_ns{};
    std::uint64_t global_available_ns{};
    std::uint64_t admitted_packets{};
    std::uint64_t rejected_packets{};
    std::uint64_t guaranteed_overruns{};
    std::uint64_t admitted_frames{};
    std::uint64_t estimated_wire_time_ns{};
    std::array<CanTrafficClassCounters, 6> classes{};
};

struct RuntimeResourceSnapshot {
    std::uint32_t node_id{};
    bool status_valid{};
    protocol::ResourceDescriptor descriptor;
    protocol::ResourceStatusPayload status;
};

struct RuntimeBusHealth {
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    bool last_status_valid{};
    protocol::BusTransactionStatus last_status{
        protocol::BusTransactionStatus::Ok};
    std::uint32_t consecutive_failures{};
    std::uint32_t peak_consecutive_failures{};
    std::uint64_t last_result_time_us{};
};

struct RuntimeNodeIssue {
    std::uint32_t node_id{};
    std::uint8_t code{};
};

enum class RuntimeClockState : std::uint8_t {
    Unsynced = 0U,
    Synced = 1U,
    Degraded = 2U,
};

struct RuntimeClockQuality {
    std::uint32_t node_id{};
    bool registered{};
    bool estimate_valid{};
    RuntimeClockState state{RuntimeClockState::Unsynced};
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

struct RuntimeSnapshot {
    std::uint16_t version{};
    std::uint64_t sequence{};
    std::vector<DiscoveredNode> nodes;
    CanTrafficStatus traffic;
    std::vector<RuntimeResourceSnapshot> resources;
    std::vector<RuntimeNodeIssue> node_issues;
    std::vector<RuntimeClockQuality> clocks;
    std::vector<RuntimeBusHealth> bus_health;
};

enum class GpioDirection : std::uint8_t {
    Input = 0,
    Output = 1,
};

enum class UartParity : std::uint8_t {
    None = 0,
    Odd = 1,
    Even = 2,
};

enum class UartReceiveMode : std::uint8_t {
    Polling = 0,
    Streaming = 1,
};

struct UartConfig {
    std::uint8_t port{};
    std::uint32_t baud_rate{};
    std::uint8_t data_bits{8};
    std::uint8_t stop_bits{1};
    UartParity parity{UartParity::None};
    UartReceiveMode receive_mode{UartReceiveMode::Polling};
};

struct UartStreamChunk {
    std::vector<std::uint8_t> data;
    std::uint64_t dropped_bytes{};
    std::uint64_t lost_events{};
};

enum class MotionGroupTransactionState : std::uint8_t {
    Idle = 0,
    Preparing,
    Ready,
    Committing,
    Committed,
    Aborting,
    Aborted,
};

struct MotionGroupMemberPlan {
    std::uint32_t node_id{};
    protocol::MotionSegmentPayload segment;
};

struct MotionGroupPlan {
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    // 与 Linux CLOCK_MONOTONIC/steady_clock 同源的绝对纳秒时间。
    std::uint64_t host_start_time_ns{};
    protocol::MotionGroupDigest content_digest{};
    std::vector<MotionGroupMemberPlan> members;
};

struct MotionGroupTransactionStatus {
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    MotionGroupTransactionState state{MotionGroupTransactionState::Idle};
    std::optional<protocol::MotionGroupAbortReason> abort_reason;
    std::uint16_t member_count{};
    std::uint16_t ready_count{};
    std::uint16_t committed_count{};
    std::uint16_t pending_request_count{};
    bool commit_dispatched{};
    // true 表示 COMMIT 批次已经释放给链路；后续 ABORT 只能尽力停止，
    // 不能保证物理回滚已经武装或开始执行的节点。
    bool abort_is_best_effort{};
};

class ClientException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 仅表示通过严格 IPC 错误信封收到的远端守护进程错误。message 只供人阅读，
// 调用方必须依据稳定 code/category/flags 分支。
class IpcErrorException : public ClientException {
public:
    IpcErrorException(std::uint16_t version, std::uint16_t code,
                      std::uint8_t category, bool retryable,
                      bool possibly_committed, const std::string& message);
    std::uint16_t version() const noexcept;
    std::uint16_t code() const noexcept;
    std::uint8_t category() const noexcept;
    bool retryable() const noexcept;
    bool possibly_committed() const noexcept;

private:
    std::uint16_t version_{};
    std::uint16_t code_{};
    std::uint8_t category_{};
    bool retryable_{};
    bool possibly_committed_{};
};

class RemoteException : public ClientException {
public:
    RemoteException(std::uint8_t status, const std::string& message);
    std::uint8_t status() const noexcept;

private:
    std::uint8_t status_;
};

// 同步客户端。每次调用使用独立 Unix Domain Socket 连接，因此同一实例可以
// 被多个线程并发调用，且一个调用超时不会污染其他调用的响应流。
class Client {
public:
    explicit Client(std::string socket_path = "/tmp/toolbusd.sock",
                    std::uint32_t node_id = 1);

    std::vector<std::uint8_t> ping(
        const std::vector<std::uint8_t>& data) const;
    NodeInfo get_info() const;
    protocol::FirmwareIdentityPayload firmware_identity() const;
    protocol::HealthSnapshot node_health_snapshot() const;
    std::uint64_t get_capabilities() const;
    std::vector<DiscoveredNode> list_nodes() const;
    DaemonIdentity daemon_identity() const;
    ToolbusdHealthSnapshot health_snapshot() const;
    void logical_recording_start(const std::string& output_name) const;
    std::string logical_recording_stop() const;
    LogicalRecordingStatus logical_recording_status() const;
    void runtime_control_acquire(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        std::uint32_t ttl_ms, std::uint16_t permissions = 0x0001U) const;
    RuntimeGpioWriteResult runtime_gpio_write(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key, bool value) const;
    void runtime_control_release(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::string& owner_key_id) const;
    RuntimeOperationOutcome runtime_gpio_write_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key, bool value) const;
    RuntimeOperationOutcome runtime_control_release_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::string& owner_key_id) const;
    RuntimeOperationOutcome runtime_pwm_configure_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key, std::uint32_t frequency_hz,
        std::uint16_t duty, bool active_low) const;
    RuntimeOperationOutcome runtime_pwm_stop_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key) const;
    RuntimeOperationOutcome runtime_timed_bitstream_configure_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key, std::uint32_t bit_period_ns,
        std::uint32_t zero_high_ns, std::uint32_t one_high_ns,
        std::uint32_t reset_time_us) const;
    RuntimeOperationOutcome runtime_timed_bitstream_frame_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key, std::uint16_t bit_count,
        const std::vector<std::uint8_t>& data) const;
    RuntimeOperationOutcome runtime_timed_bitstream_stop_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key) const;
    RuntimeOperationOutcome runtime_bus_resource_reset_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::array<std::uint8_t, 16>& expected_node_uuid,
        const std::string& owner_key_id, std::uint32_t resource_id,
        const std::string& idempotency_key) const;
    RuntimeOperationOutcome runtime_motion_group_cancel_operation(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::string& owner_key_id, const std::string& idempotency_key,
        std::uint64_t transaction_id, std::uint32_t group_id,
        std::uint32_t plan_generation, std::uint32_t deadline_ms) const;
    RuntimeOperationOutcome runtime_operation_status(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::string& owner_key_id,
        const std::array<std::uint8_t, 32>& operation_id) const;
    RuntimeOperationOutcome runtime_operation_lookup(
        const std::array<std::uint8_t, 16>& daemon_instance_id,
        const std::string& owner_key_id, RuntimeOperationKind kind,
        const std::array<std::uint8_t, 16>& lease_id,
        const std::string& idempotency_key) const;
    CanTrafficStatus traffic_status() const;
    RuntimeSnapshot runtime_snapshot(
        std::uint16_t maximum_resources = 128U,
        std::uint32_t timeout_ms = 5000U) const;
    std::optional<protocol::Packet> next_event() const;
    void enter_bootloader() const;
    void enter_usb_bootloader() const;

    std::vector<protocol::ResourceDescriptor> list_resources() const;
    protocol::ResourceDescriptor describe_resource(
        std::uint32_t resource_id) const;
    protocol::ResourceStatusPayload resource_status(
        std::uint32_t resource_id) const;
    void reset_resource(std::uint32_t resource_id) const;
    protocol::ResourceContract resource_contract(
        std::uint32_t resource_id) const;
    protocol::ResourceLeaseInfo acquire_resource(
        std::uint32_t resource_id, std::uint32_t duration_ms,
        protocol::ResourceLeaseMode mode =
            protocol::ResourceLeaseMode::Exclusive) const;
    protocol::ResourceLeaseInfo renew_resource(
        std::uint32_t resource_id, std::uint64_t lease_id,
        std::uint32_t duration_ms) const;
    void release_resource(std::uint32_t resource_id,
                          std::uint64_t lease_id) const;
    protocol::ResourceLeaseInfo resource_lease_status(
        std::uint32_t resource_id) const;

    protocol::BusResourceContract i2c_contract(
        std::uint32_t device_resource_id) const;
    protocol::BusTransferResult i2c_transfer(
        const protocol::I2cTransferRequest& request) const;
    protocol::BusResourceContract spi_contract(
        std::uint32_t device_resource_id) const;
    protocol::BusTransferResult spi_transfer(
        const protocol::SpiTransferRequest& request) const;
    protocol::AdcContract adc_contract(std::uint32_t resource_id) const;
    protocol::AdcSampleResult adc_sample(
        const protocol::AdcSampleRequest& request) const;
    protocol::StorageContract storage_contract(std::uint32_t resource_id) const;
    protocol::StorageReadResult storage_read(
        const protocol::StorageRangeRequest& request) const;
    void storage_erase(const protocol::StorageRangeRequest& request) const;
    void storage_program(const protocol::StorageProgramRequest& request) const;
    protocol::TimerContract timer_contract(std::uint32_t resource_id) const;
    protocol::TimerExecuteResult timer_execute(
        const protocol::TimerExecuteRequest& request) const;
    protocol::StreamContract stream_contract(
        std::uint32_t resource_id) const;
    protocol::StreamOpenResponse stream_open(
        const protocol::StreamOpenRequest& request) const;
    void stream_write(const protocol::StreamDataPayload& data) const;
    void stream_credit(const protocol::StreamCreditPayload& credit) const;
    // 仅从当前 stream_id 定向消费一个连续块；成功后精确归还该块信用。
    std::optional<protocol::StreamDataPayload> stream_read(
        std::uint32_t stream_id, std::uint32_t expected_sequence,
        std::uint32_t timeout_ms = 1000U) const;
    protocol::StreamStatusPayload stream_status(
        std::uint32_t stream_id) const;
    void stream_stop(std::uint32_t stream_id) const;

    protocol::DeviceParameterStatus device_parameter_status() const;
    std::vector<protocol::DeviceParameterDescriptor>
        list_device_parameters() const;
    protocol::DeviceParameterValue read_device_parameter(
        std::uint16_t id) const;
    protocol::DeviceParameterStatus write_device_parameter(
        std::uint16_t id, const std::vector<std::uint8_t>& value) const;
    protocol::DeviceParameterStatus write_device_parameter(
        std::uint16_t id, const std::vector<std::uint8_t>& value,
        std::uint32_t expected_generation) const;

    std::uint32_t gpio_create(std::uint16_t pin,
                              GpioDirection direction,
                              bool initial_value = false) const;
    bool gpio_read(std::uint32_t object_id) const;
    void gpio_write(std::uint32_t object_id, bool value) const;
    void gpio_close(std::uint32_t object_id) const;
    void gpio_input_subscribe(
        std::uint32_t object_id,
        const protocol::GpioInputSubscription& subscription) const;
    protocol::GpioInputEventStatus gpio_input_event_status(
        std::uint32_t object_id) const;

    std::uint32_t pwm_create(
        const protocol::PwmCreatePayload& config) const;
    void pwm_write(std::uint32_t object_id, std::uint16_t duty) const;
    void pwm_stop(std::uint32_t object_id) const;

    std::uint32_t timed_bitstream_create(
        const protocol::TimedBitstreamCreatePayload& config) const;
    void timed_bitstream_write(
        std::uint32_t object_id,
        const protocol::TimedBitstreamWritePayload& data) const;
    void timed_bitstream_abort(std::uint32_t object_id) const;

    std::uint32_t uart_create(const UartConfig& config) const;
    std::vector<std::uint8_t> uart_read(
        std::uint32_t object_id, std::size_t maximum_length) const;
    void uart_write(std::uint32_t object_id,
                    const std::vector<std::uint8_t>& data) const;
    void uart_write_all(
        std::uint32_t object_id,
        const std::vector<std::uint8_t>& data,
        std::uint32_t timeout_ms = 3000) const;
    std::optional<UartStreamChunk> uart_stream_read(
        std::uint32_t object_id, std::size_t maximum_length,
        std::uint32_t timeout_ms = 1000) const;

    protocol::MotionAcceptancePayload motion_enqueue(
        const protocol::MotionSegmentPayload& segment) const;
    protocol::MotionContractPayload motion_contract(
        bool refresh = false) const;
    protocol::MotionStatusPayload motion_status() const;
    void motion_abort() const;
    void motion_clear_fault() const;

    MotionGroupTransactionStatus motion_group_submit(
        const MotionGroupPlan& plan) const;
    MotionGroupTransactionStatus motion_group_status(
        std::uint64_t transaction_id, std::uint32_t group_id,
        std::uint32_t plan_generation) const;
    MotionGroupTransactionStatus motion_group_cancel(
        std::uint64_t transaction_id, std::uint32_t group_id,
        std::uint32_t plan_generation) const;

    protocol::Packet transact(protocol::Packet request) const;
    const std::string& socket_path() const noexcept;
    std::uint32_t node_id() const noexcept;

private:
    struct MotionContractCache;
    struct StreamReadCache;

    protocol::Packet command(protocol::Command command,
                             std::vector<std::uint8_t> payload = {},
                             std::uint32_t object_id = 0) const;

    std::string socket_path_;
    std::uint32_t node_id_;
    std::shared_ptr<MotionContractCache> motion_contract_cache_;
    std::shared_ptr<StreamReadCache> stream_read_cache_;
};

}
