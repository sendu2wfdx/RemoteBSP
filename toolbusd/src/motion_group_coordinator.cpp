#include "remotebsp/toolbusd/motion_group_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace remotebsp::toolbusd {
namespace {

bool digest_is_zero(const protocol::MotionGroupDigest& digest) {
    return std::all_of(digest.begin(), digest.end(),
                       [](std::uint8_t value) { return value == 0U; });
}

bool is_active(MotionGroupState state) {
    return state == MotionGroupState::Preparing ||
           state == MotionGroupState::Ready ||
           state == MotionGroupState::Committing ||
           state == MotionGroupState::Aborting;
}

}

MotionGroupCoordinatorException::MotionGroupCoordinatorException(
    MotionGroupCoordinatorError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MotionGroupCoordinatorError MotionGroupCoordinatorException::code() const
    noexcept {
    return code_;
}

MotionGroupCoordinator::MotionGroupCoordinator(
    MotionGroupCoordinatorConfig config)
    : config_(config) {
    if (config_.maximum_members == 0U ||
        config_.prepare_timeout <= std::chrono::milliseconds::zero() ||
        config_.commit_timeout <= std::chrono::milliseconds::zero() ||
        config_.abort_settle_timeout <= std::chrono::milliseconds::zero() ||
        config_.minimum_commit_guard_ns == 0U ||
        config_.maximum_clock_error_ns == 0U) {
        throw MotionGroupCoordinatorException(
            MotionGroupCoordinatorError::InvalidConfiguration,
            "运动组协调器的容量、超时和安全阈值必须大于零");
    }
}

std::vector<MotionGroupAction> MotionGroupCoordinator::begin(
    const MotionGroupPlan& plan, const NodeRegistry& registry,
    std::uint64_t host_now_ns, TimePoint now) {
    if (state_ != MotionGroupState::Idle) {
        throw MotionGroupCoordinatorException(
            MotionGroupCoordinatorError::AlreadyActive,
            "已有运动组事务正在进行");
    }
    if (plan.transaction_id == 0U || plan.group_id == 0U ||
        plan.plan_generation == 0U ||
        digest_is_zero(plan.content_digest) || plan.members.empty() ||
        plan.members.size() > config_.maximum_members ||
        plan.host_start_time_ns <= host_now_ns ||
        plan.host_start_time_ns - host_now_ns <
            config_.minimum_commit_guard_ns) {
        throw MotionGroupCoordinatorException(
            MotionGroupCoordinatorError::InvalidPlan,
            "运动组计划身份、成员数或开始提前量无效");
    }

    std::vector<FrozenMotionGroupMember> frozen;
    frozen.reserve(plan.members.size());
    std::unordered_set<std::uint32_t> node_ids;
    for (const auto& member : plan.members) {
        if (member.node_id == 0U || !node_ids.insert(member.node_id).second) {
            throw MotionGroupCoordinatorException(
                MotionGroupCoordinatorError::InvalidPlan,
                "运动组成员节点 ID 为零或重复");
        }
        if (member.segment.start_time_ns != plan.host_start_time_ns) {
            throw MotionGroupCoordinatorException(
                MotionGroupCoordinatorError::InvalidPlan,
                "成员运动段必须使用运动组统一的主机开始时间");
        }
        // 编码同时完成运动段的严格边界校验，失败时 begin 保持原状态。
        static_cast<void>(protocol::encode_motion_segment(member.segment));
        const auto boot_epoch = registry.clock_boot_epoch(member.node_id);
        if (!boot_epoch.has_value()) {
            throw MotionGroupCoordinatorException(
                MotionGroupCoordinatorError::ClockUnavailable,
                "运动组成员没有可冻结的启动代次");
        }
        const auto converted = registry.host_to_node_time(
            member.node_id, *boot_epoch, plan.host_start_time_ns,
            host_now_ns, config_.maximum_clock_error_ns);
        if (converted.status != NodeClockAccessStatus::Ready ||
            !converted.node_tick.has_value() ||
            !converted.estimate.has_value() ||
            !converted.model_generation.has_value()) {
            throw MotionGroupCoordinatorException(
                MotionGroupCoordinatorError::ClockUnavailable,
                "运动组成员时钟未同步或误差超过准入阈值");
        }
        frozen.push_back({member.node_id, *boot_epoch,
                          *converted.model_generation,
                          *converted.node_tick, *converted.estimate,
                          member.segment, false, false});
    }

    transaction_id_ = plan.transaction_id;
    group_id_ = plan.group_id;
    plan_generation_ = plan.plan_generation;
    host_start_time_ns_ = plan.host_start_time_ns;
    content_digest_ = plan.content_digest;
    members_ = std::move(frozen);
    member_indices_.clear();
    for (std::size_t index = 0U; index < members_.size(); ++index) {
        member_indices_.emplace(members_[index].node_id, index);
    }
    abort_reason_.reset();
    state_ = MotionGroupState::Preparing;
    deadline_ = now + config_.prepare_timeout;

    std::vector<MotionGroupAction> actions;
    actions.reserve(members_.size());
    for (const auto& member : members_) {
        actions.push_back({
            member.node_id, MotionGroupActionPhase::Prepare,
            protocol::Command::MotionGroupPrepare,
            protocol::encode_motion_group_prepare(
                {identity_for(member), member.segment})});
    }
    return actions;
}

MotionGroupEventOutcome MotionGroupCoordinator::accept_ready(
    std::uint32_t node_id, const protocol::MotionGroupReadyPayload& ready,
    TimePoint now) {
    if (state_ == MotionGroupState::Ready) {
        auto* member = find_member(node_id);
        return member != nullptr && member->ready &&
                       identity_matches(*member, ready.identity) &&
                       ready.code == protocol::MotionGroupReadyCode::Ready
                   ? MotionGroupEventOutcome{
                         MotionGroupEventStatus::Duplicate, {}}
                   : MotionGroupEventOutcome{
                         MotionGroupEventStatus::InvalidState, {}};
    }
    if (state_ != MotionGroupState::Preparing) {
        return {MotionGroupEventStatus::InvalidState, {}};
    }
    if (now >= deadline_) {
        return start_abort(protocol::MotionGroupAbortReason::PrepareTimedOut,
                           MotionGroupEventStatus::TimedOut, now);
    }
    auto* member = find_member(node_id);
    if (member == nullptr) {
        return {MotionGroupEventStatus::UnexpectedNode, {}};
    }
    if (!identity_matches(*member, ready.identity)) {
        return start_abort(protocol::MotionGroupAbortReason::PrepareRejected,
                           MotionGroupEventStatus::IdentityMismatch, now);
    }
    if (ready.code != protocol::MotionGroupReadyCode::Ready) {
        return start_abort(protocol::MotionGroupAbortReason::PrepareRejected,
                           MotionGroupEventStatus::Rejected, now);
    }
    if (member->ready) {
        return {MotionGroupEventStatus::Duplicate, {}};
    }
    member->ready = true;
    if (std::all_of(members_.begin(), members_.end(),
                    [](const FrozenMotionGroupMember& candidate) {
                        return candidate.ready;
                    })) {
        state_ = MotionGroupState::Ready;
        return {MotionGroupEventStatus::AllReady, {}};
    }
    return {MotionGroupEventStatus::Accepted, {}};
}

MotionGroupEventOutcome MotionGroupCoordinator::commit(
    const NodeRegistry& registry, std::uint64_t host_now_ns, TimePoint now) {
    if (state_ != MotionGroupState::Ready) {
        return {MotionGroupEventStatus::InvalidState, {}};
    }
    if (now >= deadline_) {
        return start_abort(protocol::MotionGroupAbortReason::PrepareTimedOut,
                           MotionGroupEventStatus::TimedOut, now);
    }
    if (host_start_time_ns_ <= host_now_ns ||
        host_start_time_ns_ - host_now_ns <
            config_.minimum_commit_guard_ns) {
        return start_abort(
            protocol::MotionGroupAbortReason::StartDeadlineMissed,
            MotionGroupEventStatus::TimedOut, now);
    }
    for (const auto& member : members_) {
        const auto* node = registry.find_by_node_id(member.node_id);
        const auto boot_epoch = registry.clock_boot_epoch(member.node_id);
        if (node == nullptr || !node->online || !node->assigned ||
            !boot_epoch.has_value() || *boot_epoch != member.boot_epoch) {
            return start_abort(protocol::MotionGroupAbortReason::NodeRestarted,
                               MotionGroupEventStatus::Rejected, now);
        }
    }

    std::vector<MotionGroupAction> actions;
    actions.reserve(members_.size());
    for (const auto& member : members_) {
        // 不重新换算：即使活动模型已接纳新样本，也沿用 begin 冻结的代数和 tick。
        actions.push_back({
            member.node_id, MotionGroupActionPhase::Commit,
            protocol::Command::MotionGroupCommit,
            protocol::encode_motion_group_commit({identity_for(member)})});
    }
    state_ = MotionGroupState::Committing;
    deadline_ = now + config_.commit_timeout;
    return {MotionGroupEventStatus::Accepted, std::move(actions)};
}

MotionGroupEventOutcome MotionGroupCoordinator::accept_commit_ack(
    std::uint32_t node_id,
    const protocol::MotionGroupCommitAckPayload& ack, TimePoint now) {
    if (state_ == MotionGroupState::Committed) {
        auto* member = find_member(node_id);
        return member != nullptr && member->committed &&
                       identity_matches(*member, ack.identity) &&
                       ack.code == protocol::MotionGroupCommitCode::Armed
                   ? MotionGroupEventOutcome{
                         MotionGroupEventStatus::Duplicate, {}}
                   : MotionGroupEventOutcome{
                         MotionGroupEventStatus::InvalidState, {}};
    }
    if (state_ != MotionGroupState::Committing) {
        return {MotionGroupEventStatus::InvalidState, {}};
    }
    if (now >= deadline_) {
        return start_abort(protocol::MotionGroupAbortReason::CommitTimedOut,
                           MotionGroupEventStatus::TimedOut, now);
    }
    auto* member = find_member(node_id);
    if (member == nullptr) {
        return {MotionGroupEventStatus::UnexpectedNode, {}};
    }
    if (!identity_matches(*member, ack.identity)) {
        return start_abort(protocol::MotionGroupAbortReason::CommitRejected,
                           MotionGroupEventStatus::IdentityMismatch, now);
    }
    if (ack.code != protocol::MotionGroupCommitCode::Armed) {
        return start_abort(protocol::MotionGroupAbortReason::CommitRejected,
                           MotionGroupEventStatus::Rejected, now);
    }
    if (member->committed) {
        return {MotionGroupEventStatus::Duplicate, {}};
    }
    member->committed = true;
    if (std::all_of(members_.begin(), members_.end(),
                    [](const FrozenMotionGroupMember& candidate) {
                        return candidate.committed;
                    })) {
        state_ = MotionGroupState::Committed;
        return {MotionGroupEventStatus::AllCommitted, {}};
    }
    return {MotionGroupEventStatus::Accepted, {}};
}

MotionGroupEventOutcome MotionGroupCoordinator::cancel(TimePoint now) {
    if (state_ != MotionGroupState::Preparing &&
        state_ != MotionGroupState::Ready &&
        state_ != MotionGroupState::Committing) {
        return {MotionGroupEventStatus::InvalidState, {}};
    }
    return start_abort(protocol::MotionGroupAbortReason::Cancelled,
                       MotionGroupEventStatus::Cancelled, now);
}

MotionGroupEventOutcome MotionGroupCoordinator::abort_due_to(
    protocol::MotionGroupAbortReason reason,
    MotionGroupEventStatus status, TimePoint now) {
    if (state_ != MotionGroupState::Preparing &&
        state_ != MotionGroupState::Ready &&
        state_ != MotionGroupState::Committing) {
        return {MotionGroupEventStatus::InvalidState, {}};
    }
    return start_abort(reason, status, now);
}

MotionGroupEventOutcome MotionGroupCoordinator::poll(TimePoint now) {
    if (now < deadline_) {
        return {MotionGroupEventStatus::Accepted, {}};
    }
    if (state_ == MotionGroupState::Preparing ||
        state_ == MotionGroupState::Ready) {
        return start_abort(protocol::MotionGroupAbortReason::PrepareTimedOut,
                           MotionGroupEventStatus::TimedOut, now);
    }
    if (state_ == MotionGroupState::Committing) {
        return start_abort(protocol::MotionGroupAbortReason::CommitTimedOut,
                           MotionGroupEventStatus::TimedOut, now);
    }
    if (state_ == MotionGroupState::Aborting) {
        state_ = MotionGroupState::Aborted;
        return {MotionGroupEventStatus::Settled, {}};
    }
    return {MotionGroupEventStatus::InvalidState, {}};
}

void MotionGroupCoordinator::reset() {
    if (is_active(state_)) {
        throw MotionGroupCoordinatorException(
            MotionGroupCoordinatorError::InvalidState,
            "活动运动组事务不能直接重置，必须先取消并等待收敛");
    }
    state_ = MotionGroupState::Idle;
    transaction_id_ = 0U;
    group_id_ = 0U;
    plan_generation_ = 0U;
    host_start_time_ns_ = 0U;
    content_digest_.fill(0U);
    members_.clear();
    member_indices_.clear();
    abort_reason_.reset();
    deadline_ = TimePoint{};
}

MotionGroupState MotionGroupCoordinator::state() const noexcept {
    return state_;
}

std::uint64_t MotionGroupCoordinator::transaction_id() const noexcept {
    return transaction_id_;
}

std::uint32_t MotionGroupCoordinator::group_id() const noexcept {
    return group_id_;
}

std::uint32_t MotionGroupCoordinator::plan_generation() const noexcept {
    return plan_generation_;
}

std::optional<protocol::MotionGroupAbortReason>
MotionGroupCoordinator::abort_reason() const noexcept {
    return abort_reason_;
}

const std::vector<FrozenMotionGroupMember>&
MotionGroupCoordinator::frozen_members() const noexcept {
    return members_;
}

protocol::MotionGroupIdentityPayload MotionGroupCoordinator::identity_for(
    const FrozenMotionGroupMember& member) const {
    return {transaction_id_, group_id_, plan_generation_, member.boot_epoch,
            member.clock_model_generation, member.node_start_tick,
            content_digest_};
}

bool MotionGroupCoordinator::identity_matches(
    const FrozenMotionGroupMember& member,
    const protocol::MotionGroupIdentityPayload& identity) const noexcept {
    return identity.transaction_id == transaction_id_ &&
           identity.group_id == group_id_ &&
           identity.plan_generation == plan_generation_ &&
           identity.boot_epoch == member.boot_epoch &&
           identity.clock_model_generation == member.clock_model_generation &&
           identity.node_start_tick == member.node_start_tick &&
           identity.content_digest == content_digest_;
}

std::vector<MotionGroupAction> MotionGroupCoordinator::make_abort_actions(
    protocol::MotionGroupAbortReason reason) const {
    std::vector<MotionGroupAction> actions;
    actions.reserve(members_.size());
    for (const auto& member : members_) {
        actions.push_back({
            member.node_id, MotionGroupActionPhase::Abort,
            protocol::Command::MotionGroupAbort,
            protocol::encode_motion_group_abort({identity_for(member), reason})});
    }
    return actions;
}

MotionGroupEventOutcome MotionGroupCoordinator::start_abort(
    protocol::MotionGroupAbortReason reason,
    MotionGroupEventStatus status, TimePoint now) {
    auto actions = make_abort_actions(reason);
    abort_reason_ = reason;
    state_ = MotionGroupState::Aborting;
    deadline_ = now + config_.abort_settle_timeout;
    return {status, std::move(actions)};
}

FrozenMotionGroupMember* MotionGroupCoordinator::find_member(
    std::uint32_t node_id) noexcept {
    const auto found = member_indices_.find(node_id);
    return found == member_indices_.end() ? nullptr
                                          : &members_[found->second];
}

}
