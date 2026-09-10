#pragma once

#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/transport/link_transport.hpp"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace remotebsp::mock_mcu {

constexpr std::uint32_t kTransportReplaySchemaVersion = 2U;
constexpr std::uint32_t kTransportSessionFileVersion = 1U;
constexpr std::size_t kMaximumTransportReplayEvents = 4096U;
constexpr std::size_t kMaximumTransportReplayPendingFrames = 1024U;
constexpr std::size_t kMaximumTransportReplayDeliveries = 32768U;
constexpr std::size_t kMaximumTransportReplayFrameBytes = 4096U;
constexpr std::uint64_t kMaximumTransportReplayDelayMs = 60000U;
constexpr std::size_t kMaximumTransportSessionFileBytes = 16U * 1024U * 1024U;

enum class TransportReplayAction : std::uint8_t {
    Frame = 1U,
    DelayNext = 2U,
    DropNext = 3U,
    DuplicateNext = 4U,
    NodeReboot = 5U,
    SendFailure = 6U,
    ReceiveEmpty = 7U,
    ReceiveFailure = 8U,
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
    std::uint32_t repeat_count{1U};
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
    std::uint32_t send_failures{};
    std::uint32_t receive_empty_results{};
    std::uint32_t receive_failures{};
    std::vector<TransportReplayDelivery> deliveries;
    std::vector<TransportNodeGeneration> final_generations;
    std::string replay_digest;
};

struct TransportReplaySession {
    std::uint32_t file_version{kTransportSessionFileVersion};
    std::string evidence_scope{"logical-link-boundary-only"};
    std::string time_base{"monotonic-relative-ms"};
    std::vector<TransportReplayEvent> events;
    TransportReplayRecord record;
    std::string file_checksum;
};

using TransportReplayClock = std::function<std::uint64_t()>;

class TransportSessionRecorder {
public:
    explicit TransportSessionRecorder(TransportReplayClock clock = {});

    void record_frame(TransportReplayDirection direction,
                      std::uint32_t node_id,
                      const transport::LinkFrame& frame);
    void record_send_failure(TransportReplayDirection direction,
                             std::uint32_t node_id,
                             const transport::LinkFrame& frame);
    void record_receive_empty(TransportReplayDirection direction,
                              std::uint32_t node_id,
                              std::chrono::milliseconds timeout);
    void record_receive_failure(TransportReplayDirection direction,
                                std::uint32_t node_id,
                                std::chrono::milliseconds timeout);
    void record_fault(std::uint32_t node_id, TransportReplayAction action,
                      TransportReplayDirection direction,
                      std::uint64_t delay_ms = 0U);
    void record_reboot(std::uint32_t node_id);
    TransportReplaySession snapshot() const;

private:
    struct ObservationReservation {
        std::size_t index{static_cast<std::size_t>(-1)};
        std::size_t reserved_payload_bytes{};
        bool active() const noexcept {
            return index != static_cast<std::size_t>(-1);
        }
    };

    enum class CaptureFailure : std::uint8_t {
        None,
        ClockRollback,
        Capacity,
        InvalidObservation,
        Internal,
    };

    void append(TransportReplayEvent event);
    std::uint64_t now_ms() const;
    ObservationReservation reserve_send(
        TransportReplayDirection direction, std::uint32_t node_id,
        const transport::LinkFrame& frame) noexcept;
    ObservationReservation reserve_receive(
        TransportReplayDirection direction, std::uint32_t node_id,
        std::chrono::milliseconds timeout) noexcept;
    void commit_send(ObservationReservation reservation,
                     bool succeeded) noexcept;
    void commit_receive(ObservationReservation reservation,
                        const transport::LinkFrame* frame,
                        std::uint32_t node_id, bool failed) noexcept;

    friend class RecordingLinkTransport;

    mutable std::mutex mutex_;
    TransportReplayClock clock_;
    std::chrono::steady_clock::time_point origin_;
    std::vector<TransportReplayEvent> events_;
    std::size_t payload_bytes_{};
    std::size_t reserved_payload_bytes_{};
    std::size_t pending_observations_{};
    std::uint64_t last_at_ms_{};
    std::atomic<CaptureFailure> capture_failure_{CaptureFailure::None};
};

enum class RecordingTransportRole : std::uint8_t {
    Host = 1U,
    Node = 2U,
};

class RecordingLinkTransport final : public transport::LinkTransport {
public:
    RecordingLinkTransport(
        std::unique_ptr<transport::LinkTransport> inner,
        RecordingTransportRole role,
        std::shared_ptr<TransportSessionRecorder> recorder);

    void send(const transport::LinkFrame& frame) override;
    std::optional<transport::LinkFrame> receive(
        std::chrono::milliseconds timeout) override;
    transport::LinkCapabilities capabilities() const noexcept override;

private:
    std::uint32_t resolve_node(
        TransportReplayDirection direction,
        const transport::LinkFrame* frame) const noexcept;

    std::unique_ptr<transport::LinkTransport> inner_;
    RecordingTransportRole role_;
    std::shared_ptr<TransportSessionRecorder> recorder_;
};

// 纯软件确定性回放模型：故障动作只作用于同节点、同方向的下一输入帧；
// reboot 清除该节点两个方向尚未交付的延迟帧和一次性故障，并推进代次。
TransportReplayRecord run_transport_replay(
    const std::vector<TransportReplayEvent>& events);
void verify_transport_replay(
    const std::vector<TransportReplayEvent>& events,
    const TransportReplayRecord& expected);

TransportReplaySession make_transport_replay_session(
    const std::vector<TransportReplayEvent>& events);
std::string encode_transport_replay_session(
    const TransportReplaySession& session);
TransportReplaySession parse_transport_replay_session(
    const std::string& text);
void write_transport_replay_session(
    const TransportReplaySession& session, const std::string& path);
TransportReplaySession load_transport_replay_session(
    const std::string& path);

// MockNode H2N 入口的最小适配器。默认不会被生产链路启用；调用方必须先通过
// run_transport_replay 得到逻辑交付，再显式交给这里，避免影响 SocketCAN/USB。
std::optional<NodeReply> deliver_replayed_frame_to_mock_node(
    MockNode& node, const TransportReplayDelivery& delivery,
    protocol::Reassembler::TimePoint origin);
std::vector<TransportReplayEvent> make_mock_node_reply_replay_events(
    std::uint32_t first_sequence, std::uint64_t at_ms,
    std::uint32_t node_id, std::uint32_t route, const NodeReply& reply);

}  // namespace remotebsp::mock_mcu
