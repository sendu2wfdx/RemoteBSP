#include "remotebsp/client.hpp"

#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace remotebsp {
namespace {

class SocketHandle {
public:
    explicit SocketHandle(int value) : value_(value) {}
    ~SocketHandle() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    int get() const noexcept { return value_; }

private:
    int value_;
};

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index])
                 << (index * 8U);
    }
    return value;
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

int connect_socket(const std::string& path,
                   std::uint32_t timeout_ms = 3000U) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        throw ClientException("Unix Domain Socket 路径无效或过长");
    }
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "创建本地 IPC 套接字失败");
    }
    if (timeout_ms == 0U || timeout_ms > 60000U) {
        throw ClientException("本地 IPC 超时参数无效");
    }
    const timeval timeout{
        static_cast<time_t>(timeout_ms / 1000U),
        static_cast<suseconds_t>((timeout_ms % 1000U) * 1000U)};
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) < 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) < 0) {
        const int saved_errno = errno;
        ::close(descriptor);
        throw std::system_error(saved_errno, std::generic_category(),
                                "设置本地 IPC 超时失败");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) < 0) {
        const int saved_errno = errno;
        ::close(descriptor);
        throw std::system_error(saved_errno, std::generic_category(),
                                "连接 toolbusd 失败");
    }
    return descriptor;
}

std::vector<std::uint8_t> body(const protocol::Packet& response,
                               std::size_t expected = 0) {
    if (response.payload.empty()) {
        throw ClientException("远端响应缺少状态码");
    }
    if (response.payload[0] != 0) {
        throw RemoteException(response.payload[0],
                              "远端操作失败，状态码 " +
                                  std::to_string(response.payload[0]));
    }
    const std::size_t size = response.payload.size() - 1U;
    if (expected != 0 && size != expected) {
        throw ClientException("远端响应载荷长度无效");
    }
    return {response.payload.begin() + 1, response.payload.end()};
}

MotionGroupTransactionStatus motion_group_status_from_ipc(
    const toolbusd::IpcResponse& response) {
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto snapshot =
        toolbusd::decode_ipc_motion_group_snapshot(response.body);
    MotionGroupTransactionStatus result;
    result.transaction_id = snapshot.transaction_id;
    result.group_id = snapshot.group_id;
    result.plan_generation = snapshot.plan_generation;
    result.state = static_cast<MotionGroupTransactionState>(snapshot.state);
    result.abort_reason = snapshot.abort_reason;
    result.member_count = snapshot.member_count;
    result.ready_count = snapshot.ready_count;
    result.committed_count = snapshot.committed_count;
    result.pending_request_count = snapshot.pending_request_count;
    result.commit_dispatched = snapshot.commit_dispatched;
    result.abort_is_best_effort = snapshot.abort_is_best_effort;
    return result;
}

[[noreturn]] void throw_structured_ipc_error(
    const toolbusd::IpcResponse& response, const char* fallback) {
    try {
        const auto error = toolbusd::decode_ipc_error_envelope(response.body);
        throw IpcErrorException(
            error.version, static_cast<std::uint16_t>(error.code),
            static_cast<std::uint8_t>(error.category), error.retryable,
            error.possibly_committed, error.message);
    } catch (const IpcErrorException&) {
        throw;
    } catch (const std::exception&) {
        // 覆盖请求绝不把旧 daemon 文本或畸形二进制提升为机器合同。
        throw ClientException(std::string(fallback) +
                              "：toolbusd 错误信封无效或版本过旧");
    }
}

}

IpcErrorException::IpcErrorException(
    std::uint16_t version, std::uint16_t code, std::uint8_t category,
    bool retryable, bool possibly_committed, const std::string& message)
    : ClientException(message), version_(version), code_(code),
      category_(category), retryable_(retryable),
      possibly_committed_(possibly_committed) {}

std::uint16_t IpcErrorException::version() const noexcept { return version_; }
std::uint16_t IpcErrorException::code() const noexcept { return code_; }
std::uint8_t IpcErrorException::category() const noexcept { return category_; }
bool IpcErrorException::retryable() const noexcept { return retryable_; }
bool IpcErrorException::possibly_committed() const noexcept {
    return possibly_committed_;
}

RemoteException::RemoteException(std::uint8_t status,
                                 const std::string& message)
    : ClientException(message), status_(status) {}

std::uint8_t RemoteException::status() const noexcept { return status_; }

