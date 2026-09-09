#pragma once

#include "remotebsp/mock_mcu/mock_node.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace remotebsp::mock_mcu {

constexpr std::uint32_t kTransportReplaySchemaVersion = 1U;
constexpr std::size_t kMaximumTransportReplayEvents = 4096U;
constexpr std::size_t kMaximumTransportReplayPendingFrames = 1024U;
constexpr std::size_t kMaximumTransportReplayDeliveries = 32768U;
constexpr std::size_t kMaximumTransportReplayFrameBytes = 4096U;
constexpr std::uint64_t kMaximumTransportReplayDelayMs = 60000U;

enum class TransportReplayAction : std::uint8_t {
    Frame = 1U,
    DelayNext = 2U,
    DropNext = 3U,
    DuplicateNext = 4U,
    NodeReboot = 5U,
};

enum class TransportReplayDirection : std::uint8_t {
    None = 0U,
    HostToNode = 1U,
    NodeToHost = 2U,
};

struct TransportReplayEvent {
    std::uint32_t sequence{};
    std::uint64_t at_ms{};
    std::uint32_t node_id{};
    TransportReplayAction action{TransportReplayAction::Frame};
    TransportReplayDirection direction{TransportReplayDirection::None};
    std::uint64_t delay_ms{};
    std::uint32_t route{};
    std::vector<std::uint8_t> data;
};

struct TransportReplayDelivery {
    std::uint32_t delivery_sequence{};
    std::uint32_t source_sequence{};
    std::uint64_t at_ms{};
    std::uint32_t node_id{};
    std::uint32_t session_generation{};
    TransportReplayDirection direction{TransportReplayDirection::None};
    std::uint32_t route{};
    std::vector<std::uint8_t> data;
};

struct TransportNodeGeneration {
    std::uint32_t node_id{};
    std::uint32_t session_generation{};
};

struct TransportReplayRecord {
    std::uint32_t schema_version{kTransportReplaySchemaVersion};
    std::uint32_t event_count{};
    std::uint32_t dropped_frames{};
    std::uint32_t duplicated_frames{};
    std::uint32_t reboot_invalidated_frames{};
    std::uint32_t reboot_count{};
    std::vector<TransportReplayDelivery> deliveries;
    std::vector<TransportNodeGeneration> final_generations;
    std::string replay_digest;
};

// 纯软件确定性回放模型：故障动作只作用于同节点、同方向的下一输入帧；
// reboot 清除该节点两个方向尚未交付的延迟帧和一次性故障，并推进代次。
TransportReplayRecord run_transport_replay(
    const std::vector<TransportReplayEvent>& events);
void verify_transport_replay(
    const std::vector<TransportReplayEvent>& events,
    const TransportReplayRecord& expected);

// MockNode H2N 入口的最小适配器。默认不会被生产链路启用；调用方必须先通过
// run_transport_replay 得到逻辑交付，再显式交给这里，避免影响 SocketCAN/USB。
std::optional<NodeReply> deliver_replayed_frame_to_mock_node(
    MockNode& node, const TransportReplayDelivery& delivery,
    protocol::Reassembler::TimePoint origin);
std::vector<TransportReplayEvent> make_mock_node_reply_replay_events(
    std::uint32_t first_sequence, std::uint64_t at_ms,
    std::uint32_t node_id, std::uint32_t route, const NodeReply& reply);

}  // namespace remotebsp::mock_mcu
