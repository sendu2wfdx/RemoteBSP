#pragma once

#include "remotebsp/transport/can_transport.hpp"

#include <string>

namespace remotebsp::transport {

class SocketCanTransport final : public CanTransport {
public:
    SocketCanTransport(std::string interface_name, CanMode mode);
    ~SocketCanTransport() override;

    SocketCanTransport(const SocketCanTransport&) = delete;
    SocketCanTransport& operator=(const SocketCanTransport&) = delete;
    SocketCanTransport(SocketCanTransport&& other) noexcept;
    SocketCanTransport& operator=(SocketCanTransport&& other) noexcept;

    void send(const CanMessage& message) override;
    std::optional<CanMessage> receive(
        std::chrono::milliseconds timeout) override;
    std::size_t mtu() const noexcept override;
    CanMode mode() const noexcept override;

private:
    void close_socket() noexcept;

    int socket_{-1};
    CanMode mode_;
};

}