struct Client::MotionContractCache {
    std::mutex mutex;
    std::optional<protocol::MotionContractPayload> value;
};

Client::Client(std::string socket_path, std::uint32_t node_id)
    : socket_path_(std::move(socket_path)), node_id_(node_id),
      motion_contract_cache_(std::make_shared<MotionContractCache>()) {
    if (socket_path_.empty() ||
        socket_path_.size() >= sizeof(sockaddr_un{}.sun_path)) {
        throw ClientException("Unix Domain Socket 路径无效或过长");
    }
    if (node_id_ == 0 || node_id_ > 127) {
        throw ClientException("目标节点 ID 必须位于 1～127");
    }
}

protocol::Packet Client::transact(protocol::Packet request) const {
    request.header.version = protocol::kProtocolVersion;
    request.header.message_type = protocol::MessageType::Request;
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_request(socket.get(), request, node_id_);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status == toolbusd::IpcStatus::TimedOut) {
        throw ClientException("远端请求超时");
    }
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto packet = protocol::decode(response.body);
    if (packet.header.message_type != protocol::MessageType::Response ||
        packet.header.command != request.header.command) {
        throw ClientException("toolbusd 返回了不匹配的响应");
    }
    return packet;
}

protocol::Packet Client::command(protocol::Command command_value,
                                 std::vector<std::uint8_t> payload,
                                 std::uint32_t object_id) const {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(command_value);
    request.header.object_id = object_id;
    request.payload = std::move(payload);
    return transact(std::move(request));
}

std::vector<std::uint8_t> Client::ping(
    const std::vector<std::uint8_t>& data) const {
    if (data.size() >= protocol::kMaximumPayloadSize) {
        throw ClientException("PING 数据过长");
    }
    return body(command(protocol::Command::Ping, data));
}

NodeInfo Client::get_info() const {
    const auto data = body(command(protocol::Command::GetInfo), 27);
    NodeInfo info;
    std::copy_n(data.begin(), info.uuid.size(), info.uuid.begin());
    info.firmware_major = read_u16(data.data() + 16);
    info.firmware_minor = read_u16(data.data() + 18);
    info.firmware_patch = read_u16(data.data() + 20);
    info.board_type = read_u32(data.data() + 22);
    info.protocol_version = data[26];
    return info;
}

std::uint64_t Client::get_capabilities() const {
    const auto data = body(command(protocol::Command::GetCapability), 8);
    return read_u64(data.data());
}

void Client::enter_bootloader() const {
    static constexpr std::array<std::uint8_t, 8> confirmation{
        'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};
    body(command(protocol::Command::BootloaderEnter,
                 {confirmation.begin(), confirmation.end()}));
}

void Client::enter_usb_bootloader() const {
    static constexpr std::array<std::uint8_t, 8> confirmation{
        'R', 'B', 'S', 'P', 'B', 'O', 'O', 'T'};
    body(command(protocol::Command::BootloaderEnterUsb,
                 {confirmation.begin(), confirmation.end()}));
}

std::vector<DiscoveredNode> Client::list_nodes() const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_node_list_request(socket.get());
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    std::vector<DiscoveredNode> result;
    for (const auto& node :
         toolbusd::decode_ipc_node_list(response.body)) {
        NodeInfo identity;
        identity.uuid = node.uuid;
        identity.firmware_major = node.firmware_major;
        identity.firmware_minor = node.firmware_minor;
        identity.firmware_patch = node.firmware_patch;
        identity.board_type = node.board_type;
        identity.protocol_version = node.protocol_version;
        result.push_back(
            {identity, node.node_id, node.online, node.ready});
    }
    return result;
}

DaemonIdentity Client::daemon_identity() const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_daemon_identity_request(socket.get());
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto source =
        toolbusd::decode_ipc_daemon_identity(response.body);
    return {source.version, source.instance_id};
}

ToolbusdHealthSnapshot Client::health_snapshot() const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_health_snapshot_request(socket.get());
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw_structured_ipc_error(response, "toolbusd 健康快照失败");
    }
    const auto source = toolbusd::decode_ipc_health_snapshot(response.body);
    return {source.version, source.daemon_instance_id, source.health};
}

