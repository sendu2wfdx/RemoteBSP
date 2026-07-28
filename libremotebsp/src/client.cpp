#include "remotebsp/client.hpp"

#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>
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

int connect_socket(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        throw ClientException("Unix Domain Socket 路径无效或过长");
    }
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "创建本地 IPC 套接字失败");
    }
    const timeval timeout{3, 0};
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

}

RemoteException::RemoteException(std::uint8_t status,
                                 const std::string& message)
    : ClientException(message), status_(status) {}

std::uint8_t RemoteException::status() const noexcept { return status_; }

Client::Client(std::string socket_path, std::uint32_t node_id)
    : socket_path_(std::move(socket_path)), node_id_(node_id) {
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

std::uint32_t Client::uart_create(const UartConfig& config) const {
    const auto parity = static_cast<std::uint8_t>(config.parity);
    if (config.baud_rate == 0 || config.data_bits < 5 ||
        config.data_bits > 8 ||
        (config.stop_bits != 1 && config.stop_bits != 2) ||
        parity > static_cast<std::uint8_t>(UartParity::Even)) {
        throw ClientException("UART 配置参数无效");
    }
    std::vector<std::uint8_t> payload{config.port};
    append_u32(payload, config.baud_rate);
    payload.push_back(config.data_bits);
    payload.push_back(config.stop_bits);
    payload.push_back(parity);
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

const std::string& Client::socket_path() const noexcept {
    return socket_path_;
}

std::uint32_t Client::node_id() const noexcept { return node_id_; }

}
