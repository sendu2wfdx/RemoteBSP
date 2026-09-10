#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

// GPIO_CLOSE 使用独立载荷版本，便于未来扩展关闭策略而不改变命令号。
constexpr std::uint8_t kGpioClosePayloadVersion = 1U;

struct GpioClosePayload {
    std::uint8_t version{kGpioClosePayloadVersion};
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

}
