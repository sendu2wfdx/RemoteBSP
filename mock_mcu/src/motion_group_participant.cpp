#include "remotebsp/mock_mcu/motion_group_participant.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace remotebsp::mock_mcu {

MockMotionGroupParticipant::MockMotionGroupParticipant(
    std::shared_ptr<MotionExecutor> motion,
    std::shared_ptr<MockTimeSyncBsp> clock)
    : motion_(std::move(motion)), clock_(std::move(clock)) {
    if ((motion_ == nullptr) != (clock_ == nullptr)) {
        throw std::invalid_argument(
            "运动执行器和 Mock 时钟必须同时提供");
    }
}

protocol::MotionGroupReadyPayload MockMotionGroupParticipant::prepare(
    const protocol::MotionGroupPreparePayload& prepare,
    protocol::MotionGroupReadyCode forced_result,
    std::uint32_t session_id, TimePoint now) {
    // 利用正式编解码器执行全部字段和运动段边界校验。
    const auto validated = protocol::decode_motion_group_prepare(
        protocol::encode_motion_group_prepare(prepare));
    if (prepared_.has_value() &&
        state_ == MockMotionGroupState::Aborted) {
        const bool stale_generation =
            validated.identity.group_id == prepared_->identity.group_id &&
            validated.identity.plan_generation <=
                prepared_->identity.plan_generation;
        if (stale_generation) {
            return {validated.identity,
                    protocol::MotionGroupReadyCode::Busy};
        }
        prepared_.reset();
        prepared_motion_segment_.reset();
        owner_session_id_ = 0U;
        state_ = MockMotionGroupState::Idle;
    }
    if (prepared_.has_value()) {
        const bool same_content =
            protocol::encode_motion_group_prepare(*prepared_) ==
            protocol::encode_motion_group_prepare(validated);
        return {validated.identity,
                same_content && owner_session_id_ == session_id
                    ? (state_ == MockMotionGroupState::Prepared ||
                               state_ == MockMotionGroupState::Armed
                           ? protocol::MotionGroupReadyCode::Ready
                           : protocol::MotionGroupReadyCode::Busy)
                    : protocol::MotionGroupReadyCode::Busy};
    }
    if (forced_result != protocol::MotionGroupReadyCode::Ready) {
        return {validated.identity, forced_result};
    }
    std::optional<MotionSegment> local_segment;
    if (motion_ != nullptr) {
        if (validated.identity.boot_epoch != clock_->boot_epoch()) {
            return {validated.identity,
                    protocol::MotionGroupReadyCode::ClockMismatch};
        }
        const auto status = motion_->status();
        if (status.state != MotionState::Idle ||
            status.fault != MotionFault::None || status.queue_depth != 0U) {
            return {validated.identity,
                    protocol::MotionGroupReadyCode::Busy};
        }
        local_segment = make_local_segment(validated, now);
        if (!local_segment.has_value()) {
            return {validated.identity,
                    protocol::MotionGroupReadyCode::ClockMismatch};
        }
        try {
            static_cast<void>(motion_->validate_enqueue(
                *local_segment, clock_->elapsed_ns(now)));
        } catch (const MotionException&) {
            return {validated.identity,
                    protocol::MotionGroupReadyCode::Rejected};
        }
    }
    prepared_ = validated;
    prepared_motion_segment_ = std::move(local_segment);
    owner_session_id_ = session_id;
    state_ = MockMotionGroupState::Prepared;
    return {validated.identity, protocol::MotionGroupReadyCode::Ready};
}

protocol::MotionGroupCommitAckPayload MockMotionGroupParticipant::commit(
    const protocol::MotionGroupCommitPayload& commit,
    protocol::MotionGroupCommitCode forced_result,
    std::uint32_t session_id, TimePoint now) {
    const auto validated = protocol::decode_motion_group_commit(
        protocol::encode_motion_group_commit(commit));
    if (!prepared_.has_value()) {
        return {validated.identity,
                protocol::MotionGroupCommitCode::NotPrepared};
    }
    if (owner_session_id_ != session_id ||
        !same_identity(prepared_->identity, validated.identity)) {
        return {validated.identity,
                protocol::MotionGroupCommitCode::IdentityMismatch};
    }
    if (state_ == MockMotionGroupState::Armed) {
        return {validated.identity, protocol::MotionGroupCommitCode::Armed};
    }
    if (state_ != MockMotionGroupState::Prepared) {
        return {validated.identity,
                protocol::MotionGroupCommitCode::NotPrepared};
    }
    if (forced_result != protocol::MotionGroupCommitCode::Armed) {
        return {validated.identity, forced_result};
    }
    if (motion_ != nullptr) {
        const auto local_segment = make_local_segment(*prepared_, now);
        if (!local_segment.has_value() ||
            !prepared_motion_segment_.has_value() ||
            local_segment->start_time_ns !=
                prepared_motion_segment_->start_time_ns) {
            return {validated.identity,
                    protocol::MotionGroupCommitCode::IdentityMismatch};
        }
        try {
            static_cast<void>(motion_->enqueue(
                *prepared_motion_segment_, clock_->elapsed_ns(now)));
        } catch (const MotionException&) {
            return {validated.identity,
                    protocol::MotionGroupCommitCode::Rejected};
        }
    }
    state_ = MockMotionGroupState::Armed;
    return {validated.identity, protocol::MotionGroupCommitCode::Armed};
}

