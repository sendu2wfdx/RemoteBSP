#pragma once

#include "remotebsp/toolbusd/motion_group_coordinator.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::toolbusd {

struct MotionGroupServiceConfig {
    MotionGroupCoordinatorConfig coordinator;
    std::size_t maximum_pending_routes{96U};
    std::size_t maximum_completed_routes{192U};
};

enum class MotionGroupServiceError : std::uint8_t {
    InvalidConfiguration = 0,
    InvalidSession,
    RouteCapacityReached,
    InvalidAction,
};

class MotionGroupServiceException : public std::runtime_error {
public:
    MotionGroupServiceException(MotionGroupServiceError code,
                                const char* message);
    MotionGroupServiceError code() const noexcept;

private:
    MotionGroupServiceError code_;
};

struct MotionGroupDispatch {
    std::uint32_t node_id{};
    MotionGroupActionPhase phase{MotionGroupActionPhase::Prepare};
    Submission submission;
    bool retry{};
};

enum class MotionGroupServiceStatus : std::uint8_t {
    Started = 0,
    Accepted,
    Duplicate,
    Unrelated,
    Retrying,
    AllReady,
    AllCommitted,
    Aborting,
    Settled,
    InvalidState,
};

struct MotionGroupServiceOutcome {
    MotionGroupServiceStatus status{MotionGroupServiceStatus::Accepted};
    std::vector<MotionGroupDispatch> dispatches;
    std::vector<RequestEvent> unhandled_events;
};

struct MotionGroupServiceSnapshot {
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    MotionGroupState state{MotionGroupState::Idle};
    std::optional<protocol::MotionGroupAbortReason> abort_reason;
    std::uint16_t member_count{};
    std::uint16_t ready_count{};
    std::uint16_t committed_count{};
    std::uint16_t pending_request_count{};
    bool commit_dispatched{};
    // COMMIT 发出后，ABORT 只能尽力停止已武装节点，不能物理回滚。
    bool abort_is_best_effort{};
};

class MotionGroupService {
public:
    using Clock = RequestManager::Clock;
    using TimePoint = RequestManager::TimePoint;

    explicit MotionGroupService(MotionGroupServiceConfig config = {});

    MotionGroupServiceOutcome start(
        RequestManager& requests, const MotionGroupPlan& plan,
        const NodeRegistry& registry, std::uint32_t session_id,
        std::uint64_t host_now_ns, TimePoint now = Clock::now());
    MotionGroupServiceOutcome accept_response(
        RequestManager& requests, const NodeRegistry& registry,
        std::uint32_t response_node_id,
        const protocol::Packet& response,
        std::uint64_t host_now_ns, TimePoint now = Clock::now());
    MotionGroupServiceOutcome poll(
        RequestManager& requests, const NodeRegistry& registry,
        const std::vector<RequestEvent>& request_events,
        std::uint64_t host_now_ns, TimePoint now = Clock::now());
    MotionGroupServiceOutcome cancel(
        RequestManager& requests, TimePoint now = Clock::now());
    void reset();

    MotionGroupState state() const noexcept;
    std::optional<protocol::MotionGroupAbortReason> abort_reason() const
        noexcept;
    std::size_t pending_route_count() const noexcept;
    std::uint32_t session_id() const noexcept;
    MotionGroupServiceSnapshot snapshot() const noexcept;

private:
    struct Route {
        std::uint32_t node_id{};
        MotionGroupActionPhase phase{MotionGroupActionPhase::Prepare};
        protocol::Command command{protocol::Command::MotionGroupPrepare};
        protocol::MotionGroupIdentityPayload identity;
        protocol::Packet packet;
    };

    static std::uint64_t route_key(std::uint32_t session_id,
                                   std::uint32_t request_id) noexcept;
    static protocol::MotionGroupIdentityPayload action_identity(
        const MotionGroupAction& action);
    static MotionGroupServiceStatus map_status(MotionGroupEventStatus status);
    bool frozen_nodes_are_current(const NodeRegistry& registry) const noexcept;
    std::vector<MotionGroupDispatch> submit_actions(
        RequestManager& requests,
        const std::vector<MotionGroupAction>& actions,
        TimePoint now);
    void cancel_routes(RequestManager& requests);
    void remember_completed(std::uint64_t key, const Route& route);
    MotionGroupServiceOutcome apply_coordinator_outcome(
        RequestManager& requests, const NodeRegistry& registry,
        MotionGroupEventOutcome outcome, std::uint64_t host_now_ns,
        TimePoint now);
    MotionGroupServiceOutcome fail_active(
        RequestManager& requests,
        protocol::MotionGroupAbortReason reason,
        MotionGroupEventStatus status, TimePoint now);
    MotionGroupServiceOutcome handle_request_event(
        RequestManager& requests, const RequestEvent& event,
        TimePoint now);

    MotionGroupServiceConfig config_;
    MotionGroupCoordinator coordinator_;
    std::uint32_t session_id_{};
    std::unordered_map<std::uint64_t, Route> routes_;
    std::unordered_map<std::uint64_t, Route> completed_routes_;
    std::deque<std::uint64_t> completed_order_;
    bool commit_dispatched_{};
};

}
