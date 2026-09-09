#include "remotebsp/toolbusd/clock_sync_manager.hpp"

#include <utility>
#include <vector>

namespace remotebsp::toolbusd {

ClockSyncManagerException::ClockSyncManagerException(
    ClockSyncManagerError code, const char* message)
    : std::runtime_error(message), code_(code) {}

ClockSyncManagerError ClockSyncManagerException::code() const noexcept {
    return code_;
}

ClockSyncManager::ClockSyncManager(ClockSyncManagerConfig config)
    : config_(std::move(config)) {
    if (config_.maximum_pending_requests == 0U) {
        throw ClockSyncManagerException(
            ClockSyncManagerError::InvalidConfiguration,
            "时钟同步待处理请求容量必须大于零");
    }
    /* 提前验证除节点报告频率和位宽外的基础模型配置。 */
    static_cast<void>(ClockModel(config_.clock_model_config));
    pending_.reserve(config_.maximum_pending_requests);
}

Submission ClockSyncManager::submit(RequestManager& requests,
                                    std::uint32_t node_id,
                                    std::uint32_t session_id,
                                    TimePoint request_registered_at) {
    if (node_id == 0U) {
        throw ClockSyncManagerException(
            ClockSyncManagerError::InvalidNodeId,
            "时钟同步节点 ID 不能为零");
    }
    if (has_pending_node(node_id)) {
        throw ClockSyncManagerException(
            ClockSyncManagerError::NodeAlreadyPending,
            "同一节点已有时钟同步请求等待响应");
    }
    if (pending_.size() >= config_.maximum_pending_requests) {
        throw ClockSyncManagerException(
            ClockSyncManagerError::CapacityReached,
            "时钟同步待处理请求达到容量上限");
    }

    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command =
        static_cast<std::uint16_t>(protocol::Command::TimeSync);
    request.header.session_id = session_id;
    request.header.object_id = node_id;
    request.payload = protocol::encode_time_sync_request();
    auto submission =
        requests.submit(std::move(request), request_registered_at);
    try {
        pending_.emplace(
            key(submission.packet.header.session_id,
                submission.packet.header.request_id),
            PendingSync{node_id, std::nullopt});
    } catch (...) {
        requests.cancel(submission.packet.header.session_id,
                        submission.packet.header.request_id);
        throw;
    }
    return submission;
}

ClockSyncMarkSentResult ClockSyncManager::mark_sent(
    std::uint32_t session_id, std::uint32_t request_id,
    TimePoint host_send_time) noexcept {
    const auto pending = pending_.find(key(session_id, request_id));
    if (pending == pending_.end()) {
        return ClockSyncMarkSentResult::UnknownRequest;
    }
    if (pending->second.host_send_ns.has_value()) {
        return ClockSyncMarkSentResult::AlreadyRecorded;
    }
    const auto send_ns = host_time_ns(host_send_time);
    if (!send_ns.has_value()) {
        return ClockSyncMarkSentResult::InvalidHostTime;
    }
    pending->second.host_send_ns = *send_ns;
    return ClockSyncMarkSentResult::Recorded;
}

ClockSyncResponseOutcome ClockSyncManager::accept_response(
    RequestManager& requests, NodeRegistry& nodes,
    const protocol::Packet& response, std::uint32_t response_node_id,
    TimePoint host_receive_time) {
    const auto matched = requests.accept_response(response, host_receive_time);
    if (matched.status == ResponseStatus::Duplicate) {
        return {ClockSyncResponseStatus::Duplicate, std::nullopt,
                std::nullopt, std::nullopt};
    }
    if (matched.status != ResponseStatus::Matched ||
        !matched.response.has_value()) {
        return {ClockSyncResponseStatus::Unexpected, std::nullopt,
                std::nullopt, std::nullopt};
    }

    const auto request_key =
        key(response.header.session_id, response.header.request_id);
    const auto pending = pending_.find(request_key);
    if (pending == pending_.end()) {
        return {ClockSyncResponseStatus::PendingMissing, std::nullopt,
                std::nullopt, std::nullopt};
    }
    const auto saved = pending->second;
    pending_.erase(pending);
    if (response_node_id != saved.node_id ||
        response.header.object_id != saved.node_id) {
        return {ClockSyncResponseStatus::NodeRouteMismatch, std::nullopt,
                std::nullopt, std::nullopt};
    }
    if (!saved.host_send_ns.has_value()) {
        return {ClockSyncResponseStatus::SendTimeMissing, std::nullopt,
                std::nullopt, std::nullopt};
    }
    const auto receive_ns = host_time_ns(host_receive_time);
    if (!receive_ns.has_value() ||
        *receive_ns < *saved.host_send_ns) {
        return {ClockSyncResponseStatus::HostTimeWentBackwards,
                std::nullopt, std::nullopt, std::nullopt};
    }
    if ((response.header.flags & protocol::kErrorResponseFlag) != 0U ||
        response.payload.empty() || response.payload.front() != 0U) {
        return {ClockSyncResponseStatus::RemoteError, std::nullopt,
                std::nullopt, std::nullopt};
    }

    protocol::TimeSyncResponsePayload decoded;
    try {
        const std::vector<std::uint8_t> body(
            response.payload.begin() + 1U, response.payload.end());
        decoded = protocol::decode_time_sync_response(body);
    } catch (const protocol::TimeSyncPayloadException&) {
        return {ClockSyncResponseStatus::InvalidPayload, std::nullopt,
                std::nullopt, std::nullopt};
    }

    auto model_config = config_.clock_model_config;
    model_config.nominal_tick_rate_hz = decoded.nominal_tick_rate_hz;
    model_config.node_counter_bits = decoded.counter_bits;
    const auto registration = nodes.register_clock_model(
        saved.node_id, decoded.boot_epoch, model_config);
    if (registration != NodeClockRegistrationResult::Registered &&
        registration != NodeClockRegistrationResult::AlreadyRegistered &&
        registration != NodeClockRegistrationResult::ReplacedBootEpoch) {
        return {ClockSyncResponseStatus::RegistrationRejected, decoded,
                registration, std::nullopt};
    }

    const auto sample = nodes.add_clock_sample(
        saved.node_id, decoded.boot_epoch,
        {*saved.host_send_ns, decoded.node_receive_tick,
         decoded.node_send_tick, *receive_ns});
    if (sample.status != NodeClockAccessStatus::Ready ||
        sample.sample_result != ClockSampleResult::Accepted) {
        return {ClockSyncResponseStatus::SampleRejected, decoded,
                registration, sample};
    }
    return {ClockSyncResponseStatus::Accepted, decoded, registration,
            sample};
}

bool ClockSyncManager::cancel(RequestManager& requests,
                              std::uint32_t session_id,
                              std::uint32_t request_id) noexcept {
    const bool sync_removed = pending_.erase(key(session_id, request_id)) != 0U;
    const bool request_removed = requests.cancel(session_id, request_id);
    return sync_removed || request_removed;
}

std::size_t ClockSyncManager::cancel_node(
    RequestManager& requests, std::uint32_t node_id) noexcept {
    std::size_t removed = 0U;
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->second.node_id != node_id) {
            ++iterator;
            continue;
        }
        const auto request_key = iterator->first;
        static_cast<void>(requests.cancel(
            static_cast<std::uint32_t>(request_key >> 32U),
            static_cast<std::uint32_t>(request_key)));
        iterator = pending_.erase(iterator);
        ++removed;
    }
    return removed;
}

bool ClockSyncManager::has_pending_node(
    std::uint32_t node_id) const noexcept {
    for (const auto& entry : pending_) {
        if (entry.second.node_id == node_id) {
            return true;
        }
    }
    return false;
}

std::size_t ClockSyncManager::pending_count() const noexcept {
    return pending_.size();
}

std::uint64_t ClockSyncManager::key(std::uint32_t session_id,
                                    std::uint32_t request_id) noexcept {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

std::optional<std::uint64_t> ClockSyncManager::host_time_ns(
    TimePoint time) noexcept {
    const auto count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            time.time_since_epoch())
            .count();
    if (count < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(count);
}

}
