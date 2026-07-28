#include "remotebsp/toolbusd/request_manager.hpp"

#include <limits>

namespace remotebsp::toolbusd {

RequestManagerException::RequestManagerException(RequestManagerError code,
                                                 const char* message)
    : std::runtime_error(message), code_(code) {}

RequestManagerError RequestManagerException::code() const noexcept {
    return code_;
}

RequestManager::RequestManager(RequestManagerConfig config) : config_(config) {
    if (config_.timeout <= std::chrono::milliseconds::zero() ||
        config_.duplicate_window <= std::chrono::milliseconds::zero()) {
        throw RequestManagerException(
            RequestManagerError::InvalidConfiguration,
            "请求超时和重复检测窗口必须大于零");
    }
}

Submission RequestManager::submit(protocol::Packet request, TimePoint now) {
    expire_completed(now);
    if (request.header.message_type != protocol::MessageType::Request) {
        throw RequestManagerException(RequestManagerError::NotRequest,
                                      "请求管理器只接受请求消息");
    }

    request.header.request_id =
        allocate_request_id(request.header.session_id);
    const auto request_key =
        key(request.header.session_id, request.header.request_id);
    pending_.emplace(
        request_key,
        Pending{request, now + config_.timeout, config_.maximum_retries});
    return {request.header.request_id, std::move(request)};
}

ResponseResult RequestManager::accept_response(
    const protocol::Packet& response, TimePoint now) {
    expire_completed(now);
    if (response.header.message_type != protocol::MessageType::Response) {
        return {ResponseStatus::Unexpected, std::nullopt};
    }

    const auto response_key =
        key(response.header.session_id, response.header.request_id);
    const auto found = pending_.find(response_key);
    if (found == pending_.end()) {
        if (completed_.find(response_key) != completed_.end()) {
            return {ResponseStatus::Duplicate, std::nullopt};
        }
        return {ResponseStatus::Unexpected, std::nullopt};
    }

    const bool creates_object =
        found->second.packet.header.command ==
            static_cast<std::uint16_t>(protocol::Command::GpioCreate) ||
        found->second.packet.header.command ==
            static_cast<std::uint16_t>(protocol::Command::UartCreate);
    const bool response_is_error =
        (response.header.flags & protocol::kErrorResponseFlag) != 0U;
    const bool object_matches =
        creates_object
            ? (response_is_error
                   ? response.header.object_id ==
                         found->second.packet.header.object_id
                   : found->second.packet.header.object_id == 0 &&
                         response.header.object_id != 0)
            : found->second.packet.header.object_id ==
                  response.header.object_id;
    if (found->second.packet.header.command != response.header.command ||
        !object_matches) {
        return {ResponseStatus::Unexpected, std::nullopt};
    }

    pending_.erase(found);
    remember_completed(response_key, now);
    return {ResponseStatus::Matched, response};
}

std::vector<RequestEvent> RequestManager::poll(TimePoint now) {
    expire_completed(now);
    std::vector<RequestEvent> events;
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        Pending& pending = iterator->second;
        if (now < pending.deadline) {
            ++iterator;
            continue;
        }

        if (pending.retries_remaining > 0) {
            --pending.retries_remaining;
            pending.deadline = now + config_.timeout;
            events.push_back({RequestEventType::Retry,
                              pending.packet.header.session_id,
                              pending.packet.header.request_id,
                              pending.packet});
            ++iterator;
            continue;
        }

        const auto request_key = iterator->first;
        const auto request_id = pending.packet.header.request_id;
        iterator = pending_.erase(iterator);
        remember_completed(request_key, now);
        events.push_back({RequestEventType::TimedOut,
                          static_cast<std::uint32_t>(request_key >> 32U),
                          request_id, std::nullopt});
    }
    return events;
}

std::size_t RequestManager::pending_count() const noexcept {
    return pending_.size();
}

std::size_t RequestManager::completed_count() const noexcept {
    return completed_.size();
}

std::uint64_t RequestManager::key(std::uint32_t session_id,
                                  std::uint32_t request_id) noexcept {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

std::uint32_t RequestManager::allocate_request_id(std::uint32_t session_id) {
    constexpr std::uint64_t maximum_attempts =
        std::numeric_limits<std::uint32_t>::max();
    for (std::uint64_t attempt = 0; attempt < maximum_attempts; ++attempt) {
        const std::uint32_t candidate = next_request_id_++;
        if (next_request_id_ == 0) {
            next_request_id_ = 1;
        }
        const auto candidate_key = key(session_id, candidate);
        if (pending_.find(candidate_key) == pending_.end() &&
            completed_.find(candidate_key) == completed_.end()) {
            return candidate;
        }
    }
    throw RequestManagerException(RequestManagerError::RequestIdExhausted,
                                  "请求 ID 已耗尽");
}

void RequestManager::remember_completed(std::uint64_t request_key,
                                        TimePoint now) {
    completed_[request_key] = now + config_.duplicate_window;
}

void RequestManager::expire_completed(TimePoint now) {
    for (auto iterator = completed_.begin(); iterator != completed_.end();) {
        if (now >= iterator->second) {
            iterator = completed_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

}