void Client::runtime_control_acquire(
    const std::array<std::uint8_t, 16>& daemon_instance_id,
    const std::array<std::uint8_t, 16>& lease_id,
    const std::array<std::uint8_t, 16>& expected_node_uuid,
    const std::string& owner_key_id, std::uint32_t resource_id,
    std::uint32_t ttl_ms) const {
    toolbusd::RuntimeControlAcquireRequest request;
    request.daemon_instance_id = daemon_instance_id;
    request.lease_id = lease_id;
    request.expected_node_uuid = expected_node_uuid;
    request.owner_key_id = owner_key_id;
    request.node_id = node_id_;
    request.resource_id = resource_id;
    request.ttl_ms = ttl_ms;
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_runtime_control_acquire_request(socket.get(), request);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw_structured_ipc_error(response, "Runtime 控制租约登记失败");
    }
    if (!response.body.empty()) {
        throw ClientException("Runtime 控制租约登记响应载荷无效");
    }
}

RuntimeGpioWriteResult Client::runtime_gpio_write(
    const std::array<std::uint8_t, 16>& daemon_instance_id,
    const std::array<std::uint8_t, 16>& lease_id,
    const std::array<std::uint8_t, 16>& expected_node_uuid,
    const std::string& owner_key_id, std::uint32_t resource_id,
    const std::string& idempotency_key, bool value) const {
    toolbusd::RuntimeGpioWriteRequest request;
    request.daemon_instance_id = daemon_instance_id;
    request.lease_id = lease_id;
    request.expected_node_uuid = expected_node_uuid;
    request.owner_key_id = owner_key_id;
    request.node_id = node_id_;
    request.resource_id = resource_id;
    request.idempotency_key = idempotency_key;
    request.value = value;
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_runtime_gpio_write_request(socket.get(), request);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw_structured_ipc_error(response, "Runtime GPIO 写入失败");
    }
    const auto result =
        toolbusd::decode_ipc_runtime_gpio_write_result(response.body);
    return {result.object_id, result.value, result.replayed};
}

void Client::runtime_control_release(
    const std::array<std::uint8_t, 16>& daemon_instance_id,
    const std::array<std::uint8_t, 16>& lease_id,
    const std::string& owner_key_id) const {
    toolbusd::RuntimeControlReleaseRequest request;
    request.daemon_instance_id = daemon_instance_id;
    request.lease_id = lease_id;
    request.owner_key_id = owner_key_id;
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_runtime_control_release_request(socket.get(), request);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw_structured_ipc_error(response, "Runtime 控制租约释放失败");
    }
    if (!response.body.empty()) {
        throw ClientException("Runtime 控制租约释放响应载荷无效");
    }
}

CanTrafficStatus Client::traffic_status() const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_traffic_status_request(socket.get());
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto snapshot =
        toolbusd::decode_ipc_traffic_status(response.body);
    CanTrafficStatus status;
    status.mode = static_cast<LinkTrafficMode>(snapshot.mode);
    status.can_fd =
        snapshot.mode == toolbusd::TrafficBusMode::CanFd;
    status.arbitration_bits_per_second =
        snapshot.arbitration_bits_per_second;
    status.data_bits_per_second = snapshot.data_bits_per_second;
    status.maximum_utilization_permille =
        snapshot.maximum_utilization_permille;
    status.burst_window_ms = snapshot.burst_window_ms;
    status.global_capacity_ns = snapshot.global_capacity_ns;
    status.global_available_ns = snapshot.global_available_ns;
    status.admitted_packets = snapshot.admitted_packets;
    status.rejected_packets = snapshot.rejected_packets;
    status.guaranteed_overruns = snapshot.guaranteed_overruns;
    status.admitted_frames = snapshot.admitted_frames;
    status.estimated_wire_time_ns =
        snapshot.estimated_wire_time_ns;
    for (std::size_t index = 0; index < status.classes.size(); ++index) {
        status.classes[index] = {
            snapshot.classes[index].admitted_packets,
            snapshot.classes[index].rejected_packets,
            snapshot.classes[index].admitted_frames,
            snapshot.classes[index].estimated_wire_time_ns};
    }
    return status;
}

