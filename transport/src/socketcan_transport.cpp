#include "remotebsp/transport/socketcan_transport.hpp"

#include "remotebsp/transport/can_frame_codec.hpp"

#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace remotebsp::transport {
namespace {

constexpr auto kSendRetryBudget = std::chrono::milliseconds{100};
constexpr useconds_t kSendRetrySleepUs = 1000U;

[[noreturn]] void throw_system_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

/*
 * CANable 等 USB 适配器常见很浅的内核发送队列。大包在 Classical CAN 下分片后
 * 可能短暂遇到 ENOBUFS；这不是链路失败，等待一个短时隙后重试即可。重试总时长
 * 有上界，不能让故障设备无限占用 toolbusd 事件循环。
 */
ssize_t write_with_backpressure_retry(int socket, const void* frame,
                                      std::size_t length) {
    const auto deadline = std::chrono::steady_clock::now() +
                          kSendRetryBudget;
    for (;;) {
        const ssize_t written = ::write(socket, frame, length);
        if (written >= 0) {
            return written;
        }
        const int error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK && error != ENOBUFS) {
            errno = error;
            return -1;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            errno = error;
            return -1;
        }
        /* 即使 POLLOUT 已置位，qdisc 仍可能暂未释放一个 CAN 帧槽位。 */
        ::usleep(kSendRetrySleepUs);
    }
}

}

SocketCanTransport::SocketCanTransport(std::string interface_name, CanMode mode)
    : mode_(mode) {
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
        throw std::invalid_argument("SocketCAN 接口名称无效");
    }

    socket_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_ < 0) {
        throw_system_error("创建 SocketCAN 套接字失败");
    }

    try {
        // vcan 会在极短时间内投递完整分片长包；真实 CAN 控制器虽然由线速自然
        // 限流，高优先级流量叠加时也可能形成接收突发。扩大队列可容纳最大
        // Remote Packet 的 Classical CAN 分片和一次重试，避免调度抖动造成丢帧。
        constexpr int kReceiveBufferBytes = 1024 * 1024;
        if (::setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                         &kReceiveBufferBytes,
                         sizeof(kReceiveBufferBytes)) < 0) {
            throw_system_error("设置 SocketCAN 接收缓冲失败");
        }
        if (mode_ == CanMode::FlexibleDataRate) {
            const int enable = 1;
            if (::setsockopt(socket_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable,
                             sizeof(enable)) < 0) {
                throw_system_error("启用 CAN-FD 失败");
            }
        }

        ifreq request{};
        std::memcpy(request.ifr_name, interface_name.c_str(),
                    interface_name.size() + 1U);
        if (::ioctl(socket_, SIOCGIFINDEX, &request) < 0) {
            throw_system_error("查询 SocketCAN 接口索引失败");
        }

        sockaddr_can address{};
        address.can_family = AF_CAN;
        address.can_ifindex = request.ifr_ifindex;
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) < 0) {
            throw_system_error("绑定 SocketCAN 接口失败");
        }
    } catch (...) {
        close_socket();
        throw;
    }
}

SocketCanTransport::~SocketCanTransport() { close_socket(); }

SocketCanTransport::SocketCanTransport(SocketCanTransport&& other) noexcept
    : socket_(std::exchange(other.socket_, -1)), mode_(other.mode_) {}

SocketCanTransport& SocketCanTransport::operator=(
    SocketCanTransport&& other) noexcept {
    if (this != &other) {
        close_socket();
        socket_ = std::exchange(other.socket_, -1);
        mode_ = other.mode_;
    }
    return *this;
}

void SocketCanTransport::send(const LinkFrame& link_frame) {
    CanMessage message;
    message.identifier = link_frame.route;
    message.data = link_frame.data;
    message.bit_rate_switch = mode_ == CanMode::FlexibleDataRate;
    ssize_t written = 0;
    std::size_t expected = 0;
    if (mode_ == CanMode::Classical) {
        const can_frame frame = encode_classical_frame(message);
        expected = sizeof(frame);
        written = write_with_backpressure_retry(socket_, &frame, expected);
    } else {
        const canfd_frame frame = encode_fd_frame(message);
        expected = sizeof(frame);
        written = write_with_backpressure_retry(socket_, &frame, expected);
    }
    if (written < 0) {
        throw_system_error("发送 CAN 帧失败");
    }
    if (static_cast<std::size_t>(written) != expected) {
        throw std::runtime_error("发送 CAN 帧时发生短写");
    }
}

std::optional<LinkFrame> SocketCanTransport::receive(
    std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero() ||
        timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("接收超时时间无效");
    }

    pollfd descriptor{socket_, POLLIN, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw_system_error("等待 CAN 帧失败");
    }
    if (result == 0) {
        return std::nullopt;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("SocketCAN 套接字发生异常事件");
    }

    if (mode_ == CanMode::Classical) {
        can_frame frame{};
        const ssize_t received = ::read(socket_, &frame, sizeof(frame));
        if (received < 0) {
            throw_system_error("接收 Classical CAN 帧失败");
        }
        if (static_cast<std::size_t>(received) != sizeof(frame)) {
            throw std::runtime_error("收到长度无效的 Classical CAN 帧");
        }
        const auto message = decode_classical_frame(frame);
        if (message.extended_identifier) {
            throw std::runtime_error("RemoteBSP 只接受标准 CAN 标识符");
        }
        return LinkFrame{message.identifier, message.data};
    }

    canfd_frame frame{};
    const ssize_t received = ::read(socket_, &frame, sizeof(frame));
    if (received < 0) {
        throw_system_error("接收 CAN-FD 帧失败");
    }
    if (static_cast<std::size_t>(received) != sizeof(frame)) {
        throw std::runtime_error("收到长度无效的 CAN-FD 帧");
    }
    const auto message = decode_fd_frame(frame);
    if (message.extended_identifier) {
        throw std::runtime_error("RemoteBSP 只接受标准 CAN 标识符");
    }
    return LinkFrame{message.identifier, message.data};
}

CanMode SocketCanTransport::mode() const noexcept { return mode_; }

void SocketCanTransport::close_socket() noexcept {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
}

}
