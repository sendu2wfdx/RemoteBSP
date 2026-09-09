#include "remotebsp/mock_mcu/motion_group_participant.hpp"

namespace remotebsp::mock_mcu {

protocol::MotionGroupReadyPayload MockMotionGroupParticipant::prepare(
    const protocol::MotionGroupPreparePayload& prepare,
    protocol::MotionGroupReadyCode forced_result) {
    // 利用正式编解码器执行全部字段和运动段边界校验。
    const auto validated = protocol::decode_motion_group_prepare(
        protocol::encode_motion_group_prepare(prepare));
    if (prepared_.has_value()) {
        const bool same_content =
            protocol::encode_motion_group_prepare(*prepared_) ==
            protocol::encode_motion_group_prepare(validated);
        return {validated.identity,
                same_content
                    ? (state_ == MockMotionGroupState::Prepared ||
                               state_ == MockMotionGroupState::Armed
                           ? protocol::MotionGroupReadyCode::Ready
                           : protocol::MotionGroupReadyCode::Busy)
                    : protocol::MotionGroupReadyCode::Busy};
    }
    if (forced_result != protocol::MotionGroupReadyCode::Ready) {
        return {validated.identity, forced_result};
    }
    prepared_ = validated;
    state_ = MockMotionGroupState::Prepared;
    return {validated.identity, protocol::MotionGroupReadyCode::Ready};
}

protocol::MotionGroupCommitAckPayload MockMotionGroupParticipant::commit(
    const protocol::MotionGroupCommitPayload& commit,
    protocol::MotionGroupCommitCode forced_result) {
    const auto validated = protocol::decode_motion_group_commit(
        protocol::encode_motion_group_commit(commit));
    if (!prepared_.has_value()) {
        return {validated.identity,
                protocol::MotionGroupCommitCode::NotPrepared};
    }
    if (!same_identity(prepared_->identity, validated.identity)) {
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
    state_ = MockMotionGroupState::Armed;
    return {validated.identity, protocol::MotionGroupCommitCode::Armed};
}

bool MockMotionGroupParticipant::abort(
    const protocol::MotionGroupAbortPayload& abort_payload) noexcept {
    if (!prepared_.has_value() ||
        !same_identity(prepared_->identity, abort_payload.identity)) {
        return false;
    }
    state_ = MockMotionGroupState::Aborted;
    return true;
}

void MockMotionGroupParticipant::reset() noexcept {
    state_ = MockMotionGroupState::Idle;
    prepared_.reset();
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

}
