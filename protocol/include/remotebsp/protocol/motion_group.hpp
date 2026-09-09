#pragma once

#include "remotebsp/protocol/motion.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kMotionGroupProtocolVersion = 1U;
constexpr std::size_t kMotionGroupDigestSize = 32U;
using MotionGroupDigest = std::array<std::uint8_t, kMotionGroupDigestSize>;

enum class MotionGroupReadyCode : std::uint8_t {
    Ready = 0,
    Rejected = 1,
    Busy = 2,
    ClockMismatch = 3,
    ResourceUnavailable = 4,
};

enum class MotionGroupCommitCode : std::uint8_t {
    Armed = 0,
    Rejected = 1,
    NotPrepared = 2,
    IdentityMismatch = 3,
};

enum class MotionGroupAbortReason : std::uint8_t {
    Cancelled = 1,
    PrepareRejected = 2,
    PrepareTimedOut = 3,
    CommitRejected = 4,
    CommitTimedOut = 5,
    NodeRestarted = 6,
    StartDeadlineMissed = 7,
};

struct MotionGroupIdentityPayload {
    std::uint64_t transaction_id{};
    std::uint32_t group_id{};
    std::uint32_t plan_generation{};
    std::uint64_t boot_epoch{};
    std::uint64_t clock_model_generation{};
    std::uint64_t node_start_tick{};
    MotionGroupDigest content_digest{};
};

struct MotionGroupPreparePayload {
    MotionGroupIdentityPayload identity;
    MotionSegmentPayload segment;
};

struct MotionGroupReadyPayload {
    MotionGroupIdentityPayload identity;
    MotionGroupReadyCode code{MotionGroupReadyCode::Ready};
};

struct MotionGroupCommitPayload {
    MotionGroupIdentityPayload identity;
};

struct MotionGroupCommitAckPayload {
    MotionGroupIdentityPayload identity;
    MotionGroupCommitCode code{MotionGroupCommitCode::Armed};
};

struct MotionGroupAbortPayload {
    MotionGroupIdentityPayload identity;
    MotionGroupAbortReason reason{MotionGroupAbortReason::Cancelled};
};

enum class MotionGroupPayloadError : std::uint8_t {
    InvalidLength,
    InvalidValue,
};

class MotionGroupPayloadException : public std::runtime_error {
public:
    MotionGroupPayloadException(MotionGroupPayloadError code,
                                const char* message);
    MotionGroupPayloadError code() const noexcept;

private:
    MotionGroupPayloadError code_;
};

std::vector<std::uint8_t> encode_motion_group_prepare(
    const MotionGroupPreparePayload& payload);
MotionGroupPreparePayload decode_motion_group_prepare(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_motion_group_ready(
    const MotionGroupReadyPayload& payload);
MotionGroupReadyPayload decode_motion_group_ready(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_motion_group_commit(
    const MotionGroupCommitPayload& payload);
MotionGroupCommitPayload decode_motion_group_commit(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_motion_group_commit_ack(
    const MotionGroupCommitAckPayload& payload);
MotionGroupCommitAckPayload decode_motion_group_commit_ack(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_motion_group_abort(
    const MotionGroupAbortPayload& payload);
MotionGroupAbortPayload decode_motion_group_abort(
    const std::vector<std::uint8_t>& payload);

}
