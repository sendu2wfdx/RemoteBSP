#pragma once

#include "remotebsp/transport/link_transport.hpp"
#include "remotebsp/transport/usb_frame_codec.hpp"

#include <string>

namespace remotebsp::transport {

enum class MockUsbRole {
    Host,
    Device,
};

/*
 * 使用 Unix 字节流模拟 USB Bulk。它故意不保留 write 边界，用于验证
 * USB 帧头、拆包、粘包和进程间端到端链路；不用于生产部署。
 */
class MockUsbTransport final : public LinkTransport {
public:
    MockUsbTransport(std::string socket_path, MockUsbRole role);
    ~MockUsbTransport() override;

    MockUsbTransport(const MockUsbTransport&) = delete;
    MockUsbTransport& operator=(const MockUsbTransport&) = delete;

    void send(const LinkFrame& frame) override;
    std::optional<LinkFrame> receive(
        std::chrono::milliseconds timeout) override;
    LinkCapabilities capabilities() const noexcept override;

private:
    void close_all() noexcept;

    std::string socket_path_;
    MockUsbRole role_;
    int socket_{-1};
    int listener_{-1};
    UsbFrameDecoder decoder_;
};

}
