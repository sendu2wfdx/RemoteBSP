#include "remotebsp/transport/usb_frame_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace remotebsp::transport {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{{'R', 'B', 'U', '1'}};

void put_u16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::uint8_t* output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        output[shift / 8U] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint16_t get_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint32_t get_u32(const std::uint8_t* input) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(input[shift / 8U]) << shift;
    }
    return value;
}

}

UsbFrameException::UsbFrameException(UsbFrameError code,
                                     const char* message)
    : std::runtime_error(message), code_(code) {}

UsbFrameError UsbFrameException::code() const noexcept { return code_; }

std::vector<std::uint8_t> encode_usb_frame(const LinkFrame& frame) {
    if (frame.data.empty()) {
        throw UsbFrameException(UsbFrameError::EmptyPayload,
                                "USB 链路帧载荷不能为空");
    }
    if (frame.data.size() > kUsbLogicalMtu ||
        frame.data.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw UsbFrameException(UsbFrameError::InvalidLength,
                                "USB 链路帧超过逻辑 MTU");
    }
    std::vector<std::uint8_t> wire(kUsbFrameHeaderSize + frame.data.size());
    std::copy(kMagic.begin(), kMagic.end(), wire.begin());
    wire[4] = kUsbFrameVersion;
    wire[5] = 0U;
    put_u16(wire.data() + 6U,
            static_cast<std::uint16_t>(frame.data.size()));
    put_u32(wire.data() + 8U, frame.route);
    std::copy(frame.data.begin(), frame.data.end(),
              wire.begin() + static_cast<std::ptrdiff_t>(kUsbFrameHeaderSize));
    return wire;
}

void UsbFrameDecoder::append(const std::uint8_t* data, std::size_t size) {
    if (size != 0U && data == nullptr) {
        throw std::invalid_argument("USB 解码输入指针不能为空");
    }
    if (size == 0U) {
        return;
    }
    if (size > kMaximumUsbDecoderBuffer - buffer_.size()) {
        throw UsbFrameException(UsbFrameError::BufferOverflow,
                                "USB 接收缓冲区超过上限");
    }
    buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<LinkFrame> UsbFrameDecoder::pop() {
    if (buffer_.size() < kUsbFrameHeaderSize) {
        return std::nullopt;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), buffer_.begin())) {
        throw UsbFrameException(UsbFrameError::InvalidMagic,
                                "USB 链路帧魔数无效");
    }
    if (buffer_[4] != kUsbFrameVersion) {
        throw UsbFrameException(UsbFrameError::UnsupportedVersion,
                                "USB 链路帧版本不兼容");
    }
    if (buffer_[5] != 0U) {
        throw UsbFrameException(UsbFrameError::InvalidFlags,
                                "USB 链路帧包含未知标志");
    }
    const std::size_t payload_size = get_u16(buffer_.data() + 6U);
    if (payload_size == 0U) {
        throw UsbFrameException(UsbFrameError::EmptyPayload,
                                "USB 链路帧载荷不能为空");
    }
    if (payload_size > kUsbLogicalMtu) {
        throw UsbFrameException(UsbFrameError::InvalidLength,
                                "USB 链路帧声明长度超过逻辑 MTU");
    }
    const std::size_t total_size = kUsbFrameHeaderSize + payload_size;
    if (buffer_.size() < total_size) {
        return std::nullopt;
    }
    LinkFrame frame;
    frame.route = get_u32(buffer_.data() + 8U);
    frame.data.assign(
        buffer_.begin() + static_cast<std::ptrdiff_t>(kUsbFrameHeaderSize),
        buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(total_size));
    return frame;
}

std::size_t UsbFrameDecoder::buffered_size() const noexcept {
    return buffer_.size();
}

void UsbFrameDecoder::clear() noexcept { buffer_.clear(); }

}