RuntimeSnapshot Client::runtime_snapshot(
    std::uint16_t maximum_resources, std::uint32_t timeout_ms) const {
    if (maximum_resources == 0U || maximum_resources >
            toolbusd::kMaximumRuntimeSnapshotResources ||
        timeout_ms == 0U || timeout_ms >
            toolbusd::kMaximumRuntimeSnapshotTimeoutMs) {
        throw ClientException("Runtime 快照参数超出允许范围");
    }
    SocketHandle socket(connect_socket(socket_path_, timeout_ms + 250U));
    toolbusd::write_ipc_runtime_snapshot_request(
        socket.get(), maximum_resources, timeout_ms);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status == toolbusd::IpcStatus::TimedOut) {
        throw ClientException("Runtime 快照请求超时");
    }
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto source =
        toolbusd::decode_ipc_runtime_snapshot(response.body);
    RuntimeSnapshot result;
    result.version = source.version;
    result.sequence = source.sequence;
    for (const auto& node : source.nodes) {
        NodeInfo identity;
        identity.uuid = node.uuid;
        identity.firmware_major = node.firmware_major;
        identity.firmware_minor = node.firmware_minor;
        identity.firmware_patch = node.firmware_patch;
        identity.board_type = node.board_type;
        identity.protocol_version = node.protocol_version;
        result.nodes.push_back(
            {identity, node.node_id, node.online, node.ready});
    }
    result.traffic.mode = static_cast<LinkTrafficMode>(source.traffic.mode);
    result.traffic.can_fd =
        source.traffic.mode == toolbusd::TrafficBusMode::CanFd;
    result.traffic.arbitration_bits_per_second =
        source.traffic.arbitration_bits_per_second;
    result.traffic.data_bits_per_second =
        source.traffic.data_bits_per_second;
    result.traffic.maximum_utilization_permille =
        source.traffic.maximum_utilization_permille;
    result.traffic.burst_window_ms = source.traffic.burst_window_ms;
    result.traffic.global_capacity_ns = source.traffic.global_capacity_ns;
    result.traffic.global_available_ns = source.traffic.global_available_ns;
    result.traffic.admitted_packets = source.traffic.admitted_packets;
    result.traffic.rejected_packets = source.traffic.rejected_packets;
    result.traffic.guaranteed_overruns = source.traffic.guaranteed_overruns;
    result.traffic.admitted_frames = source.traffic.admitted_frames;
    result.traffic.estimated_wire_time_ns =
        source.traffic.estimated_wire_time_ns;
    for (std::size_t index = 0U; index < result.traffic.classes.size();
         ++index) {
        const auto& counters = source.traffic.classes[index];
        result.traffic.classes[index] = {
            counters.admitted_packets, counters.rejected_packets,
            counters.admitted_frames, counters.estimated_wire_time_ns};
    }
    result.resources.reserve(source.resources.size());
    for (const auto& resource : source.resources) {
        result.resources.push_back(
            {resource.node_id, resource.status_valid,
             resource.descriptor, resource.status});
    }
    result.node_issues.reserve(source.node_issues.size());
    for (const auto& issue : source.node_issues) {
        result.node_issues.push_back(
            {issue.node_id, static_cast<std::uint8_t>(issue.error)});
    }
    result.clocks.reserve(source.clocks.size());
    for (const auto& clock : source.clocks) {
        result.clocks.push_back({
            clock.node_id, clock.registered, clock.estimate_valid,
            static_cast<RuntimeClockState>(clock.state), clock.boot_epoch,
            clock.model_generation, clock.sample_count,
            clock.selected_sample_count, clock.drift_uncertainty_ppm,
            clock.rate_deviation_ppb, clock.minimum_network_rtt_ns,
            clock.error_bound_ns, clock.sample_age_ns,
            clock.last_sample_host_time_ns});
    }
    return result;
}

std::optional<protocol::Packet> Client::next_event() const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_next_event_request(socket.get(), node_id_);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status == toolbusd::IpcStatus::TimedOut) {
        return std::nullopt;
    }
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto packet = protocol::decode(response.body);
    if (packet.header.message_type != protocol::MessageType::Event) {
        throw ClientException("toolbusd 返回的消息不是事件");
    }
    return packet;
}

std::vector<protocol::ResourceDescriptor> Client::list_resources() const {
    return protocol::decode_resource_list(
        body(command(protocol::Command::ResourceEnum)));
}

protocol::ResourceDescriptor Client::describe_resource(
    std::uint32_t resource_id) const {
    return protocol::decode_resource_descriptor(body(command(
        protocol::Command::ResourceDescribe,
        protocol::encode_resource_id(resource_id))));
}

protocol::ResourceStatusPayload Client::resource_status(
    std::uint32_t resource_id) const {
    return protocol::decode_resource_status(body(command(
        protocol::Command::ResourceStatus,
        protocol::encode_resource_id(resource_id))));
}

void Client::reset_resource(std::uint32_t resource_id) const {
    body(command(protocol::Command::ResourceReset,
                 protocol::encode_resource_id(resource_id)));
}

