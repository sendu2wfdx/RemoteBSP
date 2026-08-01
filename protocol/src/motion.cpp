#include "remotebsp/protocol/motion.hpp"

#include <cstddef>
#include <limits>

namespace remotebsp::protocol {
namespace {

constexpr std::size_t kSegmentHeaderSize = 24;
constexpr std::size_t kMoveSize = 8;
constexpr std::size_t kStatusHeaderSize = 92;
constexpr std::size_t kAxisStatusSize = 24;

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
                MotionFaultPayload::QueueUnderrun) ||
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
