#pragma once

#include "remotebsp/protocol/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::toolbusd {

struct RequestManagerConfig {
    /*
     * 最大包在 500 kbit/s Classical CAN + SLCAN 上实测接近 450 ms，
     * 在 1 Mbit/s + 2 Mbaud SLCAN 上约 364 ms。保留调度和总线竞争
     * 余量，避免合法响应触发过早重试。
     */
    std::chrono::milliseconds timeout{750};
    unsigned maximum_retries{2};
    std::chrono::milliseconds duplicate_window{2000};
};

enum class RequestManagerError {
    InvalidConfiguration,
    NotRequest,
    RequestIdExhausted,
};

class RequestManagerException : public std::runtime_error {
public:
    RequestManagerException(RequestManagerError code, const char* message);
    RequestManagerError code() const noexcept;

private:
    RequestManagerError code_;
};

struct Submission {
    std::uint32_t request_id{};
    protocol::Packet packet;
};

enum class RequestEventType {
    Retry,
    TimedOut,
};

struct RequestEvent {
    RequestEventType type{RequestEventType::Retry};
    std::uint32_t session_id{};
    std::uint32_t request_id{};
    std::optional<protocol::Packet> packet;
};

enum class ResponseStatus {
    Matched,
    Duplicate,
    Unexpected,
};

struct ResponseResult {
    ResponseStatus status{ResponseStatus::Unexpected};
    std::optional<protocol::Packet> response;
};

class RequestManager {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit RequestManager(RequestManagerConfig config = {});

    Submission submit(protocol::Packet request,
                      TimePoint now = Clock::now());
    ResponseResult accept_response(const protocol::Packet& response,
                                   TimePoint now = Clock::now());
    std::vector<RequestEvent> poll(TimePoint now = Clock::now());

    std::size_t pending_count() const noexcept;
    std::size_t completed_count() const noexcept;

private:
    struct Pending {
        protocol::Packet packet;
        TimePoint deadline;
        unsigned retries_remaining{};
    };

    static std::uint64_t key(std::uint32_t session_id,
                             std::uint32_t request_id) noexcept;
    std::uint32_t allocate_request_id(std::uint32_t session_id);
    void remember_completed(std::uint64_t request_key, TimePoint now);
    void expire_completed(TimePoint now);

    RequestManagerConfig config_;
    std::uint32_t next_request_id_{1};
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::unordered_map<std::uint64_t, TimePoint> completed_;
};

}