protocol::ResourceContract Client::resource_contract(
    std::uint32_t resource_id) const {
    return protocol::decode_resource_contract(body(command(
        protocol::Command::ResourceContract,
        protocol::encode_resource_id(resource_id))));
}

protocol::ResourceLeaseInfo Client::acquire_resource(
    std::uint32_t resource_id, std::uint32_t duration_ms,
    protocol::ResourceLeaseMode mode) const {
    if (duration_ms == 0 ||
        mode == protocol::ResourceLeaseMode::None) {
        throw ClientException("资源租约参数无效");
    }
    return protocol::decode_resource_lease_info(body(command(
        protocol::Command::ResourceAcquire,
        protocol::encode_resource_lease_request(
            {resource_id, duration_ms, mode}))));
}

protocol::ResourceLeaseInfo Client::renew_resource(
    std::uint32_t resource_id, std::uint64_t lease_id,
    std::uint32_t duration_ms) const {
    if (lease_id == 0 || duration_ms == 0) {
        throw ClientException("资源续租参数无效");
    }
    return protocol::decode_resource_lease_info(body(command(
        protocol::Command::ResourceRenew,
        protocol::encode_resource_lease_token_request(
            {resource_id, lease_id, duration_ms}))));
}

void Client::release_resource(std::uint32_t resource_id,
                              std::uint64_t lease_id) const {
    if (lease_id == 0) {
        throw ClientException("资源租约 ID 不能为零");
    }
    body(command(
        protocol::Command::ResourceRelease,
        protocol::encode_resource_lease_token_request(
            {resource_id, lease_id, 0})));
}

protocol::ResourceLeaseInfo Client::resource_lease_status(
    std::uint32_t resource_id) const {
    return protocol::decode_resource_lease_info(body(command(
        protocol::Command::ResourceLeaseStatus,
        protocol::encode_resource_id(resource_id))));
}

protocol::BusResourceContract Client::i2c_contract(
    std::uint32_t device_resource_id) const {
    return protocol::decode_bus_resource_contract(body(command(
        protocol::Command::I2cContract,
        protocol::encode_resource_id(device_resource_id))));
}

protocol::BusTransferResult Client::i2c_transfer(
    const protocol::I2cTransferRequest& request) const {
    return protocol::decode_bus_transfer_result(body(command(
        protocol::Command::I2cTransfer,
        protocol::encode_i2c_transfer_request(request))));
}

protocol::BusResourceContract Client::spi_contract(
    std::uint32_t device_resource_id) const {
    return protocol::decode_bus_resource_contract(body(command(
        protocol::Command::SpiContract,
        protocol::encode_resource_id(device_resource_id))));
}

protocol::BusTransferResult Client::spi_transfer(
    const protocol::SpiTransferRequest& request) const {
    return protocol::decode_bus_transfer_result(body(command(
        protocol::Command::SpiTransfer,
        protocol::encode_spi_transfer_request(request))));
}

protocol::StreamContract Client::stream_contract(
    std::uint32_t resource_id) const {
    return protocol::decode_stream_contract(body(command(
        protocol::Command::StreamContract,
        protocol::encode_resource_id(resource_id))));
}

protocol::StreamOpenResponse Client::stream_open(
    const protocol::StreamOpenRequest& request) const {
    return protocol::decode_stream_open_response(body(command(
        protocol::Command::StreamOpen,
        protocol::encode_stream_open_request(request))));
}

void Client::stream_write(const protocol::StreamDataPayload& data) const {
    body(command(protocol::Command::StreamData,
                 protocol::encode_stream_data(data)));
}

void Client::stream_credit(
    const protocol::StreamCreditPayload& credit) const {
    body(command(protocol::Command::StreamCredit,
                 protocol::encode_stream_credit(credit)));
}

protocol::StreamStatusPayload Client::stream_status(
    std::uint32_t stream_id) const {
    return protocol::decode_stream_status(body(command(
        protocol::Command::StreamStatus,
        protocol::encode_resource_id(stream_id))));
}

void Client::stream_stop(std::uint32_t stream_id) const {
    body(command(protocol::Command::StreamStop,
                 protocol::encode_resource_id(stream_id)));
}

protocol::DeviceParameterStatus Client::device_parameter_status() const {
    return protocol::decode_device_parameter_status(
        body(command(protocol::Command::DeviceParameterStatus)));
}

std::vector<protocol::DeviceParameterDescriptor>
Client::list_device_parameters() const {
    return protocol::decode_device_parameter_descriptors(
        body(command(protocol::Command::DeviceParameterList)));
}

