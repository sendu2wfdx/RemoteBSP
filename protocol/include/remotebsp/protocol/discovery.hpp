#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

using NodeUuid = std::array<std::uint8_t, 16>;

struct DiscoveryRequestPayload {
    std::uint8_t minimum_protocol_version{};
    std::uint8_t maximum_protocol_version{};
};

struct NodeIdentity {
    NodeUuid uuid{};
    std::uint16_t firmware_major{};
    std::uint16_t firmware_minor{};
    std::uint16_t firmware_patch{};
    std::uint32_t board_type{};
    std::uint8_t protocol_version{};
};

struct NodeAssignment {
    NodeUuid uuid{};
    std::uint32_t node_id{};
};

struct HeartbeatPayload {
    NodeUuid uuid{};
    std::uint8_t protocol_version{};
};

enum class DiscoveryPayloadError {
    InvalidLength,
    InvalidVersionRange,
    InvalidNodeId,
};

class DiscoveryPayloadException : public std::runtime_error {
public:
    DiscoveryPayloadException(DiscoveryPayloadError code,
                              const char* message);
    DiscoveryPayloadError code() const noexcept;

private:
    DiscoveryPayloadError code_;
};

std::vector<std::uint8_t> encode_discovery_request(
    const DiscoveryRequestPayload& payload);
DiscoveryRequestPayload decode_discovery_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_node_identity(const NodeIdentity& identity);
NodeIdentity decode_node_identity(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_node_assignment(
    const NodeAssignment& assignment);
NodeAssignment decode_node_assignment(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_heartbeat(
    const HeartbeatPayload& heartbeat);
HeartbeatPayload decode_heartbeat(const std::vector<std::uint8_t>& payload);

}
