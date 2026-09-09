#pragma once

#include "remotebsp/protocol/time_sync.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace remotebsp::toolbusd {

struct ClockSyncManagerConfig {
    std::size_t maximum_pending_requests{128U};
    ClockModelConfig clock_model_config{};
};

enum class ClockSyncManagerError : std::uint8_t {
    InvalidConfiguration = 0,
    InvalidNodeId,
    CapacityReached,
    NodeAlreadyPending,
};

class ClockSyncManagerException : public std::runtime_error {
public:
    ClockSyncManagerException(ClockSyncManagerError code,
                              const char* message);
    ClockSyncManagerError code() const noexcept;

private:
    ClockSyncManagerError code_;
};

enum class ClockSyncResponseStatus : std::uint8_t {
    Accepted = 0,
    Duplicate,
    Unexpected,
    PendingMissing,
    SendTimeMissing,
    NodeRouteMismatch,
    RemoteError,
    InvalidPayload,
    HostTimeWentBackwards,
    RegistrationRejected,
    SampleRejected,
};

enum class ClockSyncMarkSentResult : std::uint8_t {
    Recorded = 0,
    AlreadyRecorded,
    UnknownRequest,
    InvalidHostTime,
};

struct ClockSyncResponseOutcome {
    ClockSyncResponseStatus status{ClockSyncResponseStatus::Unexpected};
    std::optional<protocol::TimeSyncResponsePayload> payload;
    std::optional<NodeClockRegistrationResult> registration_result;
    std::optional<NodeClockSampleOutcome> sample_outcome;
};

class ClockSyncManager {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit ClockSyncManager(ClockSyncManagerConfig config = {});

    Submission submit(RequestManager& requests, std::uint32_t node_id,
                      std::uint32_t session_id,
                      TimePoint request_registered_at = Clock::now());
    ClockSyncMarkSentResult mark_sent(
        std::uint32_t session_id, std::uint32_t request_id,
        TimePoint host_send_time = Clock::now()) noexcept;
    ClockSyncResponseOutcome accept_response(
        RequestManager& requests, NodeRegistry& nodes,
        const protocol::Packet& response, std::uint32_t response_node_id,
        TimePoint host_receive_time = Clock::now());

    bool cancel(RequestManager& requests, std::uint32_t session_id,
                std::uint32_t request_id) noexcept;
    std::size_t cancel_node(RequestManager& requests,
                            std::uint32_t node_id) noexcept;
    bool has_pending_node(std::uint32_t node_id) const noexcept;
    std::size_t pending_count() const noexcept;

private:
    struct PendingSync {
        std::uint32_t node_id{};
        std::optional<std::uint64_t> host_send_ns;
    };

    static std::uint64_t key(std::uint32_t session_id,
                             std::uint32_t request_id) noexcept;
    static std::optional<std::uint64_t> host_time_ns(
        TimePoint time) noexcept;

    ClockSyncManagerConfig config_;
    std::unordered_map<std::uint64_t, PendingSync> pending_;
};

}
