#include "remotebsp/transport/can_frame_codec.hpp"

#include <algorithm>

namespace remotebsp::transport {
namespace {

std::size_t canonical_fd_length(std::size_t length) {
    if (length <= 8U) {
        return length;
    }
    constexpr std::size_t lengths[] = {12U, 16U, 20U, 24U,
                                       32U, 48U, 64U};
    for (const auto candidate : lengths) {
        if (length <= candidate) {
            return candidate;
        }
    }
    return CANFD_MAX_DLEN;
}

canid_t encode_identifier(const CanMessage& message) {
    const std::uint32_t maximum =
        message.extended_identifier ? CAN_EFF_MASK : CAN_SFF_MASK;
    if (message.identifier > maximum) {
        throw CanFrameException(CanFrameError::InvalidIdentifier,
                                "CAN 标识符超出范围");
    }
    canid_t identifier = message.identifier;
    if (message.extended_identifier) {
        identifier |= CAN_EFF_FLAG;
    }
    return identifier;
}

CanMessage decode_identifier(canid_t identifier) {
    if ((identifier & CAN_ERR_FLAG) != 0U) {
        throw CanFrameException(CanFrameError::ErrorFrame,
                                "不接受 CAN 错误帧");
    }
    if ((identifier & CAN_RTR_FLAG) != 0U) {
        throw CanFrameException(CanFrameError::RemoteTransmissionRequest,
                                "不接受 CAN 远程请求帧");
    }

    CanMessage message;
    message.extended_identifier = (identifier & CAN_EFF_FLAG) != 0U;
    message.identifier =
        identifier &
        (message.extended_identifier ? CAN_EFF_MASK : CAN_SFF_MASK);
    return message;
}

}

CanFrameException::CanFrameException(CanFrameError code, const char* message)
    : std::runtime_error(message), code_(code) {}

CanFrameError CanFrameException::code() const noexcept { return code_; }

can_frame encode_classical_frame(const CanMessage& message) {
    if (message.data.size() > CAN_MAX_DLEN) {
        throw CanFrameException(CanFrameError::PayloadTooLarge,
                                "Classical CAN 数据超过 8 字节");
    }
    can_frame frame{};
    frame.can_id = encode_identifier(message);
    frame.can_dlc = static_cast<__u8>(message.data.size());
    std::copy(message.data.begin(), message.data.end(), frame.data);
    return frame;
}

CanMessage decode_classical_frame(const can_frame& frame) {
    if (frame.can_dlc > CAN_MAX_DLEN) {
        throw CanFrameException(CanFrameError::InvalidLength,
                                "Classical CAN 帧长度无效");
    }
    CanMessage message = decode_identifier(frame.can_id);
    message.data.assign(frame.data, frame.data + frame.can_dlc);
    return message;
}

canfd_frame encode_fd_frame(const CanMessage& message) {
    if (message.data.size() > CANFD_MAX_DLEN) {
        throw CanFrameException(CanFrameError::PayloadTooLarge,
                                "CAN-FD 数据超过 64 字节");
    }
    canfd_frame frame{};
    frame.can_id = encode_identifier(message);
    // 真实控制器只接受标准 CAN-FD 长度，空余字节由零初始化补齐。
    frame.len =
        static_cast<__u8>(canonical_fd_length(message.data.size()));
    std::copy(message.data.begin(), message.data.end(), frame.data);
    return frame;
}

CanMessage decode_fd_frame(const canfd_frame& frame) {
    if (frame.len > CANFD_MAX_DLEN) {
        throw CanFrameException(CanFrameError::InvalidLength,
                                "CAN-FD 帧长度无效");
    }
    CanMessage message = decode_identifier(frame.can_id);
    message.data.assign(frame.data, frame.data + frame.len);
    return message;
}

}
