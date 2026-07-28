#pragma once

#include "remotebsp/protocol/packet.hpp"

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
};

struct IpcResponse {
    IpcStatus status{IpcStatus::Error};
    std::vector<std::uint8_t> body;
};

struct IpcRequest {
    IpcRequestKind kind{IpcRequestKind::RemotePacket};
    std::uint32_t node_id{};
    protocol::Packet packet;
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

class IpcException : public std::runtime_error {
public:
    explicit IpcException(const std::string& message);
};

void write_ipc_request(int socket, const protocol::Packet& packet,
                       std::uint32_t node_id);
void write_ipc_node_list_request(int socket);
void write_ipc_next_event_request(int socket, std::uint32_t node_id);
IpcRequest read_ipc_request(int socket);

std::vector<std::uint8_t> encode_ipc_node_list(
    const std::vector<IpcNodeInfo>& nodes);
std::vector<IpcNodeInfo> decode_ipc_node_list(
    const std::vector<std::uint8_t>& body);

void write_ipc_response(int socket, IpcStatus status,
                        const std::vector<std::uint8_t>& body);
IpcResponse read_ipc_response(int socket);

}
