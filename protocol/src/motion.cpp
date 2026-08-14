#include "remotebsp/protocol/motion.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_set>

namespace remotebsp::protocol {
namespace {

constexpr std::size_t kSegmentHeaderSize = 24;
constexpr std::size_t kMoveSize = 8;
constexpr std::size_t kStatusHeaderSize = 92;
constexpr std::size_t kAxisStatusSize = 24;
constexpr std::size_t kContractHeaderSize = 24;
constexpr std::size_t kAxisContractSize = 20;
constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index])
                 << (index * 8U);
    }
    return value;
}

void validate_axis_count(std::size_t count) {
    if (count == 0 || count > kMaximumMotionAxes) {
        throw MotionPayloadException(MotionPayloadError::TooManyAxes,
                                     "单节点运动轴数量必须位于 1～64");
    }
}

std::int32_t decode_i32(std::uint32_t value) {
    if (value <=
        static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())) {
        return static_cast<std::int32_t>(value);
    }
    return -1 - static_cast<std::int32_t>(
                    std::numeric_limits<std::uint32_t>::max() - value);
}

std::int64_t decode_i64(std::uint64_t value) {
    if (value <=
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(value);
    }
    return -1 - static_cast<std::int64_t>(
                    std::numeric_limits<std::uint64_t>::max() - value);
}

std::uint64_t absolute_steps(std::int32_t steps) {
    return steps < 0
               ? static_cast<std::uint64_t>(
                     -static_cast<std::int64_t>(steps))
               : static_cast<std::uint64_t>(steps);
}

std::uint64_t maximum_events_for_duration(
    std::uint64_t duration_ns, std::uint32_t rate_hz) {
    const auto whole_seconds = duration_ns / kNanosecondsPerSecond;
    const auto remaining_ns = duration_ns % kNanosecondsPerSecond;
    if (whole_seconds >
        std::numeric_limits<std::uint64_t>::max() / rate_hz) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_seconds * rate_hz +
           (remaining_ns * rate_hz) / kNanosecondsPerSecond;
}

void validate_axis_contract(const MotionAxisContractPayload& axis) {
    if (axis.resource_id == 0 || axis.maximum_step_rate_hz == 0 ||
        axis.step_pulse_width_ns == 0 ||
        axis.minimum_step_low_ns == 0 || axis.direction_setup_ns == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动轴合同包含零值字段");
    }
    const std::uint64_t minimum_period =
        static_cast<std::uint64_t>(axis.step_pulse_width_ns) +
        axis.minimum_step_low_ns;
    if (static_cast<std::uint64_t>(axis.maximum_step_rate_hz) *
            minimum_period >
        kNanosecondsPerSecond) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动轴合同的步频与脉冲时序冲突");
    }
}

}

MotionPayloadException::MotionPayloadException(
    MotionPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MotionPayloadError MotionPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_motion_segment(
    const MotionSegmentPayload& segment) {
    validate_axis_count(segment.axes.size());
    if (segment.sequence == 0 || segment.duration_ns == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动段序号和持续时间必须非零");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kSegmentHeaderSize + segment.axes.size() * kMoveSize);
    append_u32(output, segment.sequence);
    append_u64(output, segment.start_time_ns);
    append_u64(output, segment.duration_ns);
    output.push_back(static_cast<std::uint8_t>(segment.final_segment));
    output.push_back(static_cast<std::uint8_t>(segment.axes.size()));
    append_u16(output, 0);
    for (const auto& axis : segment.axes) {
        if (axis.resource_id == 0) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动轴资源 ID 不能为零");
        }
        append_u32(output, axis.resource_id);
        append_u32(output, static_cast<std::uint32_t>(axis.steps));
    }
    return output;
}

MotionSegmentPayload decode_motion_segment(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kSegmentHeaderSize) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动段载荷过短");
    }
    const auto axis_count = payload[21];
    validate_axis_count(axis_count);
    if (payload.size() !=
            kSegmentHeaderSize +
                static_cast<std::size_t>(axis_count) * kMoveSize ||
        payload[20] > 1 || read_u16(payload.data() + 22) != 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动段载荷长度或保留字段无效");
    }
    MotionSegmentPayload segment;
    segment.sequence = read_u32(payload.data());
    segment.start_time_ns = read_u64(payload.data() + 4);
    segment.duration_ns = read_u64(payload.data() + 12);
    segment.final_segment = payload[20] != 0;
    if (segment.sequence == 0 || segment.duration_ns == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动段序号和持续时间必须非零");
    }
    segment.axes.reserve(axis_count);
    for (std::size_t index = 0; index < axis_count; ++index) {
        const auto* entry =
            payload.data() + kSegmentHeaderSize + index * kMoveSize;
        const auto resource_id = read_u32(entry);
        if (resource_id == 0) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动轴资源 ID 不能为零");
        }
        segment.axes.push_back(
            {resource_id, decode_i32(read_u32(entry + 4))});
    }
    return segment;
}

