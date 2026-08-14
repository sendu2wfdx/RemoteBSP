#pragma once

#include "remotebsp/transport/link_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace remotebsp::transport {

constexpr std::uint8_t kUsbFrameVersion = 1U;
constexpr std::size_t kUsbFrameHeaderSize = 12U;
/* 第一版沿用现有 6 位分片长度字段，后续可在 USB 适配器内批量合并。 */
constexpr std::size_t kUsbLogicalMtu = 64U;
constexpr std::size_t kMaximumUsbDecoderBuffer = 16U * 1024U;

enum class UsbFrameError {
    InvalidMagic,
    UnsupportedVersion,
    InvalidFlags,
    InvalidLength,
    EmptyPayload,
    BufferOverflow,
};

class UsbFrameException : public std::runtime_error {
public:
    UsbFrameException(UsbFrameError code, const char* message);
    UsbFrameError code() const noexcept;

private:
    UsbFrameError code_;
};

std::vector<std::uint8_t> encode_usb_frame(const LinkFrame& frame);

class UsbFrameDecoder {
public:
    void append(const std::uint8_t* data, std::size_t size);
    void append(const std::vector<std::uint8_t>& data) {
        append(data.data(), data.size());
    }
    std::optional<LinkFrame> pop();
    std::size_t buffered_size() const noexcept;
    void clear() noexcept;

private:
    std::vector<std::uint8_t> buffer_;
};

}