bool MockMotionGroupParticipant::abort(
    const protocol::MotionGroupAbortPayload& abort_payload,
    std::uint32_t session_id, TimePoint now) noexcept {
    if (!prepared_.has_value() ||
        owner_session_id_ != session_id ||
        !same_identity(prepared_->identity, abort_payload.identity)) {
        return false;
    }
    if (state_ == MockMotionGroupState::Aborted) {
        return true;
    }
    if (motion_ != nullptr && state_ == MockMotionGroupState::Armed) {
        try {
            const auto now_ns = std::max(
                clock_->elapsed_ns(now),
                motion_->status().node_time_ns);
            static_cast<void>(motion_->abort(
                now_ns, MotionFault::Aborted));
        } catch (const std::exception&) {
            state_ = MockMotionGroupState::Aborted;
            return false;
        }
    }
    state_ = MockMotionGroupState::Aborted;
    return true;
}

bool MockMotionGroupParticipant::emergency_abort() noexcept {
    if (!prepared_.has_value()) {
        return false;
    }
    if (state_ == MockMotionGroupState::Aborted) {
        return true;
    }
    if (motion_ != nullptr && state_ == MockMotionGroupState::Armed) {
        if (motion_->status().fault == MotionFault::None) {
            try {
                const auto now_ns = motion_->status().node_time_ns;
                static_cast<void>(
                    motion_->abort(now_ns, MotionFault::Aborted));
            } catch (const std::exception&) {
                state_ = MockMotionGroupState::Aborted;
                return false;
            }
        }
    }
    state_ = MockMotionGroupState::Aborted;
    return true;
}

bool MockMotionGroupParticipant::cancel_session(
    std::uint32_t session_id) noexcept {
    return owned_by(session_id) && emergency_abort();
}

bool MockMotionGroupParticipant::uses_resource(
    std::uint32_t resource_id) const noexcept {
    if (!prepared_.has_value()) {
        return false;
    }
    return std::any_of(
        prepared_->segment.axes.begin(), prepared_->segment.axes.end(),
        [resource_id](const auto& axis) {
            return axis.resource_id == resource_id;
        });
}

bool MockMotionGroupParticipant::owned_by(
    std::uint32_t session_id) const noexcept {
    return prepared_.has_value() && owner_session_id_ == session_id;
}

void MockMotionGroupParticipant::reset() noexcept {
    state_ = MockMotionGroupState::Idle;
    prepared_.reset();
    prepared_motion_segment_.reset();
    owner_session_id_ = 0U;
}

MockMotionGroupState MockMotionGroupParticipant::state() const noexcept {
    return state_;
}

const std::optional<protocol::MotionGroupPreparePayload>&
MockMotionGroupParticipant::prepared() const noexcept {
    return prepared_;
}

bool MockMotionGroupParticipant::same_identity(
    const protocol::MotionGroupIdentityPayload& left,
    const protocol::MotionGroupIdentityPayload& right) noexcept {
    return left.transaction_id == right.transaction_id &&
           left.group_id == right.group_id &&
           left.plan_generation == right.plan_generation &&
           left.boot_epoch == right.boot_epoch &&
           left.clock_model_generation == right.clock_model_generation &&
           left.node_start_tick == right.node_start_tick &&
           left.content_digest == right.content_digest;
}

std::optional<MotionSegment> MockMotionGroupParticipant::make_local_segment(
    const protocol::MotionGroupPreparePayload& prepare,
    TimePoint now) const {
    if (clock_ == nullptr) {
        return std::nullopt;
    }
    const auto start_ns = clock_->motion_time_ns_for_tick(
        prepare.identity.node_start_tick, now);
    if (!start_ns.has_value()) {
        return std::nullopt;
    }
    MotionSegment segment;
    segment.sequence = prepare.segment.sequence;
    segment.start_time_ns = *start_ns;
    segment.duration_ns = prepare.segment.duration_ns;
    segment.final_segment = prepare.segment.final_segment;
    segment.axes.reserve(prepare.segment.axes.size());
    for (const auto& axis : prepare.segment.axes) {
        segment.axes.push_back({axis.resource_id, axis.steps});
    }
    return segment;
}

}