std::vector<std::uint8_t> encode_motion_acceptance(
    const MotionAcceptancePayload& acceptance) {
    if (acceptance.sequence == 0 || acceptance.duration_ns == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动入队确认字段无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kSegmentHeaderSize);
    append_u32(output, acceptance.sequence);
    append_u64(output, acceptance.start_time_ns);
    append_u64(output, acceptance.duration_ns);
    output.push_back(
        static_cast<std::uint8_t>(acceptance.final_segment));
    output.push_back(0);
    append_u16(output, 0);
    return output;
}

MotionAcceptancePayload decode_motion_acceptance(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kSegmentHeaderSize ||
        payload[20] > 1 || payload[21] != 0 ||
        read_u16(payload.data() + 22) != 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动入队确认长度或保留字段无效");
    }
    MotionAcceptancePayload acceptance{
        read_u32(payload.data()),
        read_u64(payload.data() + 4),
        read_u64(payload.data() + 12),
        payload[20] != 0};
    if (acceptance.sequence == 0 ||
        acceptance.duration_ns == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动入队确认字段无效");
    }
    return acceptance;
}

std::vector<std::uint8_t> encode_motion_contract(
    const MotionContractPayload& contract) {
    validate_axis_count(contract.axes.size());
    if (contract.version != kMotionContractVersion ||
        contract.flags != 0 || contract.queue_capacity == 0 ||
        contract.minimum_lead_time_ns == 0 ||
        contract.maximum_total_step_rate_hz == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动合同头字段无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kContractHeaderSize +
                   contract.axes.size() * kAxisContractSize);
    append_u16(output, contract.version);
    append_u16(output, contract.flags);
    append_u16(output,
               static_cast<std::uint16_t>(contract.axes.size()));
    append_u16(output, contract.queue_capacity);
    append_u64(output, contract.minimum_lead_time_ns);
    append_u32(output, contract.maximum_total_step_rate_hz);
    append_u32(output, 0);
    std::unordered_set<std::uint32_t> resource_ids;
    for (const auto& axis : contract.axes) {
        validate_axis_contract(axis);
        if (!resource_ids.insert(axis.resource_id).second) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动合同轴资源 ID 重复");
        }
        append_u32(output, axis.resource_id);
        append_u32(output, axis.maximum_step_rate_hz);
        append_u32(output, axis.step_pulse_width_ns);
        append_u32(output, axis.minimum_step_low_ns);
        append_u32(output, axis.direction_setup_ns);
    }
    return output;
}