protocol::DeviceParameterValue Client::read_device_parameter(
    std::uint16_t id) const {
    return protocol::decode_device_parameter_value(body(command(
        protocol::Command::DeviceParameterRead,
        protocol::encode_device_parameter_read_request(id))));
}

protocol::DeviceParameterStatus Client::write_device_parameter(
    std::uint16_t id, const std::vector<std::uint8_t>& value) const {
    const auto before = device_parameter_status();
    const auto unlock = protocol::decode_device_parameter_unlock_response(
        body(command(
            protocol::Command::DeviceParameterUnlock,
            protocol::encode_device_parameter_unlock_request(
                {before.generation,
                 protocol::kDeviceParameterUnlockConfirmation}))));
    try {
        const auto after = protocol::decode_device_parameter_status(
            body(command(
                protocol::Command::DeviceParameterWrite,
                protocol::encode_device_parameter_write_request(
                    {unlock.generation, unlock.token, id, value}))));
        body(command(protocol::Command::DeviceParameterLock));
        return after;
    } catch (...) {
        try {
            body(command(protocol::Command::DeviceParameterLock));
        } catch (...) {
            // 保留原始写入异常；维护解锁仍会在60秒后自动失效。
        }
        throw;
    }
}

std::uint32_t Client::gpio_create(std::uint16_t pin,
                                  GpioDirection direction,
                                  bool initial_value) const {
    const auto direction_value = static_cast<std::uint8_t>(direction);
    if (direction_value > static_cast<std::uint8_t>(
                              GpioDirection::Output)) {
        throw ClientException("GPIO 方向无效");
    }
    const auto response = command(
        protocol::Command::GpioCreate,
        {static_cast<std::uint8_t>(pin),
         static_cast<std::uint8_t>(pin >> 8U), direction_value,
         static_cast<std::uint8_t>(initial_value)});
    body(response);
    if (response.header.object_id == 0) {
        throw ClientException("GPIO_CREATE 未返回对象 ID");
    }
    return response.header.object_id;
}

bool Client::gpio_read(std::uint32_t object_id) const {
    if (object_id == 0) {
        throw ClientException("GPIO 对象 ID 不能为零");
    }
    const auto data =
        body(command(protocol::Command::GpioRead, {}, object_id), 1);
    if (data[0] > 1) {
        throw ClientException("GPIO_READ 返回了无效电平");
    }
    return data[0] != 0;
}

void Client::gpio_write(std::uint32_t object_id, bool value) const {
    if (object_id == 0) {
        throw ClientException("GPIO 对象 ID 不能为零");
    }
    body(command(protocol::Command::GpioWrite,
                  {static_cast<std::uint8_t>(value)}, object_id));
}

void Client::gpio_close(std::uint32_t object_id) const {
    if (object_id == 0U) {
        throw ClientException("GPIO 对象 ID 不能为零");
    }
    body(command(protocol::Command::GpioClose,
                 protocol::encode_gpio_close(), object_id));
}

std::uint32_t Client::pwm_create(
    const protocol::PwmCreatePayload& config) const {
    const auto response = command(
        protocol::Command::PwmCreate,
        protocol::encode_pwm_create(config));
    body(response);
    if (response.header.object_id == 0) {
        throw ClientException("PWM_CREATE 未返回对象 ID");
    }
    return response.header.object_id;
}

void Client::pwm_write(std::uint32_t object_id,
                       std::uint16_t duty) const {
    if (object_id == 0) {
        throw ClientException("PWM 对象 ID 不能为零");
    }
    body(command(protocol::Command::PwmWrite,
                 protocol::encode_pwm_duty(duty), object_id));
}

void Client::pwm_stop(std::uint32_t object_id) const {
    if (object_id == 0) {
        throw ClientException("PWM 对象 ID 不能为零");
    }
    body(command(protocol::Command::PwmStop, {}, object_id));
}

std::uint32_t Client::timed_bitstream_create(
    const protocol::TimedBitstreamCreatePayload& config) const {
    const auto response = command(
        protocol::Command::TimedBitstreamCreate,
        protocol::encode_timed_bitstream_create(config));
    body(response);
    if (response.header.object_id == 0) {
        throw ClientException(
            "TIMED_BITSTREAM_CREATE 未返回对象 ID");
    }
    return response.header.object_id;
}

