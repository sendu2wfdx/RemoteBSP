#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

// 单个节点在当前 v1 线格式中最多报告 64 个运动轴。逻辑运动组可以跨多个
// 节点组合，因此系统总轴数不受此单节点编解码预算限制。
constexpr std::size_t kMaximumMotionAxes = 64;
constexpr std::uint16_t kMotionContractVersion = 1;
constexpr std::uint16_t kMotionQueueLowWatermark = 1;

struct MotionAxisMovePayload {
    std::uint32_t resource_id{};
    std::int32_t steps{};
};

struct MotionSegmentPayload {
    std::uint32_t sequence{};
    std::uint64_t start_time_ns{};
    std::uint64_t duration_ns{};
    bool final_segment{};
    std::vector<MotionAxisMovePayload> axes;
};

// 入队响应保持固定长度，不回显全部轴数据。这样即使单板轴数较多，有副作用的
// MOTION_ENQUEUE 响应仍可放入资源受限 MCU 的请求去重缓存。
struct MotionAcceptancePayload {
    std::uint32_t sequence{};
    std::uint64_t start_time_ns{};
    std::uint64_t duration_ns{};
    bool final_segment{};
};

struct MotionAxisContractPayload {
    std::uint32_t resource_id{};
    std::uint32_t maximum_step_rate_hz{};
    std::uint32_t step_pulse_width_ns{};
    std::uint32_t minimum_step_low_ns{};
    std::uint32_t direction_setup_ns{};
};

// 节点级运动合同描述固件当前运行时资源清单能够保证的静态边界。每轴限制与
// 整板总步频限制同时生效；数值来自实体标定或明确的保守配置，不能填理论峰值。
struct MotionContractPayload {
    std::uint16_t version{kMotionContractVersion};
    std::uint16_t flags{};
    std::uint16_t queue_capacity{};
    std::uint64_t minimum_lead_time_ns{};
    std::uint32_t maximum_total_step_rate_hz{};
    std::vector<MotionAxisContractPayload> axes;
};

enum class MotionStatePayload : std::uint8_t {
    Idle = 0,
    Armed = 1,
    Running = 2,
    Faulted = 3,
};

enum class MotionFaultPayload : std::uint8_t {
    None = 0,
    Aborted = 1,
    LimitTriggered = 2,
    QueueUnderrun = 3,
    TimingDeadlineMissed = 4,
};

struct MotionAxisStatusPayload {
    std::uint32_t resource_id{};
    bool enabled{};
    bool direction_positive{};
    bool step_level{};
    std::int64_t position_steps{};
    std::uint64_t emitted_steps{};
};

struct MotionMetricsPayload {
    std::uint64_t accepted_segments{};
    std::uint64_t rejected_segments{};
    std::uint64_t completed_segments{};
    std::uint64_t emitted_edges{};
    std::uint64_t emitted_steps{};
    std::uint64_t safety_stops{};
    std::uint64_t limit_stops{};
    std::uint64_t queue_underruns{};
    std::uint16_t maximum_queue_depth{};
};

struct MotionStatusPayload {
    MotionStatePayload state{MotionStatePayload::Idle};
    MotionFaultPayload fault{MotionFaultPayload::None};
    std::uint64_t node_time_ns{};
    std::uint16_t queue_depth{};
    std::uint16_t queue_capacity{};
    std::uint16_t queue_low_watermark{};
    bool queue_low{};
    std::uint32_t last_accepted_sequence{};
    std::uint32_t last_completed_sequence{};
    MotionMetricsPayload metrics;
    std::vector<MotionAxisStatusPayload> axes;
};

enum class MotionPayloadError {
    InvalidLength,
    InvalidValue,
    TooManyAxes,
    ContractMismatch,
    RateExceeded,
};

class MotionPayloadException : public std::runtime_error {
public:
    MotionPayloadException(MotionPayloadError code, const char* message);
    MotionPayloadError code() const noexcept;

private:
    MotionPayloadError code_;
};

std::vector<std::uint8_t> encode_motion_segment(
    const MotionSegmentPayload& segment);
MotionSegmentPayload decode_motion_segment(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_motion_acceptance(
    const MotionAcceptancePayload& acceptance);
MotionAcceptancePayload decode_motion_acceptance(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_motion_contract(
    const MotionContractPayload& contract);
MotionContractPayload decode_motion_contract(
    const std::vector<std::uint8_t>& payload);
void validate_motion_segment_against_contract(
    const MotionSegmentPayload& segment,
    const MotionContractPayload& contract);

std::vector<std::uint8_t> encode_motion_status(
    const MotionStatusPayload& status);
MotionStatusPayload decode_motion_status(
    const std::vector<std::uint8_t>& payload);

}
