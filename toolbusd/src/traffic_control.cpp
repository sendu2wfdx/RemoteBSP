#include "remotebsp/toolbusd/traffic_control.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace remotebsp::toolbusd {
namespace {

std::uint64_t divide_round_up(std::uint64_t numerator,
                              std::uint64_t denominator) {
    return numerator / denominator +
           static_cast<std::uint64_t>(numerator % denominator != 0);
}

std::size_t canonical_fd_length(std::size_t length) {
    if (length <= 8U) {
        return length;
    }
    constexpr std::size_t lengths[] = {12U, 16U, 20U, 24U,
                                       32U, 48U, 64U};
    for (const auto candidate : lengths) {
        if (length <= candidate) {
            return candidate;
        }
    }
    throw TrafficException(TrafficError::InvalidFrameLength,
                           "CAN-FD 帧载荷超过 64 字节");
}

std::size_t class_index(TrafficClass traffic_class) {
    const auto index = static_cast<std::size_t>(traffic_class);
    if (index >= kTrafficClassCount) {
        throw TrafficException(TrafficError::InvalidConfiguration,
                               "CAN 业务类别无效");
    }
    return index;
}

}

TrafficException::TrafficException(TrafficError code, const char* message)
    : std::runtime_error(message), code_(code) {}

TrafficError TrafficException::code() const noexcept { return code_; }

CanFrameCost estimate_can_frame_cost(
    TrafficBusMode mode, std::size_t payload_length,
    std::uint32_t arbitration_bits_per_second,
    std::uint32_t data_bits_per_second, bool bit_rate_switch) {
    if (arbitration_bits_per_second == 0 ||
        data_bits_per_second == 0) {
        throw TrafficException(TrafficError::InvalidConfiguration,
                               "CAN 位速率必须大于零");
    }
    if (mode == TrafficBusMode::Usb) {
        if (payload_length > 64U) {
            throw TrafficException(TrafficError::InvalidFrameLength,
                                   "USB 逻辑帧载荷超过 64 字节");
        }
        /* RBU1 帧头 12 字节；USB 物理层校验和令牌开销由控制器保留量吸收。 */
        const std::uint64_t total_bits = (12U + payload_length) * 8U;
        return {
            0U,
            static_cast<std::uint16_t>(total_bits),
            divide_round_up(total_bits * 1000000000ULL,
                            data_bits_per_second),
            static_cast<std::uint8_t>(payload_length),
        };
    }
    if (mode == TrafficBusMode::Classical) {
        if (payload_length > 8U) {
            throw TrafficException(TrafficError::InvalidFrameLength,
                                   "Classical CAN 帧载荷超过 8 字节");
        }
        /*
         * 11 位标准数据帧从 SOF 到帧间隔的基础长度为 47 + 8N 位。
         * 对 SOF 到 CRC 序列使用每 4 位增加 1 位的保守填充上界。
         */
        const std::uint64_t stuffed_region_bits =
            34U + payload_length * 8U;
        const std::uint64_t stuff_bits =
            stuffed_region_bits == 0
                ? 0
                : (stuffed_region_bits - 1U) / 4U;
        const std::uint64_t total_bits =
            47U + payload_length * 8U + stuff_bits;
        return {
            static_cast<std::uint16_t>(total_bits),
            0,
            divide_round_up(total_bits * 1000000000ULL,
                            arbitration_bits_per_second),
            static_cast<std::uint8_t>(payload_length)};
    }

    const std::size_t wire_length = canonical_fd_length(payload_length);
    /*
     * CAN-FD 精确长度受控制器实现、CRC 固定填充和实际位模式影响。
     * 这里按 11 位 ID、BRS 数据帧建立确定性的保守上界：
     * 仲裁/尾部 43 位；数据与 CRC 区按每 4 位增加 1 位，再保留 4 位边界余量。
     */
    const std::uint64_t crc_bits = wire_length <= 16U ? 17U : 21U;
    const std::uint64_t raw_data_phase_bits =
        wire_length * 8U + crc_bits;
    const std::uint64_t data_phase_bits =
        raw_data_phase_bits +
        divide_round_up(raw_data_phase_bits, 4U) + 4U;
    constexpr std::uint64_t nominal_phase_bits = 43U;
    const auto effective_data_rate =
        bit_rate_switch ? data_bits_per_second
                        : arbitration_bits_per_second;
    const std::uint64_t wire_time_ns =
        divide_round_up(nominal_phase_bits * 1000000000ULL,
                        arbitration_bits_per_second) +
        divide_round_up(data_phase_bits * 1000000000ULL,
                        effective_data_rate);
    return {
        static_cast<std::uint16_t>(nominal_phase_bits),
        static_cast<std::uint16_t>(data_phase_bits),
        wire_time_ns,
        static_cast<std::uint8_t>(wire_length)};
}

TrafficClass classify_traffic(const protocol::Packet& packet) noexcept {
    const auto command = static_cast<protocol::Command>(
        packet.header.command);
    switch (command) {
        case protocol::Command::DiscoveryRequest:
        case protocol::Command::DiscoveryResponse:
        case protocol::Command::NodeAssign:
        case protocol::Command::Heartbeat:
        case protocol::Command::TimeSync:
            return TrafficClass::System;
        case protocol::Command::UartWrite:
        case protocol::Command::UartRxEvent:
        case protocol::Command::StreamData:
        case protocol::Command::StreamCredit:
            return TrafficClass::Streaming;
        case protocol::Command::MotionEnqueue:
        case protocol::Command::MotionStatus:
        case protocol::Command::MotionContract:
        case protocol::Command::MotionGroupPrepare:
        case protocol::Command::MotionGroupCommit:
            return TrafficClass::Motion;
        case protocol::Command::MotionAbort:
        case protocol::Command::MotionGroupAbort:
            return TrafficClass::Safety;
        case protocol::Command::BootloaderEnter:
        case protocol::Command::BootloaderEnterUsb:
        case protocol::Command::TimedBitstreamWrite:
            return TrafficClass::Bulk;
        default:
            return TrafficClass::Interactive;
    }
}

