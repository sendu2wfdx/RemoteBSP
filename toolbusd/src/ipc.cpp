#include "remotebsp/toolbusd/ipc.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <algorithm>
#include <cstring>
#include <system_error>

namespace remotebsp::toolbusd {
namespace {

constexpr std::size_t kMaximumIpcBodySize = 64U * 1024U;
constexpr std::size_t kNodeInfoSize = 33;

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
        body[0] > static_cast<std::uint8_t>(IpcRequestKind::NextEvent)) {
        throw IpcException("本地 IPC 请求类型无效");
    }
    const auto kind = static_cast<IpcRequestKind>(body[0]);
    if (kind == IpcRequestKind::ListNodes) {
        if (body.size() != 1) {
            throw IpcException("节点列表请求载荷无效");
        }
        return {kind, 0, {}};
    }
    if (kind == IpcRequestKind::NextEvent) {
        if (body.size() != 5) {
            throw IpcException("事件等待请求载荷无效");
        }
        const auto node_id = get_u32(body.data() + 1);
        if (node_id == 0 || node_id > 127) {
            throw IpcException("事件目标节点 ID 必须位于 1～127");
        }
        return {kind, node_id, {}};
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
    return {kind, node_id, protocol::decode(packet)};
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

void write_ipc_response(int socket, IpcStatus status,
                        const std::vector<std::uint8_t>& body) {
    const std::uint8_t status_byte = static_cast<std::uint8_t>(status);
    send_all(socket, &status_byte, 1);
    send_body(socket, body);
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
