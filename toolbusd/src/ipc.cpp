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
constexpr std::size_t kTrafficStatusHeaderSize = 76;
constexpr std::size_t kTrafficClassCounterSize = 32;

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
        body[0] >
            static_cast<std::uint8_t>(IpcRequestKind::UartStreamRead)) {
        throw IpcException("本地 IPC 请求类型无效");
    }
    const auto kind = static_cast<IpcRequestKind>(body[0]);
    if (kind == IpcRequestKind::ListNodes ||
        kind == IpcRequestKind::TrafficStatus) {
        if (body.size() != 1) {
            throw IpcException("本地状态请求载荷无效");
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
        snapshot.mode > TrafficBusMode::CanFd) {
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
        body[2] > static_cast<std::uint8_t>(TrafficBusMode::CanFd) ||
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
