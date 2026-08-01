#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace remotebsp::transport {

enum class CanMode {
    Classical,
    FlexibleDataRate,
};

struct CanMessage {
    std::uint32_t identifier{};
    bool extended_identifier{};
    std::vector<std::uint8_t> data;
    bool bit_rate_switch{};
};

class CanTransport {
public:
    virtual ~CanTransport() = default;

    virtual void send(const CanMessage& message) = 0;
    virtual std::optional<CanMessage> receive(
        std::chrono::milliseconds timeout) = 0;
    virtual std::size_t mtu() const noexcept = 0;
    virtual CanMode mode() const noexcept = 0;
};

}