const char* traffic_class_name(TrafficClass traffic_class) noexcept {
    switch (traffic_class) {
        case TrafficClass::Safety:
            return "safety";
        case TrafficClass::Motion:
            return "motion";
        case TrafficClass::System:
            return "system";
        case TrafficClass::Interactive:
            return "interactive";
        case TrafficClass::Streaming:
            return "streaming";
        case TrafficClass::Bulk:
            return "bulk";
        case TrafficClass::Count:
            break;
    }
    return "unknown";
}

TrafficController::TrafficController(TrafficConfig config, TimePoint now)
    : config_(config), last_refill_(now) {
    if (config_.arbitration_bits_per_second == 0 ||
        config_.data_bits_per_second == 0 ||
        config_.maximum_utilization_permille == 0 ||
        config_.maximum_utilization_permille > 1000 ||
        config_.burst_window <= std::chrono::milliseconds::zero()) {
        throw TrafficException(TrafficError::InvalidConfiguration,
                               "CAN 带宽控制配置无效");
    }
    const auto window_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            config_.burst_window)
            .count();
    global_.capacity_ns =
        static_cast<long double>(window_ns) *
        config_.maximum_utilization_permille / 1000.0L;
    global_.available_ns = global_.capacity_ns;
    global_.refill_permille = config_.maximum_utilization_permille;
    for (std::size_t index = 0; index < classes_.size(); ++index) {
        const auto limit = config_.class_limit_permille[index];
        if (limit == 0 || limit > 1000) {
            throw TrafficException(TrafficError::InvalidConfiguration,
                                   "CAN 业务类别带宽上限无效");
        }
        classes_[index].capacity_ns =
            static_cast<long double>(window_ns) * limit / 1000.0L;
        classes_[index].available_ns = classes_[index].capacity_ns;
        classes_[index].refill_permille = limit;
    }
    counters_.mode = config_.mode;
    counters_.arbitration_bits_per_second =
        config_.arbitration_bits_per_second;
    counters_.data_bits_per_second = config_.data_bits_per_second;
    counters_.maximum_utilization_permille =
        config_.maximum_utilization_permille;
    counters_.burst_window_ms =
        static_cast<std::uint32_t>(config_.burst_window.count());
    counters_.global_capacity_ns =
        static_cast<std::uint64_t>(global_.capacity_ns);
    counters_.global_available_ns = counters_.global_capacity_ns;
}

bool TrafficController::admit(
    TrafficClass traffic_class,
    const std::vector<std::size_t>& frame_payload_lengths,
    AdmissionPolicy policy, TimePoint now) {
    const auto index = class_index(traffic_class);
    refill(now);
    std::uint64_t wire_time_ns = 0;
    for (const auto length : frame_payload_lengths) {
        const auto cost = estimate_can_frame_cost(
            config_.mode, length, config_.arbitration_bits_per_second,
            config_.data_bits_per_second, true);
        if (wire_time_ns >
            std::numeric_limits<std::uint64_t>::max() -
                cost.wire_time_ns) {
            throw TrafficException(TrafficError::InvalidFrameLength,
                                   "CAN 包估算线时间溢出");
        }
        wire_time_ns += cost.wire_time_ns;
    }
    auto& bucket = classes_[index];
    auto& class_counters = counters_.classes[index];
    const bool available =
        global_.available_ns >= wire_time_ns &&
        bucket.available_ns >= wire_time_ns;
    if (!available && policy == AdmissionPolicy::Enforce) {
        ++counters_.rejected_packets;
        ++class_counters.rejected_packets;
        return false;
    }
    if (!available) {
        ++counters_.guaranteed_overruns;
    }
    global_.available_ns =
        std::max<long double>(0.0L,
                              global_.available_ns - wire_time_ns);
    bucket.available_ns =
        std::max<long double>(0.0L, bucket.available_ns - wire_time_ns);
    ++counters_.admitted_packets;
    ++class_counters.admitted_packets;
    counters_.admitted_frames += frame_payload_lengths.size();
    class_counters.admitted_frames += frame_payload_lengths.size();
    counters_.estimated_wire_time_ns += wire_time_ns;
    class_counters.estimated_wire_time_ns += wire_time_ns;
    return true;
}

TrafficSnapshot TrafficController::snapshot(TimePoint now) {
    refill(now);
    counters_.global_available_ns =
        static_cast<std::uint64_t>(global_.available_ns);
    return counters_;
}

const TrafficConfig& TrafficController::config() const noexcept {
    return config_;
}

void TrafficController::refill(TimePoint now) {
    if (now <= last_refill_) {
        return;
    }
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - last_refill_)
            .count();
    global_.available_ns = std::min(
        global_.capacity_ns,
        global_.available_ns +
            static_cast<long double>(elapsed_ns) *
                global_.refill_permille / 1000.0L);
    for (auto& bucket : classes_) {
        bucket.available_ns = std::min(
            bucket.capacity_ns,
            bucket.available_ns +
                static_cast<long double>(elapsed_ns) *
                    bucket.refill_permille / 1000.0L);
    }
    last_refill_ = now;
}

}
