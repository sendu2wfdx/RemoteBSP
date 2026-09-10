#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

// GPIO_CLOSE 使用独立载荷版本，便于未来扩展关闭策略而不改变命令号。
constexpr std::uint8_t kGpioClosePayloadVersion = 1U;
constexpr std::uint8_t kGpioInputEventPayloadVersion = 1U;
constexpr std::uint8_t kGpioEdgeRising = 1U << 0U;
constexpr std::uint8_t kGpioEdgeFalling = 1U << 1U;
constexpr std::uint16_t kMaximumGpioInputEventQueueCapacity = 64U;

struct GpioClosePayload {
    std::uint8_t version{kGpioClosePayloadVersion};
};

struct GpioInputSubscription {
    std::uint8_t version{kGpioInputEventPayloadVersion};
    std::uint8_t edge_mask{kGpioEdgeRising | kGpioEdgeFalling};
    std::uint16_t queue_capacity{8U};
    std::uint32_t debounce_us{};
};

struct GpioInputEvent {
    std::uint8_t version{kGpioInputEventPayloadVersion};
    std::uint32_t sequence{};
    std::uint64_t timestamp_us{};
    bool value{};
    std::uint8_t edge{};
    std::uint32_t dropped_events{};
};

struct GpioInputEventStatus {
    std::uint8_t version{kGpioInputEventPayloadVersion};
    std::uint16_t queued_events{};
    std::uint16_t queue_capacity{};
    std::uint32_t dropped_events{};
    std::uint32_t last_sequence{};
};

enum class GpioPayloadError {
    InvalidLength,
    UnsupportedVersion,
};

class GpioPayloadException : public std::runtime_error {
public:
    GpioPayloadException(GpioPayloadError code, const char* message);
    GpioPayloadError code() const noexcept;

private:
    GpioPayloadError code_;
};

std::vector<std::uint8_t> encode_gpio_close(
    const GpioClosePayload& payload = {});
GpioClosePayload decode_gpio_close(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_gpio_input_subscription(
    const GpioInputSubscription& payload);
GpioInputSubscription decode_gpio_input_subscription(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_gpio_input_event(
    const GpioInputEvent& payload);
GpioInputEvent decode_gpio_input_event(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_gpio_input_event_status(
    const GpioInputEventStatus& payload);
GpioInputEventStatus decode_gpio_input_event_status(
    const std::vector<std::uint8_t>& payload);

}