void Client::timed_bitstream_write(
    std::uint32_t object_id,
    const protocol::TimedBitstreamWritePayload& data) const {
    if (object_id == 0) {
        throw ClientException("定时位流对象 ID 不能为零");
    }
    body(command(protocol::Command::TimedBitstreamWrite,
                 protocol::encode_timed_bitstream_write(data),
                 object_id));
}

void Client::timed_bitstream_abort(std::uint32_t object_id) const {
    if (object_id == 0) {
        throw ClientException("定时位流对象 ID 不能为零");
    }
    body(command(protocol::Command::TimedBitstreamAbort, {}, object_id));
}

std::uint32_t Client::uart_create(const UartConfig& config) const {
    const auto parity = static_cast<std::uint8_t>(config.parity);
    const auto receive_mode =
        static_cast<std::uint8_t>(config.receive_mode);
    if (config.baud_rate == 0 || config.data_bits < 5 ||
        config.data_bits > 8 ||
        (config.stop_bits != 1 && config.stop_bits != 2) ||
        parity > static_cast<std::uint8_t>(UartParity::Even) ||
        receive_mode >
            static_cast<std::uint8_t>(UartReceiveMode::Streaming)) {
        throw ClientException("UART 配置参数无效");
    }
    std::vector<std::uint8_t> payload{config.port};
    append_u32(payload, config.baud_rate);
    payload.push_back(config.data_bits);
    payload.push_back(config.stop_bits);
    payload.push_back(parity);
    if (config.receive_mode == UartReceiveMode::Streaming) {
        payload.push_back(receive_mode);
    }
    const auto response =
        command(protocol::Command::UartCreate, std::move(payload));
    body(response);
    if (response.header.object_id == 0) {
        throw ClientException("UART_CREATE 未返回对象 ID");
    }
    return response.header.object_id;
}

std::vector<std::uint8_t> Client::uart_read(
    std::uint32_t object_id, std::size_t maximum_length) const {
    if (object_id == 0 || maximum_length == 0 ||
        maximum_length > protocol::kMaximumPayloadSize - 1U ||
        maximum_length > std::numeric_limits<std::uint16_t>::max()) {
        throw ClientException("UART 读取参数无效");
    }
    return body(command(
        protocol::Command::UartRead,
        {static_cast<std::uint8_t>(maximum_length),
         static_cast<std::uint8_t>(maximum_length >> 8U)},
        object_id));
}

void Client::uart_write(
    std::uint32_t object_id,
    const std::vector<std::uint8_t>& data) const {
    if (object_id == 0 || data.empty() ||
        data.size() > protocol::kMaximumPayloadSize - 1U) {
        throw ClientException("UART 写入参数无效");
    }
    body(command(protocol::Command::UartWrite, data, object_id));
}

void Client::uart_write_all(
    std::uint32_t object_id,
    const std::vector<std::uint8_t>& data,
    std::uint32_t timeout_ms) const {
    if (object_id == 0 || data.empty() || timeout_ms == 0) {
        throw ClientException("UART 完整写入参数无效");
    }
    constexpr std::size_t chunk_size = 64;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count =
            std::min(chunk_size, data.size() - offset);
        const std::vector<std::uint8_t> chunk(
            data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() +
                static_cast<std::ptrdiff_t>(offset + count));
        try {
            uart_write(object_id, chunk);
            offset += count;
        } catch (const RemoteException& error) {
            const bool retryable =
                error.status() == 5U || error.status() == 7U ||
                error.status() == 8U;
            if (!retryable ||
                std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (std::chrono::steady_clock::now() >= deadline &&
            offset < data.size()) {
            throw ClientException("UART 完整写入超时");
        }
    }
}

std::optional<UartStreamChunk> Client::uart_stream_read(
    std::uint32_t object_id, std::size_t maximum_length,
    std::uint32_t timeout_ms) const {
    if (object_id == 0 || maximum_length == 0 ||
        maximum_length > 64U * 1024U - 20U ||
        maximum_length > std::numeric_limits<std::uint32_t>::max() ||
        timeout_ms > 60000U) {
        throw ClientException("UART 流读取参数无效");
    }
    SocketHandle socket(connect_socket(socket_path_));
    const auto receive_timeout =
        std::chrono::milliseconds(timeout_ms) +
        std::chrono::milliseconds(2000);
    const timeval socket_timeout{
        static_cast<time_t>(receive_timeout.count() / 1000),
        static_cast<suseconds_t>(
            (receive_timeout.count() % 1000) * 1000)};
    if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                     &socket_timeout, sizeof(socket_timeout)) < 0) {
        throw std::system_error(
            errno, std::generic_category(),
            "设置 UART 流读取超时失败");
    }
    toolbusd::write_ipc_uart_stream_read_request(
        socket.get(), node_id_, object_id,
        static_cast<std::uint32_t>(maximum_length), timeout_ms);
    const auto response = toolbusd::read_ipc_response(socket.get());
    if (response.status == toolbusd::IpcStatus::TimedOut) {
        return std::nullopt;
    }
    if (response.status != toolbusd::IpcStatus::Ok) {
        throw ClientException(
            std::string(response.body.begin(), response.body.end()));
    }
    const auto ipc_chunk =
        toolbusd::decode_ipc_uart_stream_chunk(response.body);
    return UartStreamChunk{ipc_chunk.data, ipc_chunk.dropped_bytes,
                           ipc_chunk.lost_events};
}

