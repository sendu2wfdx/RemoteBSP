#pragma once

#include "remotebsp/protocol/motion_group.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::toolbusd {

struct MotionGroupCoordinatorConfig {
    std::size_t maximum_members{32U};
    std::chrono::milliseconds prepare_timeout{1000};
    std::chrono::milliseconds commit_timeout{500};
    std::chrono::milliseconds abort_settle_timeout{500};
    std::uint64_t minimum_commit_guard_ns{20000000ULL};
    std::uint64_t maximum_clock_error_ns{100000ULL};
};

struct MotionGroupMemberPlan {
    std::uint32_t node_id{};
    protocol::MotionSegmentPayload segment;
};

struct MotionGroupPlan {
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    std::uint64_t host_start_time_ns{};
    protocol::MotionGroupDigest content_digest{};
    std::vector<MotionGroupMemberPlan> members;
};

struct FrozenMotionGroupMember {
    std::uint32_t node_id{};
    std::uint64_t boot_epoch{};
    std::uint64_t clock_model_generation{};
    std::uint64_t node_start_tick{};
    ClockEstimate clock_estimate;
    protocol::MotionSegmentPayload segment;
    bool ready{};
    bool committed{};
};

enum class MotionGroupState : std::uint8_t {
    Idle = 0,
    Preparing,
    Ready,
    Committing,
    Committed,
    Aborting,
    Aborted,
};

enum class MotionGroupCoordinatorError : std::uint8_t {
    InvalidConfiguration,
    InvalidPlan,
    AlreadyActive,
    InvalidState,
    ClockUnavailable,
};

class MotionGroupCoordinatorException : public std::runtime_error {
public:
    MotionGroupCoordinatorException(MotionGroupCoordinatorError code,
                                    const char* message);
    MotionGroupCoordinatorError code() const noexcept;

private:
    MotionGroupCoordinatorError code_;
};

enum class MotionGroupActionPhase : std::uint8_t {
    Prepare = 0,
    Commit,
    Abort,
};

struct MotionGroupAction {
    std::uint32_t node_id{};
    MotionGroupActionPhase phase{MotionGroupActionPhase::Prepare};
    protocol::Command command{protocol::Command::MotionGroupPrepare};
    std::vector<std::uint8_t> payload;
};

enum class MotionGroupEventStatus : std::uint8_t {
    Accepted = 0,
    Duplicate,
    AllReady,
    AllCommitted,
    UnexpectedNode,
    InvalidState,
    IdentityMismatch,
    Rejected,
    TimedOut,
    Cancelled,
    Settled,
};

struct MotionGroupEventOutcome {
    MotionGroupEventStatus status{MotionGroupEventStatus::Accepted};
    std::vector<MotionGroupAction> actions;
};

class MotionGroupCoordinator {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit MotionGroupCoordinator(MotionGroupCoordinatorConfig config = {});

    std::vector<MotionGroupAction> begin(
        const MotionGroupPlan& plan, const NodeRegistry& registry,
        std::uint64_t host_now_ns, TimePoint now = Clock::now());
    MotionGroupEventOutcome accept_ready(
        std::uint32_t node_id,
        const protocol::MotionGroupReadyPayload& ready,
        TimePoint now = Clock::now());
    MotionGroupEventOutcome commit(
        const NodeRegistry& registry, std::uint64_t host_now_ns,
        TimePoint now = Clock::now());
    MotionGroupEventOutcome accept_commit_ack(
        std::uint32_t node_id,
        const protocol::MotionGroupCommitAckPayload& ack,
        TimePoint now = Clock::now());
    MotionGroupEventOutcome cancel(TimePoint now = Clock::now());
    MotionGroupEventOutcome poll(TimePoint now = Clock::now());
    void reset();

    MotionGroupState state() const noexcept;
    std::optional<protocol::MotionGroupAbortReason> abort_reason() const noexcept;
    const std::vector<FrozenMotionGroupMember>& frozen_members() const noexcept;

private:
    protocol::MotionGroupIdentityPayload identity_for(
        const FrozenMotionGroupMember& member) const;
    bool identity_matches(
        const FrozenMotionGroupMember& member,
        const protocol::MotionGroupIdentityPayload& identity) const noexcept;
    std::vector<MotionGroupAction> make_abort_actions(
        protocol::MotionGroupAbortReason reason) const;
    MotionGroupEventOutcome start_abort(
        protocol::MotionGroupAbortReason reason,
        MotionGroupEventStatus status, TimePoint now);
    FrozenMotionGroupMember* find_member(std::uint32_t node_id) noexcept;

    MotionGroupCoordinatorConfig config_;
    MotionGroupState state_{MotionGroupState::Idle};
    std::uint64_t transaction_id_{};
    std::uint32_t group_id_{};
    std::uint32_t plan_generation_{};
    std::uint64_t host_start_time_ns_{};
    protocol::MotionGroupDigest content_digest_{};
    std::vector<FrozenMotionGroupMember> members_;
    std::unordered_map<std::uint32_t, std::size_t> member_indices_;
    TimePoint deadline_{};
    std::optional<protocol::MotionGroupAbortReason> abort_reason_;
};

}
