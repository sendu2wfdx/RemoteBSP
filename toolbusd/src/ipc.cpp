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
constexpr std::size_t kRuntimeSnapshotHeaderSize = 28U;
constexpr std::size_t kRuntimeResourceSize = 50U;
constexpr std::size_t kRuntimeNodeIssueSize = 8U;
constexpr std::size_t kRuntimeClockQualitySize = 68U;

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
            static_cast<std::uint8_t>(IpcRequestKind::RuntimeSnapshot)) {
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
        snapshot.clocks.size() * kRuntimeClockQualitySize;
    if (total_size > kMaximumIpcBodySize) {
        throw IpcException("Runtime 快照超过本地 IPC 字节上限");
    }
    std::vector<std::uint8_t> body;
    body.reserve(total_size);
    append_u16(body, snapshot.version);
    append_u16(body, 0U);
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
    return body;
}

IpcRuntimeSnapshot decode_ipc_runtime_snapshot(
    const std::vector<std::uint8_t>& body) {
    if (body.size() < kRuntimeSnapshotHeaderSize ||
        get_u16(body.data()) != kRuntimeSnapshotIpcVersion ||
        get_u16(body.data() + 2U) != 0U) {
        throw IpcException("Runtime 快照响应版本或头部无效");
    }
    IpcRuntimeSnapshot snapshot;
    snapshot.version = get_u16(body.data());
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
        static_cast<std::size_t>(clock_count) * kRuntimeClockQualitySize;
    if (snapshot.sequence == 0U || node_count > 127U ||
        resource_count > kMaximumRuntimeSnapshotResources ||
        node_issue_count > 127U || clock_count != node_count ||
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