MotionContractPayload decode_motion_contract(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kContractHeaderSize) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动合同载荷过短");
    }
    const auto axis_count = read_u16(payload.data() + 4);
    validate_axis_count(axis_count);
    if (payload.size() !=
            kContractHeaderSize +
                static_cast<std::size_t>(axis_count) * kAxisContractSize ||
        read_u32(payload.data() + 20) != 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动合同长度或保留字段无效");
    }
    MotionContractPayload contract;
    contract.version = read_u16(payload.data());
    contract.flags = read_u16(payload.data() + 2);
    contract.queue_capacity = read_u16(payload.data() + 6);
    contract.minimum_lead_time_ns = read_u64(payload.data() + 8);
    contract.maximum_total_step_rate_hz = read_u32(payload.data() + 16);
    if (contract.version != kMotionContractVersion ||
        contract.flags != 0 || contract.queue_capacity == 0 ||
        contract.minimum_lead_time_ns == 0 ||
        contract.maximum_total_step_rate_hz == 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动合同头字段无效");
    }
    std::unordered_set<std::uint32_t> resource_ids;
    contract.axes.reserve(axis_count);
    for (std::size_t index = 0; index < axis_count; ++index) {
        const auto* entry = payload.data() + kContractHeaderSize +
                            index * kAxisContractSize;
        MotionAxisContractPayload axis{
            read_u32(entry), read_u32(entry + 4), read_u32(entry + 8),
            read_u32(entry + 12), read_u32(entry + 16)};
        validate_axis_contract(axis);
        if (!resource_ids.insert(axis.resource_id).second) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动合同轴资源 ID 重复");
        }
        contract.axes.push_back(axis);
    }
    return contract;
}

void validate_motion_segment_against_contract(
    const MotionSegmentPayload& segment,
    const MotionContractPayload& contract) {
    // 复用合同编码器的全部结构校验，避免调用方传入未验证的本地合同。
    static_cast<void>(encode_motion_contract(contract));
    if (segment.duration_ns == 0 ||
        segment.axes.size() != contract.axes.size()) {
        throw MotionPayloadException(
            MotionPayloadError::ContractMismatch,
            "运动段轴数量与节点当前运动合同不一致");
    }
    std::unordered_set<std::uint32_t> seen;
    std::uint64_t total_steps = 0;
    for (const auto& move : segment.axes) {
        if (!seen.insert(move.resource_id).second) {
            throw MotionPayloadException(MotionPayloadError::ContractMismatch,
                                         "运动段包含重复轴资源");
        }
        const auto found = std::find_if(
            contract.axes.begin(), contract.axes.end(),
            [&move](const auto& axis) {
                return axis.resource_id == move.resource_id;
            });
        if (found == contract.axes.end()) {
            throw MotionPayloadException(MotionPayloadError::ContractMismatch,
                                         "运动段引用了合同之外的轴资源");
        }
        const auto steps = absolute_steps(move.steps);
        const auto available_after_setup =
            segment.duration_ns > found->direction_setup_ns
                ? segment.duration_ns - found->direction_setup_ns
                : 0;
        const auto minimum_period =
            static_cast<std::uint64_t>(found->step_pulse_width_ns) +
            found->minimum_step_low_ns;
        if (steps > maximum_events_for_duration(
                        segment.duration_ns,
                        found->maximum_step_rate_hz) ||
            (steps != 0 &&
             (available_after_setup < found->step_pulse_width_ns ||
              steps > available_after_setup / minimum_period + 1U))) {
            throw MotionPayloadException(MotionPayloadError::RateExceeded,
                                         "运动段超过单轴步频或STEP时序合同");
        }
        if (std::numeric_limits<std::uint64_t>::max() - total_steps <
            steps) {
            throw MotionPayloadException(MotionPayloadError::RateExceeded,
                                         "运动段总步数溢出");
        }
        total_steps += steps;
    }
    if (total_steps > maximum_events_for_duration(
                          segment.duration_ns,
                          contract.maximum_total_step_rate_hz)) {
        throw MotionPayloadException(MotionPayloadError::RateExceeded,
                                     "运动段超过整板总STEP频率合同");
    }
}

