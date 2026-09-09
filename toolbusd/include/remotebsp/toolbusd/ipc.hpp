#pragma once

#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"

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
};

constexpr std::uint16_t kRuntimeSnapshotIpcVersion = 1U;
constexpr std::uint16_t kMaximumRuntimeSnapshotResources = 128U;
constexpr std::uint32_t kMaximumRuntimeSnapshotTimeoutMs = 5000U;

struct IpcResponse {
    IpcStatus status{IpcStatus::Error};
    std::vector<std::uint8_t> body;
};

struct IpcRequest {
    IpcRequestKind kind{IpcRequestKind::RemotePacket};
    std::uint32_t node_id{};
    protocol::Packet packet;
    std::uint32_t object_id{};
    std::uint32_t maximum_length{};
    std::uint32_t timeout_ms{};
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

struct IpcRuntimeSnapshot {
    std::uint16_t version{kRuntimeSnapshotIpcVersion};
    std::uint64_t sequence{};
    std::vector<IpcNodeInfo> nodes;
    TrafficSnapshot traffic;
    std::vector<IpcRuntimeResource> resources;
    std::vector<IpcRuntimeNodeIssue> node_issues;
};

class IpcException : public std::runtime_error {
public:
    explicit IpcException(const std::string& message);
};

void write_ipc_request(int socket, const protocol::Packet& packet,
                       std::uint32_t node_id);
void write_ipc_node_list_request(int socket);
void write_ipc_next_event_request(int socket, std::uint32_t node_id);
void write_ipc_traffic_status_request(int socket);
void write_ipc_uart_stream_read_request(
    int socket, std::uint32_t node_id, std::uint32_t object_id,
    std::uint32_t maximum_length, std::uint32_t timeout_ms);
void write_ipc_runtime_snapshot_request(
    int socket, std::uint16_t maximum_resources,
    std::uint32_t timeout_ms);
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

void write_ipc_response(int socket, IpcStatus status,
                        const std::vector<std::uint8_t>& body);
IpcResponse read_ipc_response(int socket);

}
