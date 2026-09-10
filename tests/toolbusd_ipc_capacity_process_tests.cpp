#include "remotebsp/client.hpp"

#include <sys/socket.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << __FILE__ << ':' << __LINE__ \
    << " 检查失败: " #c << '\n'; ++failures; } } while (false)

class TestDirectory {
public:
    TestDirectory() {
        std::string pattern = "/tmp/remotebsp-ipc-capacity-XXXXXX";
        pattern.push_back('\0');
        auto* path = ::mkdtemp(pattern.data());
        if (path == nullptr) throw std::runtime_error("创建测试目录失败");
        path_ = path;
    }
    ~TestDirectory() { std::filesystem::remove_all(path_); }
    std::string child(const char* name) const { return (path_ / name).string(); }
private:
    std::filesystem::path path_;
};

class Child {
public:
    explicit Child(pid_t pid) : pid_(pid) {}
    ~Child() {
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGTERM));
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
    }
private:
    pid_t pid_;
};

pid_t spawn(const std::vector<std::string>& arguments) {
    const auto pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork 失败");
    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        ::_exit(127);
    }
    return pid;
}

bool wait_for_socket(const std::string& path) {
    for (int retry = 0; retry < 500; ++retry) {
        struct stat status {};
        if (::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int slow_client(const std::string& path) {
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) throw std::runtime_error("创建客户端套接字失败");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        throw std::runtime_error("套接字路径过长");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(socket);
        throw std::runtime_error("连接 toolbusd 失败");
    }
    // 只发送长度头的第一个字节，稳定占住一个正在限时读取的工作槽。
    const unsigned char partial = 1U;
    if (::send(socket, &partial, 1U, MSG_NOSIGNAL) != 1) {
        ::close(socket);
        throw std::runtime_error("发送部分 IPC 帧失败");
    }
    return socket;
}

int connected_client(const std::string& path) {
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) throw std::runtime_error("创建客户端套接字失败");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(socket);
        throw std::runtime_error("套接字路径过长");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(socket);
        throw std::runtime_error("连接 toolbusd 失败");
    }
    return socket;
}

bool peer_closes(int socket, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor{socket, POLLIN | POLLHUP, 0};
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                      std::chrono::steady_clock::now());
        const int result = ::poll(&descriptor, 1,
                                  static_cast<int>(remaining.count()));
        if (result <= 0) return false;
        unsigned char bytes[64]{};
        const auto received = ::recv(socket, bytes, sizeof(bytes), 0);
        if (received <= 0) return true;
    }
    return false;
}

bool list_nodes_succeeds(const std::string& ipc,
                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            remotebsp::Client client(ipc, 1U);
            static_cast<void>(client.list_nodes());
            return true;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return false;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    TestDirectory directory;
    const auto ipc = directory.child("toolbusd.sock");
    const auto usb = directory.child("usb.sock");
    const auto ledger = directory.child("ledger");
    Child daemon(spawn({argv[1], usb, "usb-mock", ipc,
                        "--runtime-operation-ledger-dir", ledger,
                        "--max-ipc-clients", "4"}));
    CHECK(wait_for_socket(ipc));

    std::vector<int> slow;
    slow.push_back(slow_client(ipc));
    CHECK(list_nodes_succeeds(ipc, std::chrono::milliseconds(500)));

    // 超过64 KiB的长度头在分配帧体前关闭，仅影响该连接。
    const int oversized = connected_client(ipc);
    const unsigned char oversized_length[4]{1U, 0U, 1U, 0U}; // 65537，小端
    CHECK(::send(oversized, oversized_length, sizeof(oversized_length),
                 MSG_NOSIGNAL) == 4);
    CHECK(peer_closes(oversized, std::chrono::milliseconds(500)));
    ::close(oversized);
    CHECK(list_nodes_succeeds(ipc, std::chrono::milliseconds(500)));

    // 加上三个慢连接达到四槽上限。超限客户端可以被拒绝，但不得创建
    // 第五个工作线程；释放任意一槽后，正常客户端应立即恢复，而无需等
    // 其他慢客户端的两秒截止时间。
    for (int index = 0; index < 3; ++index) {
        slow.push_back(slow_client(ipc));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!list_nodes_succeeds(ipc, std::chrono::milliseconds(200)));
    ::close(slow.back());
    slow.pop_back();
    CHECK(list_nodes_succeeds(ipc, std::chrono::milliseconds(500)));

    // 再次填满配额但不主动释放，半开连接应在2秒请求期限后自动退出；
    // 正常客户端随后恢复，证明配额不会被永久占用。
    slow.push_back(slow_client(ipc));
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    CHECK(list_nodes_succeeds(ipc, std::chrono::milliseconds(700)));

    // 客户端发出合法短请求后故意不读取响应。响应本身受固定帧上限和
    // 发送期限约束，不应阻塞另一个正常客户端。
    const int slow_reader = connected_client(ipc);
    const unsigned char list_nodes_request[5]{1U, 0U, 0U, 0U, 1U};
    CHECK(::send(slow_reader, list_nodes_request,
                 sizeof(list_nodes_request), MSG_NOSIGNAL) == 5);
    CHECK(list_nodes_succeeds(ipc, std::chrono::milliseconds(500)));
    ::close(slow_reader);

    const auto health = remotebsp::Client(ipc, 1U).health_snapshot();
    CHECK(health.ipc_active_clients >= 1U);
    CHECK(health.ipc_active_clients <= health.ipc_maximum_clients);
    CHECK(health.ipc_maximum_clients == 4U);
    CHECK(health.ipc_peak_clients == 4U);
    CHECK(health.ipc_accepted_total >= 8U);
    CHECK(health.ipc_capacity_rejected_total >= 1U);
    CHECK(health.ipc_oversized_frame_total == 1U);
    CHECK(health.ipc_timeout_total >= 1U);
    CHECK(health.ipc_thread_creation_failed_total == 0U);

    for (const auto socket : slow) ::close(socket);
    return failures == 0 ? 0 : 1;
}
