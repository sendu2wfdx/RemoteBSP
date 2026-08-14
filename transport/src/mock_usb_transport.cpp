#include "remotebsp/transport/mock_usb_transport.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace remotebsp::transport {
namespace {

[[noreturn]] void throw_system_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

sockaddr_un make_address(const std::string& path) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("Mock USB 套接字路径无效或过长");
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    return address;
}

void write_all(int descriptor, const std::vector<std::uint8_t>& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::send(
            descriptor, data.data() + offset, data.size() - offset,
            MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            throw_system_error("发送 Mock USB 数据失败");
        }
        if (written == 0) {
            throw std::runtime_error("Mock USB 连接发生短写");
        }
        offset += static_cast<std::size_t>(written);
    }
}

}

MockUsbTransport::MockUsbTransport(std::string socket_path, MockUsbRole role)
    : socket_path_(std::move(socket_path)), role_(role) {
    const auto address = make_address(socket_path_);
    try {
        if (role_ == MockUsbRole::Host) {
            struct stat existing {};
            if (::lstat(socket_path_.c_str(), &existing) == 0) {
                if (!S_ISSOCK(existing.st_mode)) {
                    throw std::runtime_error(
                        "Mock USB 路径已经存在且不是套接字");
                }
                if (::unlink(socket_path_.c_str()) < 0) {
                    throw_system_error("清理旧 Mock USB 套接字失败");
                }
            } else if (errno != ENOENT) {
                throw_system_error("检查 Mock USB 套接字失败");
            }
            listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (listener_ < 0) {
                throw_system_error("创建 Mock USB 监听套接字失败");
            }
            if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) < 0 ||
                ::listen(listener_, 1) < 0) {
                throw_system_error("启动 Mock USB 监听失败");
            }
            socket_ = ::accept(listener_, nullptr, nullptr);
            if (socket_ < 0) {
                throw_system_error("接受 Mock USB 设备连接失败");
            }
            ::close(listener_);
            listener_ = -1;
        } else {
            socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (socket_ < 0) {
                throw_system_error("创建 Mock USB 设备套接字失败");
            }
            constexpr unsigned kMaximumAttempts = 200U;
            for (unsigned attempt = 0; attempt < kMaximumAttempts; ++attempt) {
                if (::connect(socket_,
                              reinterpret_cast<const sockaddr*>(&address),
                              sizeof(address)) == 0) {
                    break;
                }
                if (errno != ENOENT && errno != ECONNREFUSED) {
                    throw_system_error("连接 Mock USB 主机失败");
                }
                if (attempt + 1U == kMaximumAttempts) {
                    throw_system_error("等待 Mock USB 主机超时");
                }
                ::usleep(10000U);
            }
        }
    } catch (...) {
        close_all();
        throw;
    }
}

MockUsbTransport::~MockUsbTransport() { close_all(); }

void MockUsbTransport::send(const LinkFrame& frame) {
    write_all(socket_, encode_usb_frame(frame));
}

std::optional<LinkFrame> MockUsbTransport::receive(
    std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero() ||
        timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("Mock USB 接收超时参数无效");
    }
    if (const auto ready = decoder_.pop(); ready.has_value()) {
        return ready;
    }
    pollfd descriptor{socket_, POLLIN, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw_system_error("等待 Mock USB 数据失败");
    }
    if (result == 0) {
        return std::nullopt;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("Mock USB 连接发生异常事件");
    }
    std::array<std::uint8_t, 4096> chunk{};
    const ssize_t received = ::recv(socket_, chunk.data(), chunk.size(), 0);
    if (received < 0) {
        throw_system_error("接收 Mock USB 数据失败");
    }
    if (received == 0) {
        throw std::runtime_error("Mock USB 设备已经断开");
    }
    decoder_.append(chunk.data(), static_cast<std::size_t>(received));
    return decoder_.pop();
}

LinkCapabilities MockUsbTransport::capabilities() const noexcept {
    return {LinkKind::MockUsb, kUsbLogicalMtu, true, true, false,
            12000000U};
}

void MockUsbTransport::close_all() noexcept {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    if (listener_ >= 0) {
        ::close(listener_);
        listener_ = -1;
    }
    if (role_ == MockUsbRole::Host && !socket_path_.empty()) {
        struct stat existing {};
        if (::lstat(socket_path_.c_str(), &existing) == 0 &&
            S_ISSOCK(existing.st_mode)) {
            ::unlink(socket_path_.c_str());
        }
    }
}

}
