#include "remotebsp/toolbusd/node_registry.hpp"

#include <algorithm>
#include <stdexcept>

namespace remotebsp::toolbusd {

NodeRegistry::NodeRegistry(std::chrono::milliseconds offline_timeout)
    : offline_timeout_(offline_timeout) {
    if (offline_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("节点离线超时必须大于零");
    }
}

protocol::Packet NodeRegistry::make_discovery_request(
    std::uint32_t request_id, std::uint8_t minimum_version,
    std::uint8_t maximum_version) const {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryRequest);
    request.header.request_id = request_id;
    request.payload = protocol::encode_discovery_request(
        {minimum_version, maximum_version});
    return request;
}

NodeUpdate NodeRegistry::accept_discovery_response(
    const protocol::Packet& response, TimePoint now) {
    if (response.header.message_type != protocol::MessageType::Response ||
        response.header.command != static_cast<std::uint16_t>(
                                       protocol::Command::DiscoveryResponse)) {
        throw std::invalid_argument("消息不是发现响应");
    }
    const auto identity = protocol::decode_node_identity(response.payload);
    const auto found = nodes_.find(identity.uuid);
    if (found == nodes_.end()) {
        nodes_.emplace(identity.uuid,
                       NodeRecord{identity, 0, true, false, now});
        return NodeUpdate::Added;
    }
    found->second.identity = identity;
    found->second.online = true;
    found->second.last_seen = now;
    return NodeUpdate::Updated;
}

protocol::Packet NodeRegistry::make_node_assignment(
    const protocol::NodeUuid& uuid, std::uint32_t node_id,
    std::uint32_t request_id) {
    const auto found = nodes_.find(uuid);
    if (found == nodes_.end()) {
        throw std::invalid_argument("不能为未知节点分配 ID");
    }
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command =
        static_cast<std::uint16_t>(protocol::Command::NodeAssign);
    request.header.request_id = request_id;
    request.payload =
        protocol::encode_node_assignment({uuid, node_id});
    found->second.node_id = node_id;
    found->second.assigned = false;
    return request;
}

bool NodeRegistry::mark_assignment_unconfirmed(
    const protocol::NodeUuid& uuid) {
    const auto found = nodes_.find(uuid);
    if (found == nodes_.end()) {
        return false;
    }
    found->second.assigned = false;
    return true;
}

bool NodeRegistry::accept_heartbeat(const protocol::Packet& heartbeat,
                                    TimePoint now) {
    if (heartbeat.header.message_type != protocol::MessageType::Event ||
        heartbeat.header.command !=
            static_cast<std::uint16_t>(protocol::Command::Heartbeat)) {
        return false;
    }
    const auto decoded = protocol::decode_heartbeat(heartbeat.payload);
    const auto found = nodes_.find(decoded.uuid);
    if (found == nodes_.end() ||
        found->second.identity.protocol_version !=
            decoded.protocol_version) {
        return false;
    }
    found->second.online = true;
    found->second.last_seen = now;
    if (heartbeat.header.object_id != 0) {
        found->second.node_id = heartbeat.header.object_id;
        found->second.assigned = true;
    }
    return true;
}

std::vector<protocol::NodeUuid> NodeRegistry::expire(TimePoint now) {
    std::vector<protocol::NodeUuid> offline;
    for (auto& entry : nodes_) {
        NodeRecord& node = entry.second;
        if (node.online && now - node.last_seen >= offline_timeout_) {
            node.online = false;
            offline.push_back(entry.first);
        }
    }
    return offline;
}

const NodeRecord* NodeRegistry::find(
    const protocol::NodeUuid& uuid) const noexcept {
    const auto found = nodes_.find(uuid);
    return found == nodes_.end() ? nullptr : &found->second;
}

const NodeRecord* NodeRegistry::find_by_node_id(
    std::uint32_t node_id) const noexcept {
    for (const auto& entry : nodes_) {
        if (entry.second.node_id == node_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::size_t NodeRegistry::size() const noexcept { return nodes_.size(); }

std::size_t NodeRegistry::online_count() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : nodes_) {
        if (entry.second.online) {
            ++count;
        }
    }
    return count;
}

std::vector<NodeRecord> NodeRegistry::records() const {
    std::vector<NodeRecord> result;
    result.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const NodeRecord& left, const NodeRecord& right) {
                  return left.node_id < right.node_id;
              });
    return result;
}

std::size_t NodeRegistry::UuidHash::operator()(
    const protocol::NodeUuid& uuid) const noexcept {
    std::size_t hash = 1469598103934665603ULL;
    for (const auto byte : uuid) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}