protocol::MotionAcceptancePayload Client::motion_enqueue(
    const protocol::MotionSegmentPayload& segment) const {
    try {
        protocol::validate_motion_segment_against_contract(
            segment, motion_contract());
        return protocol::decode_motion_acceptance(body(command(
            protocol::Command::MotionEnqueue,
            protocol::encode_motion_segment(segment))));
    } catch (const protocol::MotionPayloadException& error) {
        if (error.code() == protocol::MotionPayloadError::ContractMismatch ||
            error.code() == protocol::MotionPayloadError::RateExceeded) {
            throw ClientException(
                std::string("主机运动能力准入拒绝: ") + error.what());
        }
        throw ClientException(
            std::string("运动段编码或响应无效: ") + error.what());
    }
}

protocol::MotionContractPayload Client::motion_contract(
    bool refresh) const {
    std::lock_guard<std::mutex> lock(motion_contract_cache_->mutex);
    if (!refresh && motion_contract_cache_->value.has_value()) {
        return *motion_contract_cache_->value;
    }
    try {
        auto contract = protocol::decode_motion_contract(
            body(command(protocol::Command::MotionContract)));
        motion_contract_cache_->value = contract;
        return contract;
    } catch (const protocol::MotionPayloadException& error) {
        throw ClientException(
            std::string("运动能力合同响应无效: ") + error.what());
    }
}

protocol::MotionStatusPayload Client::motion_status() const {
    try {
        return protocol::decode_motion_status(
            body(command(protocol::Command::MotionStatus)));
    } catch (const protocol::MotionPayloadException& error) {
        throw ClientException(
            std::string("运动状态响应无效: ") + error.what());
    }
}

void Client::motion_abort() const {
    body(command(protocol::Command::MotionAbort));
}

void Client::motion_clear_fault() const {
    body(command(protocol::Command::MotionClearFault));
}

MotionGroupTransactionStatus Client::motion_group_submit(
    const MotionGroupPlan& plan) const {
    toolbusd::MotionGroupPlan ipc_plan;
    ipc_plan.transaction_id = plan.transaction_id;
    ipc_plan.group_id = plan.group_id;
    ipc_plan.plan_generation = plan.plan_generation;
    ipc_plan.host_start_time_ns = plan.host_start_time_ns;
    ipc_plan.content_digest = plan.content_digest;
    ipc_plan.members.reserve(plan.members.size());
    for (const auto& member : plan.members) {
        ipc_plan.members.push_back({member.node_id, member.segment});
    }
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_motion_group_submit_request(socket.get(), ipc_plan);
    return motion_group_status_from_ipc(
        toolbusd::read_ipc_response(socket.get()));
}

MotionGroupTransactionStatus Client::motion_group_status(
    std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation) const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_motion_group_status_request(
        socket.get(), transaction_id, group_id, plan_generation);
    return motion_group_status_from_ipc(
        toolbusd::read_ipc_response(socket.get()));
}

MotionGroupTransactionStatus Client::motion_group_cancel(
    std::uint64_t transaction_id, std::uint32_t group_id,
    std::uint32_t plan_generation) const {
    SocketHandle socket(connect_socket(socket_path_));
    toolbusd::write_ipc_motion_group_cancel_request(
        socket.get(), transaction_id, group_id, plan_generation);
    return motion_group_status_from_ipc(
        toolbusd::read_ipc_response(socket.get()));
}

const std::string& Client::socket_path() const noexcept {
    return socket_path_;
}

std::uint32_t Client::node_id() const noexcept { return node_id_; }

}
