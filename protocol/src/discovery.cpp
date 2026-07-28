#include "remotebsp/protocol/discovery.hpp"

#include <algorithm>

namespace remotebsp::protocol {
namespace {

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

}

DiscoveryPayloadException::DiscoveryPayloadException(
    DiscoveryPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

DiscoveryPayloadError DiscoveryPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_discovery_request(
    const DiscoveryRequestPayload& payload) {
    if (payload.minimum_protocol_version == 0 ||
        payload.minimum_protocol_version > payload.maximum_protocol_version) {
        throw DiscoveryPayloadException(
            DiscoveryPayloadError::InvalidVersionRange,
            "发现请求的协议版本范围无效");
    }
    return {payload.minimum_protocol_version,
            payload.maximum_protocol_version};
}

DiscoveryRequestPayload decode_discovery_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 2) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidLength,
                                        "发现请求载荷长度无效");
    }
    DiscoveryRequestPayload decoded{payload[0], payload[1]};
    encode_discovery_request(decoded);
    return decoded;
}

std::vector<std::uint8_t> encode_node_identity(
    const NodeIdentity& identity) {
    std::vector<std::uint8_t> output;
    output.reserve(27);
    output.insert(output.end(), identity.uuid.begin(), identity.uuid.end());
    append_u16(output, identity.firmware_major);
    append_u16(output, identity.firmware_minor);
    append_u16(output, identity.firmware_patch);
    append_u32(output, identity.board_type);
    output.push_back(identity.protocol_version);
    return output;
}

NodeIdentity decode_node_identity(const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 27) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidLength,
                                        "节点信息载荷长度无效");
    }
    NodeIdentity identity;
    std::copy_n(payload.begin(), identity.uuid.size(),
                identity.uuid.begin());
    identity.firmware_major = read_u16(payload.data() + 16);
    identity.firmware_minor = read_u16(payload.data() + 18);
    identity.firmware_patch = read_u16(payload.data() + 20);
    identity.board_type = read_u32(payload.data() + 22);
    identity.protocol_version = payload[26];
    return identity;
}

std::vector<std::uint8_t> encode_node_assignment(
    const NodeAssignment& assignment) {
    if (assignment.node_id == 0) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidNodeId,
                                        "节点 ID 不能为零");
    }
    std::vector<std::uint8_t> output;
    output.reserve(20);
    output.insert(output.end(), assignment.uuid.begin(),
                  assignment.uuid.end());
    append_u32(output, assignment.node_id);
    return output;
}

NodeAssignment decode_node_assignment(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 20) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidLength,
                                        "节点分配载荷长度无效");
    }
    NodeAssignment assignment;
    std::copy_n(payload.begin(), assignment.uuid.size(),
                assignment.uuid.begin());
    assignment.node_id = read_u32(payload.data() + 16);
    if (assignment.node_id == 0) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidNodeId,
                                        "节点 ID 不能为零");
    }
    return assignment;
}

std::vector<std::uint8_t> encode_heartbeat(
    const HeartbeatPayload& heartbeat) {
    std::vector<std::uint8_t> output;
    output.reserve(17);
    output.insert(output.end(), heartbeat.uuid.begin(), heartbeat.uuid.end());
    output.push_back(heartbeat.protocol_version);
    return output;
}

HeartbeatPayload decode_heartbeat(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 17) {
        throw DiscoveryPayloadException(DiscoveryPayloadError::InvalidLength,
                                        "心跳载荷长度无效");
    }
    HeartbeatPayload heartbeat;
    std::copy_n(payload.begin(), heartbeat.uuid.size(),
                heartbeat.uuid.begin());
    heartbeat.protocol_version = payload[16];
    return heartbeat;
}

}
