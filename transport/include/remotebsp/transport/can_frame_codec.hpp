#pragma once

#include "remotebsp/transport/can_transport.hpp"

#include <linux/can.h>

#include <stdexcept>

namespace remotebsp::transport {

enum class CanFrameError {
    PayloadTooLarge,
    InvalidIdentifier,
    InvalidLength,
    RemoteTransmissionRequest,
    ErrorFrame,
};

class CanFrameException : public std::runtime_error {
public:
    CanFrameException(CanFrameError code, const char* message);
    CanFrameError code() const noexcept;

private:
    CanFrameError code_;
};

can_frame encode_classical_frame(const CanMessage& message);
CanMessage decode_classical_frame(const can_frame& frame);

canfd_frame encode_fd_frame(const CanMessage& message);
CanMessage decode_fd_frame(const canfd_frame& frame);

}
