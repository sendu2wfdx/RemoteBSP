#include "remotebsp/protocol/gpio.hpp"

namespace remotebsp::protocol {
namespace {

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}
void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}
std::uint32_t read_u32(const std::uint8_t* data) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(data[index]) << (index * 8U);
    }
    return value;
}
std::uint64_t read_u64(const std::uint8_t* data) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}
void require_version(std::uint8_t version) {
    if (version != kGpioInputEventPayloadVersion) {
        throw GpioPayloadException(GpioPayloadError::UnsupportedVersion,
                                   "GPIO 输入事件载荷版本不受支持");
    }
}

}

GpioPayloadException::GpioPayloadException(
    GpioPayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

GpioPayloadError GpioPayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_gpio_close(
    const GpioClosePayload& payload) {
    if (payload.version != kGpioClosePayloadVersion) {
        throw GpioPayloadException(
            GpioPayloadError::UnsupportedVersion,
            "GPIO_CLOSE 载荷版本不受支持");
    }
    return {payload.version};
}

GpioClosePayload decode_gpio_close(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 1U) {
        throw GpioPayloadException(
            GpioPayloadError::InvalidLength,
            "GPIO_CLOSE 载荷长度无效");
    }
    if (payload[0] != kGpioClosePayloadVersion) {
        throw GpioPayloadException(
            GpioPayloadError::UnsupportedVersion,
            "GPIO_CLOSE 载荷版本不受支持");
    }
    return {payload[0]};
}

std::vector<std::uint8_t> encode_gpio_input_subscription(
    const GpioInputSubscription& value) {
    require_version(value.version);
    if (value.edge_mask == 0U ||
        (value.edge_mask & ~(kGpioEdgeRising | kGpioEdgeFalling)) != 0U ||
        value.queue_capacity == 0U ||
        value.queue_capacity > kMaximumGpioInputEventQueueCapacity) {
        throw GpioPayloadException(GpioPayloadError::InvalidLength,
                                   "GPIO 输入订阅参数无效");
    }
    std::vector<std::uint8_t> output{value.version, value.edge_mask,
        static_cast<std::uint8_t>(value.queue_capacity),
        static_cast<std::uint8_t>(value.queue_capacity >> 8U)};
    append_u32(output, value.debounce_us);
    return output;
}

GpioInputSubscription decode_gpio_input_subscription(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8U) {
        throw GpioPayloadException(GpioPayloadError::InvalidLength,
                                   "GPIO 输入订阅载荷长度无效");
    }
    GpioInputSubscription value{payload[0], payload[1],
        static_cast<std::uint16_t>(payload[2] |
            (static_cast<std::uint16_t>(payload[3]) << 8U)),
        read_u32(payload.data() + 4U)};
    static_cast<void>(encode_gpio_input_subscription(value));
    return value;
}

std::vector<std::uint8_t> encode_gpio_input_event(const GpioInputEvent& value) {
    require_version(value.version);
    if (value.edge != kGpioEdgeRising && value.edge != kGpioEdgeFalling) {
        throw GpioPayloadException(GpioPayloadError::InvalidLength,
                                   "GPIO 输入事件边沿无效");
    }
    std::vector<std::uint8_t> output{value.version};
    append_u32(output, value.sequence);
    append_u64(output, value.timestamp_us);
    output.push_back(value.value ? 1U : 0U);
    output.push_back(value.edge);
    append_u32(output, value.dropped_events);
    return output;
}

GpioInputEvent decode_gpio_input_event(const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 19U || payload[13] > 1U) {
        throw GpioPayloadException(GpioPayloadError::InvalidLength,
                                   "GPIO 输入事件载荷无效");
    }
    GpioInputEvent value{payload[0], read_u32(payload.data() + 1U),
        read_u64(payload.data() + 5U), payload[13] != 0U, payload[14],
        read_u32(payload.data() + 15U)};
    static_cast<void>(encode_gpio_input_event(value));
    return value;
}

std::vector<std::uint8_t> encode_gpio_input_event_status(
    const GpioInputEventStatus& value) {
    if (value.version != kGpioInputEventPayloadVersion &&
        value.version != kGpioInputEventStatusVersion) {
        throw GpioPayloadException(GpioPayloadError::UnsupportedVersion,
                                   "GPIO 输入事件状态版本不支持");
    }
    std::vector<std::uint8_t> output{value.version,
        static_cast<std::uint8_t>(value.queued_events),
        static_cast<std::uint8_t>(value.queued_events >> 8U),
        static_cast<std::uint8_t>(value.queue_capacity),
        static_cast<std::uint8_t>(value.queue_capacity >> 8U)};
    append_u32(output, value.dropped_events);
    append_u32(output, value.last_sequence);
    if (value.version == kGpioInputEventStatusVersion) {
        output.push_back(value.exti_diagnostics_available ? 1U : 0U);
        append_u32(output, value.mailbox_dropped);
        append_u32(output, value.hints_matched);
        append_u32(output, value.hints_ignored);
    }
    return output;
}

GpioInputEventStatus decode_gpio_input_event_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 13U && payload.size() != 26U) {
        throw GpioPayloadException(GpioPayloadError::InvalidLength,
                                   "GPIO 输入事件状态载荷无效");
    }
    GpioInputEventStatus value{payload[0],
        static_cast<std::uint16_t>(payload[1] |
            (static_cast<std::uint16_t>(payload[2]) << 8U)),
        static_cast<std::uint16_t>(payload[3] |
            (static_cast<std::uint16_t>(payload[4]) << 8U)),
        read_u32(payload.data() + 5U), read_u32(payload.data() + 9U)};
    if (payload.size() == 13U) {
        require_version(value.version);
    } else {
        if (value.version != kGpioInputEventStatusVersion || payload[13] > 1U)
            throw GpioPayloadException(GpioPayloadError::UnsupportedVersion,
                                       "GPIO 输入事件状态版本不支持");
        value.exti_diagnostics_available = payload[13] != 0U;
        value.mailbox_dropped = read_u32(payload.data() + 14U);
        value.hints_matched = read_u32(payload.data() + 18U);
        value.hints_ignored = read_u32(payload.data() + 22U);
    }
    return value;
}

}
