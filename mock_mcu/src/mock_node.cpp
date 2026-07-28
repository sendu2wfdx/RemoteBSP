#include "remotebsp/mock_mcu/mock_node.hpp"

#include "remotebsp/protocol/discovery.hpp"

#include <algorithm>
#include <utility>

namespace remotebsp::mock_mcu {

MockNodeException::MockNodeException(MockNodeError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MockNodeError MockNodeException::code() const noexcept { return code_; }

MockNode::MockNode(RemoteCore core, std::size_t mtu, std::uint32_t node_id,
                   std::chrono::milliseconds reassembly_timeout,
                   std::size_t maximum_cached_requests)
    : core_(std::move(core)),
      fragmenter_(mtu),
      reassembler_(mtu, reassembly_timeout),
      node_id_(node_id),
      maximum_cached_requests_(maximum_cached_requests) {
    if (maximum_cached_requests_ == 0) {
        throw MockNodeException(MockNodeError::InvalidCacheSize,
                                "重复请求缓存容量必须大于零");
    }
}

std::optional<NodeReply> MockNode::handle_frame(
    std::uint32_t stream_id, const std::vector<std::uint8_t>& frame,
    protocol::Reassembler::TimePoint now) {
    protocol::ReassemblyResult result =
        reassembler_.accept(stream_id, frame, now);
    const bool completed =
        result.status == protocol::ReassemblyStatus::Complete;
    const bool repeated_last =
        result.status == protocol::ReassemblyStatus::Duplicate &&
        (protocol::decode_fragment(frame, fragmenter_.mtu()).flags &
         protocol::kFragmentLast) != 0U;
    if ((!completed && !repeated_last) || !result.packet.has_value()) {
        return std::nullopt;
    }

    const protocol::Packet request = protocol::decode(*result.packet);
    return process_request(request);
}

NodeReply MockNode::make_heartbeat() {
    protocol::Packet heartbeat;
    heartbeat.header.message_type = protocol::MessageType::Event;
    heartbeat.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    heartbeat.header.object_id = node_id_;
    heartbeat.payload = protocol::encode_heartbeat(
        {core_.node_info().uuid, core_.node_info().protocol_version});

    const std::uint16_t transfer_id = allocate_transfer_id();
    NodeReply reply;
    reply.transfer_id = transfer_id;
    reply.frames =
        fragmenter_.split(protocol::encode(heartbeat), transfer_id);
    return reply;
}

std::uint16_t MockNode::allocate_transfer_id() {
    const std::uint16_t result = next_outbound_transfer_id_++;
    if (next_outbound_transfer_id_ == 0U) {
        next_outbound_transfer_id_ = 1U;
    }
    return result;
}

std::size_t MockNode::expire(protocol::Reassembler::TimePoint now) {
    return reassembler_.expire(now);
}

std::size_t MockNode::cached_request_count() const noexcept {
    return cache_.size();
}

std::uint64_t MockNode::executed_request_count() const noexcept {
    return executed_request_count_;
}

std::uint64_t MockNode::cached_response_count() const noexcept {
    return cached_response_count_;
}

std::uint32_t MockNode::node_id() const noexcept { return node_id_; }

std::size_t MockNode::clear_session(std::uint32_t session_id) {
    std::size_t removed = 0;
    for (auto iterator = cache_.begin(); iterator != cache_.end();) {
        if (static_cast<std::uint32_t>(iterator->first >> 32U) == session_id) {
            iterator = cache_.erase(iterator);
            ++removed;
        } else {
            ++iterator;
        }
    }
    cache_order_.erase(
        std::remove_if(cache_order_.begin(), cache_order_.end(),
                       [&](std::uint64_t key) {
                           return static_cast<std::uint32_t>(key >> 32U) ==
                                  session_id;
                       }),
        cache_order_.end());
    return removed;
}

std::uint64_t MockNode::request_key(std::uint32_t session_id,
                                    std::uint32_t request_id) noexcept {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

bool MockNode::same_request(const protocol::Packet& left,
                            const protocol::Packet& right) noexcept {
    return left.header.version == right.header.version &&
           left.header.message_type == right.header.message_type &&
           left.header.command == right.header.command &&
           left.header.session_id == right.header.session_id &&
           left.header.request_id == right.header.request_id &&
           left.header.object_id == right.header.object_id &&
           left.header.flags == right.header.flags &&
           left.payload == right.payload;
}

std::optional<NodeReply> MockNode::process_request(
    const protocol::Packet& request) {
    const auto key =
        request_key(request.header.session_id, request.header.request_id);
    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        if (!same_request(found->second.request, request)) {
            throw MockNodeException(
                MockNodeError::ConflictingDuplicateRequest,
                "相同会话 ID 和请求 ID 对应了不同请求内容");
        }
        ++cached_response_count_;
        const std::uint16_t transfer_id = allocate_transfer_id();
        return NodeReply{
            transfer_id,
            fragmenter_.split(found->second.encoded_response, transfer_id)};
    }

    protocol::Packet response;
    if (request.header.command ==
        static_cast<std::uint16_t>(protocol::Command::DiscoveryRequest)) {
        const auto discovery =
            protocol::decode_discovery_request(request.payload);
        const auto version = core_.node_info().protocol_version;
        if (version < discovery.minimum_protocol_version ||
            version > discovery.maximum_protocol_version) {
            return std::nullopt;
        }
        response.header.version = protocol::kProtocolVersion;
        response.header.message_type = protocol::MessageType::Response;
        response.header.command = static_cast<std::uint16_t>(
            protocol::Command::DiscoveryResponse);
        response.header.session_id = request.header.session_id;
        response.header.request_id = request.header.request_id;
        response.payload = protocol::encode_node_identity(
            {core_.node_info().uuid,
             core_.node_info().firmware_major,
             core_.node_info().firmware_minor,
             core_.node_info().firmware_patch,
             core_.node_info().board_type,
             core_.node_info().protocol_version});
    } else if (request.header.command ==
               static_cast<std::uint16_t>(protocol::Command::NodeAssign)) {
        const auto assignment =
            protocol::decode_node_assignment(request.payload);
        if (assignment.uuid != core_.node_info().uuid) {
            return std::nullopt;
        }
        node_id_ = assignment.node_id;
        response.header.version = protocol::kProtocolVersion;
        response.header.message_type = protocol::MessageType::Response;
        response.header.command = request.header.command;
        response.header.session_id = request.header.session_id;
        response.header.request_id = request.header.request_id;
        response.header.object_id = node_id_;
        response.payload = {0};
    } else {
        response = core_.handle(request);
    }
    std::vector<std::uint8_t> encoded_response = protocol::encode(response);
    ++executed_request_count_;
    insert_cache(key, CachedRequest{request, encoded_response});
    const std::uint16_t transfer_id = allocate_transfer_id();
    return NodeReply{transfer_id,
                     fragmenter_.split(encoded_response, transfer_id)};
}

void MockNode::insert_cache(std::uint64_t key, CachedRequest entry) {
    if (cache_.size() == maximum_cached_requests_) {
        cache_.erase(cache_order_.front());
        cache_order_.pop_front();
    }
    cache_.emplace(key, std::move(entry));
    cache_order_.push_back(key);
}

}
