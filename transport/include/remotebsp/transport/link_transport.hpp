#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace remotebsp::transport {

enum class LinkKind : std::uint8_t {
    ClassicalCan = 0,
    CanFd = 1,
    Usb = 2,
    MockUsb = 3,
};

struct LinkCapabilities {
    LinkKind kind{LinkKind::ClassicalCan};
    std::size_t mtu{};
    bool reliable{};
    bool ordered{};
    bool broadcast{};
    std::uint64_t nominal_bits_per_second{};
};

/*
 * route 是链路内部的逻辑路由号，不属于 Remote Packet。
 * CAN 适配器把它映射为标准 CAN ID；USB 适配器把它放入 USB 帧头。
 */
struct LinkFrame {
    std::uint32_t route{};
    std::vector<std::uint8_t> data;
};

class LinkTransport {
public:
    virtual ~LinkTransport() = default;

    virtual void send(const LinkFrame& frame) = 0;
    virtual std::optional<LinkFrame> receive(
        std::chrono::milliseconds timeout) = 0;
    virtual LinkCapabilities capabilities() const noexcept = 0;

    std::size_t mtu() const noexcept { return capabilities().mtu; }
    LinkKind kind() const noexcept { return capabilities().kind; }
};

bool is_can_link(LinkKind kind) noexcept;
const char* link_kind_name(LinkKind kind) noexcept;

}