std::vector<std::uint8_t> encode_motion_status(
    const MotionStatusPayload& status) {
    validate_axis_count(status.axes.size());
    if (status.queue_depth > status.queue_capacity ||
        status.metrics.maximum_queue_depth > status.queue_capacity) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动队列状态无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kStatusHeaderSize +
                   status.axes.size() * kAxisStatusSize);
    output.push_back(static_cast<std::uint8_t>(status.state));
    output.push_back(static_cast<std::uint8_t>(status.fault));
    output.push_back(static_cast<std::uint8_t>(status.axes.size()));
    output.push_back(0);
    append_u64(output, status.node_time_ns);
    append_u16(output, status.queue_depth);
    append_u16(output, status.queue_capacity);
    append_u32(output, status.last_accepted_sequence);
    append_u32(output, status.last_completed_sequence);
    append_u64(output, status.metrics.accepted_segments);
    append_u64(output, status.metrics.rejected_segments);
    append_u64(output, status.metrics.completed_segments);
    append_u64(output, status.metrics.emitted_edges);
    append_u64(output, status.metrics.emitted_steps);
    append_u64(output, status.metrics.safety_stops);
    append_u64(output, status.metrics.limit_stops);
    append_u64(output, status.metrics.queue_underruns);
    append_u16(output, status.metrics.maximum_queue_depth);
    append_u16(output, 0);
    for (const auto& axis : status.axes) {
        if (axis.resource_id == 0) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动状态轴资源 ID 不能为零");
        }
        append_u32(output, axis.resource_id);
        const auto flags =
            static_cast<std::uint8_t>(
                (axis.enabled ? 0x01U : 0U) |
                (axis.direction_positive ? 0x02U : 0U) |
                (axis.step_level ? 0x04U : 0U));
        output.push_back(flags);
        output.insert(output.end(), 3, 0);
        append_u64(output, static_cast<std::uint64_t>(
                               axis.position_steps));
        append_u64(output, axis.emitted_steps);
    }
    return output;
}

MotionStatusPayload decode_motion_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < kStatusHeaderSize) {
        throw MotionPayloadException(MotionPayloadError::InvalidLength,
                                     "运动状态载荷过短");
    }
    const auto state = payload[0];
    const auto fault = payload[1];
    const auto axis_count = payload[2];
    validate_axis_count(axis_count);
    if (state > static_cast<std::uint8_t>(MotionStatePayload::Faulted) ||
        fault >
            static_cast<std::uint8_t>(
                MotionFaultPayload::TimingDeadlineMissed) ||
        payload[3] != 0 ||
        payload.size() !=
            kStatusHeaderSize +
                static_cast<std::size_t>(axis_count) * kAxisStatusSize ||
        read_u16(payload.data() + 90) != 0) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动状态枚举、长度或保留字段无效");
    }
    MotionStatusPayload status;
    status.state = static_cast<MotionStatePayload>(state);
    status.fault = static_cast<MotionFaultPayload>(fault);
    status.node_time_ns = read_u64(payload.data() + 4);
    status.queue_depth = read_u16(payload.data() + 12);
    status.queue_capacity = read_u16(payload.data() + 14);
    status.last_accepted_sequence = read_u32(payload.data() + 16);
    status.last_completed_sequence = read_u32(payload.data() + 20);
    status.metrics.accepted_segments = read_u64(payload.data() + 24);
    status.metrics.rejected_segments = read_u64(payload.data() + 32);
    status.metrics.completed_segments = read_u64(payload.data() + 40);
    status.metrics.emitted_edges = read_u64(payload.data() + 48);
    status.metrics.emitted_steps = read_u64(payload.data() + 56);
    status.metrics.safety_stops = read_u64(payload.data() + 64);
    status.metrics.limit_stops = read_u64(payload.data() + 72);
    status.metrics.queue_underruns = read_u64(payload.data() + 80);
    status.metrics.maximum_queue_depth = read_u16(payload.data() + 88);
    if (status.queue_depth > status.queue_capacity ||
        status.metrics.maximum_queue_depth > status.queue_capacity) {
        throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                     "运动队列状态无效");
    }
    status.axes.reserve(axis_count);
    for (std::size_t index = 0; index < axis_count; ++index) {
        const auto* entry =
            payload.data() + kStatusHeaderSize +
            index * kAxisStatusSize;
        const auto flags = entry[4];
        if (read_u32(entry + 4) > 0x07U ||
            read_u32(entry) == 0) {
            throw MotionPayloadException(MotionPayloadError::InvalidValue,
                                         "运动轴状态标志无效");
        }
        status.axes.push_back(
            {read_u32(entry),
             (flags & 0x01U) != 0,
             (flags & 0x02U) != 0,
             (flags & 0x04U) != 0,
             decode_i64(read_u64(entry + 8)),
             read_u64(entry + 16)});
    }
    return status;
}

}
