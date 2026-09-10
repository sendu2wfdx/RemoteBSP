#include "remotebsp/protocol/gpio.hpp"

namespace remotebsp::protocol {

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

}
