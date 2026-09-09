#include "remotebsp/protocol/motion_group.hpp"

#include <algorithm>

namespace remotebsp::protocol {
namespace {

constexpr std::size_t kIdentitySize = 80U;
constexpr std::size_t kResultSize = kIdentitySize + 8U;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(input[1] << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

bool digest_is_zero(const MotionGroupDigest& digest) {
    return std::all_of(digest.begin(), digest.end(),
                       [](std::uint8_t value) { return value == 0U; });
}

void validate_identity(const MotionGroupIdentityPayload& identity) {
    if (identity.transaction_id == 0U || identity.group_id == 0U ||
        identity.plan_generation == 0U ||
        identity.boot_epoch == 0U ||
        identity.clock_model_generation == 0U ||
        digest_is_zero(identity.content_digest)) {
        throw MotionGroupPayloadException(
            MotionGroupPayloadError::InvalidValue,
            "运动组事务身份包含零值字段");
    }
}

void append_identity(std::vector<std::uint8_t>& output,
                     const MotionGroupIdentityPayload& identity) {
    validate_identity(identity);
    append_u16(output, kMotionGroupProtocolVersion);
    append_u16(output, 0U);
    append_u64(output, identity.transaction_id);
    append_u32(output, identity.group_id);
    append_u32(output, identity.plan_generation);
    append_u32(output, 0U);
    append_u64(output, identity.boot_epoch);
    append_u64(output, identity.clock_model_generation);
    append_u64(output, identity.node_start_tick);
    output.insert(output.end(), identity.content_digest.begin(),
                  identity.content_digest.end());
}

MotionGroupIdentityPayload read_identity(const std::uint8_t* input) {
    if (read_u16(input) != kMotionGroupProtocolVersion ||
        read_u16(input + 2U) != 0U || read_u32(input + 20U) != 0U) {
        throw MotionGroupPayloadException(
            MotionGroupPayloadError::InvalidValue,
            "运动组协议版本或保留字段无效");
    }
    MotionGroupIdentityPayload identity;
    identity.transaction_id = read_u64(input + 4U);
    identity.group_id = read_u32(input + 12U);
    identity.plan_generation = read_u32(input + 16U);
    identity.boot_epoch = read_u64(input + 24U);
    identity.clock_model_generation = read_u64(input + 32U);
    identity.node_start_tick = read_u64(input + 40U);
    std::copy_n(input + 48U, kMotionGroupDigestSize,
                identity.content_digest.begin());
    validate_identity(identity);
    return identity;
}

template <typename Code>
std::vector<std::uint8_t> encode_result(
    const MotionGroupIdentityPayload& identity, Code code,
    std::uint8_t maximum_code) {
    const auto raw = static_cast<std::uint8_t>(code);
    if (raw > maximum_code) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidValue,
                                          "运动组结果码无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kResultSize);
    append_identity(output, identity);
    output.push_back(raw);
    output.insert(output.end(), 7U, 0U);
    return output;
}

template <typename Code>
Code decode_result(const std::vector<std::uint8_t>& payload,
                   std::uint8_t maximum_code,
                   MotionGroupIdentityPayload* identity) {
    if (payload.size() != kResultSize || payload[80U] > maximum_code ||
        !std::all_of(payload.begin() + 81, payload.end(),
                     [](std::uint8_t value) { return value == 0U; })) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidLength,
                                          "运动组结果载荷长度或保留字段无效");
    }
    *identity = read_identity(payload.data());
    return static_cast<Code>(payload[80U]);
}

}

MotionGroupPayloadException::MotionGroupPayloadException(
    MotionGroupPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MotionGroupPayloadError MotionGroupPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_motion_group_prepare(
    const MotionGroupPreparePayload& payload) {
    auto segment = encode_motion_segment(payload.segment);
    std::vector<std::uint8_t> output;
    output.reserve(kIdentitySize + 4U + segment.size());
    append_identity(output, payload.identity);
    append_u16(output, static_cast<std::uint16_t>(segment.size()));
    append_u16(output, 0U);
    output.insert(output.end(), segment.begin(), segment.end());
    return output;
}

MotionGroupPreparePayload decode_motion_group_prepare(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kIdentitySize + 4U ||
        read_u16(payload.data() + kIdentitySize + 2U) != 0U) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidLength,
                                          "运动组 PREPARE 载荷过短");
    }
    const auto segment_size = read_u16(payload.data() + kIdentitySize);
    if (segment_size == 0U ||
        payload.size() != kIdentitySize + 4U + segment_size) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidLength,
                                          "运动组 PREPARE 段长度无效");
    }
    MotionGroupPreparePayload decoded;
    decoded.identity = read_identity(payload.data());
    decoded.segment = decode_motion_segment(std::vector<std::uint8_t>(
        payload.begin() + static_cast<std::ptrdiff_t>(kIdentitySize + 4U),
        payload.end()));
    return decoded;
}

std::vector<std::uint8_t> encode_motion_group_ready(
    const MotionGroupReadyPayload& payload) {
    return encode_result(payload.identity, payload.code, 4U);
}

MotionGroupReadyPayload decode_motion_group_ready(
    const std::vector<std::uint8_t>& payload) {
    MotionGroupReadyPayload decoded;
    decoded.code = decode_result<MotionGroupReadyCode>(
        payload, 4U, &decoded.identity);
    return decoded;
}

std::vector<std::uint8_t> encode_motion_group_commit(
    const MotionGroupCommitPayload& payload) {
    std::vector<std::uint8_t> output;
    output.reserve(kIdentitySize);
    append_identity(output, payload.identity);
    return output;
}

MotionGroupCommitPayload decode_motion_group_commit(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kIdentitySize) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidLength,
                                          "运动组 COMMIT 载荷长度无效");
    }
    return {read_identity(payload.data())};
}

std::vector<std::uint8_t> encode_motion_group_commit_ack(
    const MotionGroupCommitAckPayload& payload) {
    return encode_result(payload.identity, payload.code, 3U);
}

MotionGroupCommitAckPayload decode_motion_group_commit_ack(
    const std::vector<std::uint8_t>& payload) {
    MotionGroupCommitAckPayload decoded;
    decoded.code = decode_result<MotionGroupCommitCode>(
        payload, 3U, &decoded.identity);
    return decoded;
}

std::vector<std::uint8_t> encode_motion_group_abort(
    const MotionGroupAbortPayload& payload) {
    const auto raw = static_cast<std::uint8_t>(payload.reason);
    if (raw == 0U || raw > 7U) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidValue,
                                          "运动组 ABORT 原因无效");
    }
    return encode_result(payload.identity, payload.reason, 7U);
}

MotionGroupAbortPayload decode_motion_group_abort(
    const std::vector<std::uint8_t>& payload) {
    MotionGroupAbortPayload decoded;
    decoded.reason = decode_result<MotionGroupAbortReason>(
        payload, 7U, &decoded.identity);
    if (static_cast<std::uint8_t>(decoded.reason) == 0U) {
        throw MotionGroupPayloadException(MotionGroupPayloadError::InvalidValue,
                                          "运动组 ABORT 原因不能为零");
    }
    return decoded;
}

}
