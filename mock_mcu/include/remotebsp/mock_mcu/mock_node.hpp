#pragma once

#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/fragmentation.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

struct NodeReply {
    std::uint16_t transfer_id{};
    std::vector<std::vector<std::uint8_t>> frames;
};

enum class MockNodeError {
    InvalidCacheSize,
    ConflictingDuplicateRequest,
};

class MockNodeException : public std::runtime_error {
public:
    MockNodeException(MockNodeError code, const char* message);
    MockNodeError code() const noexcept;

private:
    MockNodeError code_;
};

class MockNode {
public:
    MockNode(RemoteCore core, std::size_t mtu, std::uint32_t node_id,
             std::chrono::milliseconds reassembly_timeout =
                 std::chrono::milliseconds(500),
             std::size_t maximum_cached_requests = 128);

    std::optional<NodeReply> handle_frame(
        std::uint32_t stream_id, const std::vector<std::uint8_t>& frame,
        protocol::Reassembler::TimePoint now =
            protocol::Reassembler::Clock::now());

    NodeReply make_heartbeat();
    std::vector<NodeReply> poll_uart_events(
        std::size_t maximum_payload = 64);
    std::uint16_t allocate_transfer_id();
    std::size_t expire(protocol::Reassembler::TimePoint now =
                           protocol::Reassembler::Clock::now());
    std::size_t cached_request_count() const noexcept;
    std::uint64_t executed_request_count() const noexcept;
    std::uint64_t cached_response_count() const noexcept;
    std::uint32_t node_id() const noexcept;
    std::size_t clear_session(std::uint32_t session_id);

private:
    struct CachedRequest {
        protocol::Packet request;
        std::vector<std::uint8_t> encoded_response;
    };

    static std::uint64_t request_key(std::uint32_t session_id,
                                     std::uint32_t request_id) noexcept;
    static bool same_request(const protocol::Packet& left,
                             const protocol::Packet& right) noexcept;
    std::optional<NodeReply> process_request(
        const protocol::Packet& request,
        protocol::Reassembler::TimePoint now);
    void insert_cache(std::uint64_t key, CachedRequest entry);

    RemoteCore core_;
    protocol::Fragmenter fragmenter_;
    protocol::Reassembler reassembler_;
    std::uint32_t node_id_;
    std::size_t maximum_cached_requests_;
    std::uint16_t next_outbound_transfer_id_{1};
    std::unordered_map<std::uint64_t, CachedRequest> cache_;
    std::deque<std::uint64_t> cache_order_;
    std::uint64_t executed_request_count_{};
    std::uint64_t cached_response_count_{};
};

}
