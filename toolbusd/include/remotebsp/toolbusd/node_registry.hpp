#pragma once

#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace remotebsp::toolbusd {

enum class NodeUpdate {
    Added,
    Updated,
};

struct NodeRecord {
    protocol::NodeIdentity identity;
    std::uint32_t node_id{};
    bool online{};
    bool assigned{};
    std::chrono::steady_clock::time_point last_seen;
};

class NodeRegistry {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit NodeRegistry(std::chrono::milliseconds offline_timeout =
                              std::chrono::milliseconds(2000));

    protocol::Packet make_discovery_request(
        std::uint32_t request_id,
        std::uint8_t minimum_version = protocol::kProtocolVersion,
        std::uint8_t maximum_version = protocol::kProtocolVersion) const;
    NodeUpdate accept_discovery_response(const protocol::Packet& response,
                                         TimePoint now = Clock::now());
    protocol::Packet make_node_assignment(
        const protocol::NodeUuid& uuid, std::uint32_t node_id,
        std::uint32_t request_id);
    bool mark_assignment_unconfirmed(const protocol::NodeUuid& uuid);
    bool accept_heartbeat(const protocol::Packet& heartbeat,
                          TimePoint now = Clock::now());
    std::vector<protocol::NodeUuid> expire(TimePoint now = Clock::now());

    const NodeRecord* find(const protocol::NodeUuid& uuid) const noexcept;
    const NodeRecord* find_by_node_id(std::uint32_t node_id) const noexcept;
    std::size_t size() const noexcept;
    std::size_t online_count() const noexcept;
    std::vector<NodeRecord> records() const;

private:
    struct UuidHash {
        std::size_t operator()(const protocol::NodeUuid& uuid) const noexcept;
    };

    std::chrono::milliseconds offline_timeout_;
    std::unordered_map<protocol::NodeUuid, NodeRecord, UuidHash> nodes_;
};

}
