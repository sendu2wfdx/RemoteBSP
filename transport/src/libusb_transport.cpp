#include "remotebsp/transport/libusb_transport.hpp"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace remotebsp::transport {
namespace {

std::runtime_error usb_error(const char* operation, int code) {
    std::ostringstream stream;
    stream << operation << ": " << libusb_error_name(code);
    return std::runtime_error(stream.str());
}

std::uint16_t parse_u16(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value == 0U || value > 0xFFFFU) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint16_t>(value);
}

bool serial_matches(libusb_device_handle* handle,
                    const libusb_device_descriptor& descriptor,
                    const std::string& expected) {
    if (expected.empty()) {
        return true;
    }
    if (descriptor.iSerialNumber == 0U) {
        return false;
    }
    std::array<unsigned char, 256> text{};
    const int length = libusb_get_string_descriptor_ascii(
        handle, descriptor.iSerialNumber, text.data(),
        static_cast<int>(text.size()));
    return length >= 0 &&
           std::string(reinterpret_cast<const char*>(text.data()),
                       static_cast<std::size_t>(length)) == expected;
}

}

UsbDeviceSelector parse_usb_device_selector(const std::string& text) {
    const auto at = text.find('@');
    const std::string ids = text.substr(0, at);
    const auto colon = ids.find(':');
    if (colon == std::string::npos || colon == 0U ||
        colon + 1U >= ids.size() ||
        ids.find(':', colon + 1U) != std::string::npos) {
        throw std::invalid_argument(
            "USB 设备选择器必须为 VID:PID 或 VID:PID@SERIAL");
    }
    UsbDeviceSelector selector;
    selector.vendor_id = parse_u16(ids.substr(0, colon), "USB VID");
    selector.product_id = parse_u16(ids.substr(colon + 1U), "USB PID");
    if (at != std::string::npos) {
        selector.serial_number = text.substr(at + 1U);
        if (selector.serial_number.empty()) {
            throw std::invalid_argument("USB 序列号不能为空");
        }
    }
    return selector;
}

LibusbTransport::LibusbTransport(UsbDeviceSelector selector)
    : selector_(std::move(selector)) {
    if (selector_.vendor_id == 0U || selector_.product_id == 0U ||
        (selector_.input_endpoint & LIBUSB_ENDPOINT_DIR_MASK) !=
            LIBUSB_ENDPOINT_IN ||
        (selector_.output_endpoint & LIBUSB_ENDPOINT_DIR_MASK) !=
            LIBUSB_ENDPOINT_OUT ||
        selector_.transfer_timeout_ms == 0U) {
        throw std::invalid_argument("USB Vendor Bulk 选择参数无效");
    }

    int result = libusb_init(&context_);
    if (result < 0) {
        throw usb_error("初始化 libusb 失败", result);
    }
    try {
        libusb_device** devices = nullptr;
        const ssize_t count = libusb_get_device_list(context_, &devices);
        if (count < 0) {
            throw usb_error("枚举 USB 设备失败", static_cast<int>(count));
        }
        std::vector<libusb_device_handle*> matches;
        for (ssize_t index = 0; index < count; ++index) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[index], &descriptor) < 0 ||
                descriptor.idVendor != selector_.vendor_id ||
                descriptor.idProduct != selector_.product_id) {
                continue;
            }
            libusb_device_handle* candidate = nullptr;
            result = libusb_open(devices[index], &candidate);
            if (result < 0) {
                continue;
            }
            if (serial_matches(candidate, descriptor,
                               selector_.serial_number)) {
                matches.push_back(candidate);
            } else {
                libusb_close(candidate);
            }
        }
        libusb_free_device_list(devices, 1);
        if (matches.empty()) {
            throw std::runtime_error("未找到匹配的 RemoteBSP USB 设备");
        }
        if (matches.size() > 1U) {
            for (auto* match : matches) {
                libusb_close(match);
            }
            throw std::runtime_error(
                "存在多个相同 VID/PID 的 USB 设备，请在选择器中指定 @SERIAL");
        }
        handle_ = matches.front();

        result = libusb_kernel_driver_active(
            handle_, selector_.interface_number);
        if (result == 1) {
            result = libusb_detach_kernel_driver(
                handle_, selector_.interface_number);
            if (result < 0) {
                throw usb_error("分离 USB 内核驱动失败", result);
            }
            kernel_driver_detached_ = true;
        } else if (result < 0 && result != LIBUSB_ERROR_NOT_SUPPORTED) {
            throw usb_error("查询 USB 内核驱动状态失败", result);
        }
        result = libusb_claim_interface(handle_, selector_.interface_number);
        if (result < 0) {
            throw usb_error("占用 RemoteBSP USB 接口失败", result);
        }
        interface_claimed_ = true;
    } catch (...) {
        close_device();
        throw;
    }
}

LibusbTransport::~LibusbTransport() { close_device(); }

void LibusbTransport::send(const LinkFrame& frame) {
    auto wire = encode_usb_frame(frame);
    int transferred = 0;
    const int result = libusb_bulk_transfer(
        handle_, selector_.output_endpoint, wire.data(),
        static_cast<int>(wire.size()), &transferred,
        selector_.transfer_timeout_ms);
    if (result < 0) {
        throw usb_error("发送 USB Bulk 数据失败", result);
    }
    if (transferred != static_cast<int>(wire.size())) {
        throw std::runtime_error("发送 USB Bulk 数据时发生短写");
    }
}

std::optional<LinkFrame> LibusbTransport::receive(
    std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero() ||
        timeout.count() > std::numeric_limits<unsigned>::max()) {
        throw std::invalid_argument("USB 接收超时参数无效");
    }
    if (const auto ready = decoder_.pop(); ready.has_value()) {
        return ready;
    }
    std::array<unsigned char, 4096> chunk{};
    int transferred = 0;
    /* libusb 的 0 表示无限等待；LinkTransport 的 0 表示立即轮询。 */
    const unsigned timeout_ms = timeout == std::chrono::milliseconds::zero()
                                    ? 1U
                                    : static_cast<unsigned>(timeout.count());
    const int result = libusb_bulk_transfer(
        handle_, selector_.input_endpoint, chunk.data(),
        static_cast<int>(chunk.size()), &transferred, timeout_ms);
    if (result == LIBUSB_ERROR_TIMEOUT) {
        return std::nullopt;
    }
    if (result < 0) {
        throw usb_error("接收 USB Bulk 数据失败", result);
    }
    if (transferred <= 0) {
        return std::nullopt;
    }
    decoder_.append(chunk.data(), static_cast<std::size_t>(transferred));
    return decoder_.pop();
}

LinkCapabilities LibusbTransport::capabilities() const noexcept {
    return {LinkKind::Usb, kUsbLogicalMtu, true, true, false, 12000000U};
}

const UsbDeviceSelector& LibusbTransport::selector() const noexcept {
    return selector_;
}

void LibusbTransport::close_device() noexcept {
    if (handle_ != nullptr) {
        if (interface_claimed_) {
            static_cast<void>(libusb_release_interface(
                handle_, selector_.interface_number));
            interface_claimed_ = false;
        }
        if (kernel_driver_detached_) {
            static_cast<void>(libusb_attach_kernel_driver(
                handle_, selector_.interface_number));
            kernel_driver_detached_ = false;
        }
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (context_ != nullptr) {
        libusb_exit(context_);
        context_ = nullptr;
    }
}

}
