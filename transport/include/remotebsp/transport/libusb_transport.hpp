#pragma once

#include "remotebsp/transport/link_transport.hpp"
#include "remotebsp/transport/usb_frame_codec.hpp"

#include <cstdint>
#include <string>

struct libusb_context;
struct libusb_device_handle;

namespace remotebsp::transport {

struct UsbDeviceSelector {
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::string serial_number;
    std::uint8_t interface_number{};
    std::uint8_t input_endpoint{0x81U};
    std::uint8_t output_endpoint{0x01U};
    unsigned transfer_timeout_ms{100U};
};

UsbDeviceSelector parse_usb_device_selector(const std::string& text);

class LibusbTransport final : public LinkTransport {
public:
    explicit LibusbTransport(UsbDeviceSelector selector);
    ~LibusbTransport() override;

    LibusbTransport(const LibusbTransport&) = delete;
    LibusbTransport& operator=(const LibusbTransport&) = delete;

    void send(const LinkFrame& frame) override;
    std::optional<LinkFrame> receive(
        std::chrono::milliseconds timeout) override;
    LinkCapabilities capabilities() const noexcept override;

    const UsbDeviceSelector& selector() const noexcept;

private:
    void close_device() noexcept;

    UsbDeviceSelector selector_;
    libusb_context* context_{};
    libusb_device_handle* handle_{};
    bool interface_claimed_{};
    bool kernel_driver_detached_{};
    UsbFrameDecoder decoder_;
};

}
