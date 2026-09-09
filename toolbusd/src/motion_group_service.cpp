#include "remotebsp/toolbusd/motion_group_service.hpp"

#include <algorithm>

namespace remotebsp::toolbusd {
namespace {

bool is_transaction_active(MotionGroupState state) noexcept {
    return state == MotionGroupState::Preparing ||
           state == MotionGroupState::Ready ||
           state == MotionGroupState::Committing;
}

protocol::MotionGroupAbortReason failure_reason(
    MotionGroupActionPhase phase) noexcept {
    return phase == MotionGroupActionPhase::Commit
               ? protocol::MotionGroupAbortReason::CommitRejected
               : protocol::MotionGroupAbortReason::PrepareRejected;
}

protocol::MotionGroupAbortReason timeout_reason(
    MotionGroupActionPhase phase) noexcept {
    return phase == MotionGroupActionPhase::Commit
               ? protocol::MotionGroupAbortReason::CommitTimedOut
               : protocol::MotionGroupAbortReason::PrepareTimedOut;
}

protocol::Command expected_command(MotionGroupActionPhase phase) {
    switch (phase) {
        case MotionGroupActionPhase::Prepare:
            return protocol::Command::MotionGroupPrepare;
        case MotionGroupActionPhase::Commit:
            return protocol::Command::MotionGroupCommit;
        case MotionGroupActionPhase::Abort:
            return protocol::Command::MotionGroupAbort;
    }
    throw MotionGroupServiceException(
        MotionGroupServiceError::InvalidAction,
        "未知运动组动作阶段");
}

}

MotionGroupServiceException::MotionGroupServiceException(
    MotionGroupServiceError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MotionGroupServiceError MotionGroupServiceException::code() const noexcept {
    return code_;
}

MotionGroupService::MotionGroupService(MotionGroupServiceConfig config)
    : config_(config), coordinator_(config.coordinator) {
    if (config_.maximum_pending_routes == 0U ||
        config_.maximum_completed_routes == 0U ||
        config_.maximum_pending_routes < config_.coordinator.maximum_members) {
        throw MotionGroupServiceException(
            MotionGroupServiceError::InvalidConfiguration,
            "运动组服务路由容量必须覆盖协调器最大成员数");
    }
}

MotionGroupServiceOutcome MotionGroupService::start(
    RequestManager& requests, const MotionGroupPlan& plan,
    const NodeRegistry& registry, std::uint32_t session_id,
    std::uint64_t host_now_ns, TimePoint now) {
    if (session_id == 0U) {
        throw MotionGroupServiceException(
            MotionGroupServiceError::InvalidSession,
            "运动组服务会话 ID 不能为零");
    }
    const auto actions = coordinator_.begin(
        plan, registry, host_now_ns, now);
    session_id_ = session_id;
    try {
        return {MotionGroupServiceStatus::Started,
                submit_actions(requests, actions, now), {}};
    } catch (...) {
        cancel_routes(requests);
        static_cast<void>(coordinator_.cancel(now));
        throw;
    }
}

MotionGroupServiceOutcome MotionGroupService::accept_response(
    RequestManager& requests, const NodeRegistry& registry,
    std::uint32_t response_node_id, const protocol::Packet& response,
    std::uint64_t host_now_ns, TimePoint now) {
    const auto key = route_key(response.header.session_id,
                               response.header.request_id);
    const auto found = routes_.find(key);
    if (found == routes_.end()) {
        const auto completed = completed_routes_.find(key);
        if (completed != completed_routes_.end() &&
            completed->second.node_id == response_node_id &&
            response.header.command ==
                static_cast<std::uint16_t>(completed->second.command)) {
            return {MotionGroupServiceStatus::Duplicate, {}, {}};
        }
        return {MotionGroupServiceStatus::Unrelated, {}, {}};
    }

    const Route route = found->second;
    const bool route_matches =
        response.header.message_type == protocol::MessageType::Response &&
        response.header.session_id == session_id_ &&
        response_node_id == route.node_id &&
        response.header.command == static_cast<std::uint16_t>(route.command) &&
        response.header.object_id == 0U;
    if (!route_matches) {
        if (route.phase == MotionGroupActionPhase::Abort) {
            return {MotionGroupServiceStatus::Unrelated, {}, {}};
        }
        return fail_active(requests, failure_reason(route.phase),
                           MotionGroupEventStatus::IdentityMismatch, now);
    }

    if (!frozen_nodes_are_current(registry) &&
        is_transaction_active(coordinator_.state())) {
        return fail_active(requests,
                           protocol::MotionGroupAbortReason::NodeRestarted,
                           MotionGroupEventStatus::Rejected, now);
    }

    const auto accepted = requests.accept_response(response, now);
    if (accepted.status == ResponseStatus::Duplicate) {
        remember_completed(key, route);
        routes_.erase(found);
        return {MotionGroupServiceStatus::Duplicate, {}, {}};
    }
    if (accepted.status != ResponseStatus::Matched) {
        if (route.phase == MotionGroupActionPhase::Abort) {
            remember_completed(key, route);
            routes_.erase(found);
            return {MotionGroupServiceStatus::Unrelated, {}, {}};
        }
        return fail_active(requests, failure_reason(route.phase),
                           MotionGroupEventStatus::IdentityMismatch, now);
    }
    remember_completed(key, route);
    routes_.erase(found);

    const bool remote_error =
        (response.header.flags & protocol::kErrorResponseFlag) != 0U ||
        response.payload.empty() || response.payload.front() != 0U;
    if (remote_error) {
        if (route.phase == MotionGroupActionPhase::Abort) {
            return {MotionGroupServiceStatus::Accepted, {}, {}};
        }
        return fail_active(requests, failure_reason(route.phase),
                           MotionGroupEventStatus::Rejected, now);
    }
    if (route.phase == MotionGroupActionPhase::Abort) {
        return {MotionGroupServiceStatus::Accepted, {}, {}};
    }

    const std::vector<std::uint8_t> body(response.payload.begin() + 1,
                                         response.payload.end());
    try {
        MotionGroupEventOutcome outcome;
        if (route.phase == MotionGroupActionPhase::Prepare) {
            const auto ready = protocol::decode_motion_group_ready(body);
            outcome = coordinator_.accept_ready(route.node_id, ready, now);
        } else {
            const auto ack = protocol::decode_motion_group_commit_ack(body);
            outcome = coordinator_.accept_commit_ack(route.node_id, ack, now);
        }
        return apply_coordinator_outcome(requests, registry,
                                         std::move(outcome), host_now_ns, now);
    } catch (const protocol::MotionGroupPayloadException&) {
        return fail_active(requests, failure_reason(route.phase),
                           MotionGroupEventStatus::IdentityMismatch, now);
    }
}

MotionGroupServiceOutcome MotionGroupService::poll(
    RequestManager& requests, const NodeRegistry& registry,
    const std::vector<RequestEvent>& request_events,
    std::uint64_t host_now_ns, TimePoint now) {
    MotionGroupServiceOutcome combined;
    for (const auto& event : request_events) {
        auto outcome = handle_request_event(requests, event, now);
        if (outcome.status == MotionGroupServiceStatus::Unrelated) {
            combined.unhandled_events.push_back(event);
        } else {
            if (outcome.status == MotionGroupServiceStatus::Aborting ||
                combined.status != MotionGroupServiceStatus::Aborting) {
                combined.status = outcome.status;
            }
            combined.dispatches.insert(
                combined.dispatches.end(),
                std::make_move_iterator(outcome.dispatches.begin()),
                std::make_move_iterator(outcome.dispatches.end()));
        }
    }

    if (is_transaction_active(coordinator_.state()) &&
        !frozen_nodes_are_current(registry)) {
        auto outcome = fail_active(
            requests, protocol::MotionGroupAbortReason::NodeRestarted,
            MotionGroupEventStatus::Rejected, now);
        combined.status = outcome.status;
        combined.dispatches.insert(
            combined.dispatches.end(),
            std::make_move_iterator(outcome.dispatches.begin()),
            std::make_move_iterator(outcome.dispatches.end()));
        return combined;
    }

    const auto state = coordinator_.state();
    if (is_transaction_active(state) || state == MotionGroupState::Aborting) {
        auto outcome = coordinator_.poll(now);
        if (!outcome.actions.empty() ||
            outcome.status == MotionGroupEventStatus::Settled ||
            outcome.status == MotionGroupEventStatus::TimedOut) {
            auto applied = apply_coordinator_outcome(
                requests, registry, std::move(outcome), host_now_ns, now);
            combined.status = applied.status;
            combined.dispatches.insert(
                combined.dispatches.end(),
                std::make_move_iterator(applied.dispatches.begin()),
                std::make_move_iterator(applied.dispatches.end()));
        }
    }
    return combined;
}

MotionGroupServiceOutcome MotionGroupService::cancel(
    RequestManager& requests, TimePoint now) {
    auto outcome = coordinator_.cancel(now);
    if (!outcome.actions.empty()) {
        cancel_routes(requests);
    }
    return {map_status(outcome.status),
            submit_actions(requests, outcome.actions, now), {}};
}

void MotionGroupService::reset() {
    if (!routes_.empty()) {
        throw MotionGroupServiceException(
            MotionGroupServiceError::InvalidAction,
            "仍有运动组请求路由，不能重置服务");
    }
    coordinator_.reset();
    session_id_ = 0U;
    completed_routes_.clear();
    completed_order_.clear();
}

MotionGroupState MotionGroupService::state() const noexcept {
    return coordinator_.state();
}

std::optional<protocol::MotionGroupAbortReason>
MotionGroupService::abort_reason() const noexcept {
    return coordinator_.abort_reason();
}

std::size_t MotionGroupService::pending_route_count() const noexcept {
    return routes_.size();
}

std::uint32_t MotionGroupService::session_id() const noexcept {
    return session_id_;
}

std::uint64_t MotionGroupService::route_key(
    std::uint32_t session_id, std::uint32_t request_id) noexcept {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

protocol::MotionGroupIdentityPayload MotionGroupService::action_identity(
    const MotionGroupAction& action) {
    switch (action.phase) {
        case MotionGroupActionPhase::Prepare:
            return protocol::decode_motion_group_prepare(action.payload)
                .identity;
        case MotionGroupActionPhase::Commit:
            return protocol::decode_motion_group_commit(action.payload)
                .identity;
        case MotionGroupActionPhase::Abort:
            return protocol::decode_motion_group_abort(action.payload)
                .identity;
    }
    throw MotionGroupServiceException(
        MotionGroupServiceError::InvalidAction,
        "未知运动组动作阶段");
}

MotionGroupServiceStatus MotionGroupService::map_status(
    MotionGroupEventStatus status) {
    switch (status) {
        case MotionGroupEventStatus::Duplicate:
            return MotionGroupServiceStatus::Duplicate;
        case MotionGroupEventStatus::AllReady:
            return MotionGroupServiceStatus::AllReady;
        case MotionGroupEventStatus::AllCommitted:
            return MotionGroupServiceStatus::AllCommitted;
        case MotionGroupEventStatus::Settled:
            return MotionGroupServiceStatus::Settled;
        case MotionGroupEventStatus::InvalidState:
        case MotionGroupEventStatus::UnexpectedNode:
            return MotionGroupServiceStatus::InvalidState;
        case MotionGroupEventStatus::IdentityMismatch:
        case MotionGroupEventStatus::Rejected:
        case MotionGroupEventStatus::TimedOut:
        case MotionGroupEventStatus::Cancelled:
            return MotionGroupServiceStatus::Aborting;
        case MotionGroupEventStatus::Accepted:
            return MotionGroupServiceStatus::Accepted;
    }
    return MotionGroupServiceStatus::InvalidState;
}

bool MotionGroupService::frozen_nodes_are_current(
    const NodeRegistry& registry) const noexcept {
    return std::all_of(
        coordinator_.frozen_members().begin(),
        coordinator_.frozen_members().end(),
        [&](const FrozenMotionGroupMember& member) {
            const auto* node = registry.find_by_node_id(member.node_id);
            const auto epoch = registry.clock_boot_epoch(member.node_id);
            return node != nullptr && node->online && node->assigned &&
                   epoch.has_value() && *epoch == member.boot_epoch;
        });
}

std::vector<MotionGroupDispatch> MotionGroupService::submit_actions(
    RequestManager& requests,
    const std::vector<MotionGroupAction>& actions, TimePoint now) {
    if (routes_.size() + actions.size() > config_.maximum_pending_routes) {
        throw MotionGroupServiceException(
            MotionGroupServiceError::RouteCapacityReached,
            "运动组待处理请求超过有界路由容量");
    }
    std::vector<MotionGroupDispatch> dispatches;
    dispatches.reserve(actions.size());
    std::vector<std::uint64_t> inserted;
    inserted.reserve(actions.size());
    try {
        for (const auto& action : actions) {
            if (action.node_id == 0U ||
                action.command != expected_command(action.phase)) {
                throw MotionGroupServiceException(
                    MotionGroupServiceError::InvalidAction,
                    "运动组动作节点或命令与阶段不匹配");
            }
            const auto identity = action_identity(action);
            protocol::Packet packet;
            packet.header.message_type = protocol::MessageType::Request;
            packet.header.command = static_cast<std::uint16_t>(action.command);
            packet.header.session_id = session_id_;
            packet.header.object_id = 0U;
            packet.payload = action.payload;
            auto submission = requests.submit(std::move(packet), now);
            const auto key = route_key(session_id_, submission.request_id);
            completed_routes_.erase(key);
            completed_order_.erase(
                std::remove(completed_order_.begin(), completed_order_.end(),
                            key),
                completed_order_.end());
            Route route{action.node_id, action.phase, action.command,
                        identity, submission.packet};
            try {
                routes_.emplace(key, route);
            } catch (...) {
                requests.cancel(session_id_, submission.request_id);
                throw;
            }
            inserted.push_back(key);
            dispatches.push_back(
                {action.node_id, action.phase, std::move(submission), false});
        }
    } catch (...) {
        for (const auto key : inserted) {
            const auto found = routes_.find(key);
            if (found != routes_.end()) {
                requests.cancel(found->second.packet.header.session_id,
                                found->second.packet.header.request_id);
                routes_.erase(found);
            }
        }
        throw;
    }
    return dispatches;
}

void MotionGroupService::cancel_routes(RequestManager& requests) {
    for (const auto& entry : routes_) {
        requests.cancel(entry.second.packet.header.session_id,
                        entry.second.packet.header.request_id);
        remember_completed(entry.first, entry.second);
    }
    routes_.clear();
}

void MotionGroupService::remember_completed(std::uint64_t key,
                                            const Route& route) {
    if (completed_routes_.find(key) == completed_routes_.end()) {
        completed_order_.push_back(key);
    }
    completed_routes_[key] = route;
    while (completed_order_.size() > config_.maximum_completed_routes) {
        completed_routes_.erase(completed_order_.front());
        completed_order_.pop_front();
    }
}

MotionGroupServiceOutcome MotionGroupService::apply_coordinator_outcome(
    RequestManager& requests, const NodeRegistry& registry,
    MotionGroupEventOutcome outcome, std::uint64_t host_now_ns,
    TimePoint now) {
    const bool reached_all_ready =
        outcome.status == MotionGroupEventStatus::AllReady;
    if (reached_all_ready) {
        auto commit = coordinator_.commit(registry, host_now_ns, now);
        if (commit.status == MotionGroupEventStatus::Accepted) {
            commit.status = MotionGroupEventStatus::AllReady;
        }
        outcome = std::move(commit);
    }
    if (!outcome.actions.empty() &&
        outcome.actions.front().phase == MotionGroupActionPhase::Abort) {
        cancel_routes(requests);
    }
    auto dispatches = submit_actions(requests, outcome.actions, now);
    return {map_status(outcome.status), std::move(dispatches), {}};
}

MotionGroupServiceOutcome MotionGroupService::fail_active(
    RequestManager& requests, protocol::MotionGroupAbortReason reason,
    MotionGroupEventStatus status, TimePoint now) {
    auto outcome = coordinator_.abort_due_to(reason, status, now);
    if (!outcome.actions.empty()) {
        cancel_routes(requests);
    }
    return {map_status(outcome.status),
            submit_actions(requests, outcome.actions, now), {}};
}

MotionGroupServiceOutcome MotionGroupService::handle_request_event(
    RequestManager& requests, const RequestEvent& event, TimePoint now) {
    const auto key = route_key(event.session_id, event.request_id);
    const auto found = routes_.find(key);
    if (found == routes_.end()) {
        if (completed_routes_.find(key) != completed_routes_.end()) {
            return {MotionGroupServiceStatus::Duplicate, {}, {}};
        }
        return {MotionGroupServiceStatus::Unrelated, {}, {}};
    }
    const Route route = found->second;
    if (event.type == RequestEventType::Retry && event.packet.has_value()) {
        if (event.packet->header.command !=
                static_cast<std::uint16_t>(route.command) ||
            event.packet->header.session_id != session_id_ ||
            event.packet->header.request_id != event.request_id ||
            event.packet->header.object_id != 0U ||
            event.packet->payload != route.packet.payload) {
            if (route.phase == MotionGroupActionPhase::Abort) {
                return {MotionGroupServiceStatus::Unrelated, {}, {}};
            }
            return fail_active(requests, failure_reason(route.phase),
                               MotionGroupEventStatus::IdentityMismatch, now);
        }
        return {MotionGroupServiceStatus::Retrying,
                {{route.node_id, route.phase,
                  {event.request_id, *event.packet}, true}}, {}};
    }

    remember_completed(key, route);
    routes_.erase(found);
    if (route.phase == MotionGroupActionPhase::Abort) {
        return {MotionGroupServiceStatus::Accepted, {}, {}};
    }
    return fail_active(requests, timeout_reason(route.phase),
                       MotionGroupEventStatus::TimedOut, now);
}

}
