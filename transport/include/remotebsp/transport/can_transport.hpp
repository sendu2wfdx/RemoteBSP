#pragma once

#include "remotebsp/transport/link_transport.hpp"

#include <cstdint>
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

class CanTransport : public LinkTransport {
public:
    ~CanTransport() override = default;
    virtual CanMode mode() const noexcept = 0;

    LinkCapabilities capabilities() const noexcept override {
        return {
            mode() == CanMode::Classical
                ? LinkKind::ClassicalCan
                : LinkKind::CanFd,
            mode() == CanMode::Classical ? 8U : 64U,
            false,
            true,
            true,
            0U,
        };
    }
};

}
