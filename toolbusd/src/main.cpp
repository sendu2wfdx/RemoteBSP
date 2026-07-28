#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"
#include "remotebsp/transport/socketcan_transport.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kBroadcastRequestCanId = 0x700;
constexpr std::uint32_t kNodeRequestBaseCanId = 0x600;
constexpr std::uint32_t kNodeResponseBaseCanId = 0x580;
constexpr std::uint32_t kNodeHeartbeatBaseCanId = 0x500;
constexpr std::uint32_t kProvisionalResponseBaseCanId = 0x480;
constexpr std::uint32_t kMaximumNodeId = 127;
constexpr std::size_t kMaximumQueuedEventsPerNode = 256;
constexpr auto kDiscoveryInterval = std::chrono::milliseconds(2000);

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) { stop_requested = 1; }

remotebsp::transport::CanMode parse_mode(const std::string& text) {
    if (text == "classical") {
        return remotebsp::transport::CanMode::Classical;
    }
    if (text == "fd") {
        return remotebsp::transport::CanMode::FlexibleDataRate;
    }
    throw std::invalid_argument("模式必须是 classical 或 fd");
}

std::uint64_t request_key(std::uint32_t session_id,
                          std::uint32_t request_id) {
    return (static_cast<std::uint64_t>(session_id) << 32U) | request_id;
}

std::vector<std::uint8_t> text_body(const std::string& text) {
    return {text.begin(), text.end()};
}

std::uint32_t make_session_id() noexcept {
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t mixed =
        ticks ^ (static_cast<std::uint64_t>(::getpid()) << 32U);
    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    const auto value = static_cast<std::uint32_t>(mixed);
    return value == 0 ? 1U : value;
}

class ToolbusDaemon {
public:
    ToolbusDaemon(const std::string& interface_name,
                  remotebsp::transport::CanMode mode,
                  std::string socket_path)
        : transport_(interface_name, mode),
          fragmenter_(transport_.mtu()),
          reassembler_(transport_.mtu(), std::chrono::milliseconds(500)),
          socket_path_(std::move(socket_path)) {}

    ~ToolbusDaemon() {
        stop();
        close_server();
    }

    void run() {
        open_server();
        running_ = true;
        worker_ = std::thread(&ToolbusDaemon::can_loop, this);
        std::cout << "toolbusd 已启动，本地套接字: " << socket_path_
                  << '\n';

        while (stop_requested == 0) {
            pollfd descriptor{server_socket_, POLLIN, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "等待本地 IPC 连接失败");
            }
            if (result == 0 || (descriptor.revents & POLLIN) == 0) {
                continue;
            }
            const int client = ::accept(server_socket_, nullptr, nullptr);
            if (client < 0 && errno == EINTR) {
                continue;
            }
            if (client < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "接受本地 IPC 连接失败");
            }
            const timeval timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                ++active_clients_;
            }
            std::thread([this, client] {
                handle_client(client);
                ::close(client);
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    --active_clients_;
                }
                clients_finished_.notify_all();
            }).detach();
        }
        stop();
        std::cout << "toolbusd 已退出\n";
    }

private:
    void send_packet(const remotebsp::protocol::Packet& packet,
                     std::uint32_t can_id) {
        /*
         * 同一把锁同时保护传输 ID 分配和整包发送。这样多个 IPC 客户端
         * 不会竞争 next_transfer_id_，不同远程包的分片也不会相互穿插。
         */
        std::lock_guard<std::mutex> lock(send_mutex_);
        const auto frames = fragmenter_.split(
            remotebsp::protocol::encode(packet), next_transfer_id_++);
        for (const auto& frame : frames) {
            transport_.send({can_id, false, frame});
        }
    }

    void can_loop() {
        auto next_discovery = std::chrono::steady_clock::now();
        while (running_) {
            try {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_discovery) {
                    send_discovery();
                    next_discovery = now + kDiscoveryInterval;
                }

                const auto message =
                    transport_.receive(std::chrono::milliseconds(50));
                if (message.has_value() &&
                    !message->extended_identifier &&
                    ((message->identifier >
                          kNodeResponseBaseCanId &&
                      message->identifier <=
                          kNodeResponseBaseCanId + kMaximumNodeId) ||
                     (message->identifier >
                          kNodeHeartbeatBaseCanId &&
                      message->identifier <=
                          kNodeHeartbeatBaseCanId + kMaximumNodeId) ||
                     (message->identifier >
                          kProvisionalResponseBaseCanId &&
                      message->identifier <=
                          kProvisionalResponseBaseCanId +
                              kMaximumNodeId))) {
                    handle_can_message(*message);
                }
                process_request_timers();
            } catch (const std::exception& error) {
                std::cerr << "CAN 事件循环错误: " << error.what() << '\n';
            }
        }
    }

    void send_discovery() {
        remotebsp::protocol::Packet request;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            request = nodes_.make_discovery_request(next_control_request_id_++);
        }
        send_packet(request, kBroadcastRequestCanId);
    }

    void handle_can_message(
        const remotebsp::transport::CanMessage& message) {
        const auto result =
            reassembler_.accept(message.identifier, message.data);
        if (result.status !=
                remotebsp::protocol::ReassemblyStatus::Complete ||
            !result.packet.has_value()) {
            return;
        }
        const auto packet = remotebsp::protocol::decode(*result.packet);

        std::optional<remotebsp::protocol::Packet> assignment;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (packet.header.command == static_cast<std::uint16_t>(
                    remotebsp::protocol::Command::DiscoveryResponse)) {
                const auto update =
                    nodes_.accept_discovery_response(packet);
                const auto identity =
                    remotebsp::protocol::decode_node_identity(packet.payload);
                /*
                 * 已知节点若重新从临时响应 ID 发出发现响应，说明 MCU
                 * 已复位并丢失了 RAM 中的节点 ID。保留原 ID，但撤销
                 * 心跳确认，让下方逻辑重新发送 NODE_ASSIGN。
                 */
                if (message.identifier > kProvisionalResponseBaseCanId &&
                    message.identifier <=
                        kProvisionalResponseBaseCanId + kMaximumNodeId) {
                    nodes_.mark_assignment_unconfirmed(identity.uuid);
                }
                const auto* node = nodes_.find(identity.uuid);
                if (update == remotebsp::toolbusd::NodeUpdate::Added) {
                    if (next_node_id_ > kMaximumNodeId) {
                        throw std::runtime_error(
                            "可分配节点 ID 已耗尽");
                    }
                    assignment = nodes_.make_node_assignment(
                        identity.uuid, next_node_id_++,
                        next_control_request_id_++);
                } else if (node != nullptr && !node->assigned &&
                           node->node_id != 0) {
                    /*
                     * NODE_ASSIGN 或其响应可能丢失。节点在发出带 ID 的
                     * 心跳前都视为未确认；每次发现响应重发同一个 ID，
                     * 避免消耗新 ID，也保证重试幂等。
                     */
                    assignment = nodes_.make_node_assignment(
                        identity.uuid, node->node_id,
                        next_control_request_id_++);
                }
            } else if (packet.header.command ==
                       static_cast<std::uint16_t>(
                           remotebsp::protocol::Command::Heartbeat)) {
                nodes_.accept_heartbeat(packet);
            } else if (packet.header.message_type ==
                       remotebsp::protocol::MessageType::Event) {
                if (message.identifier <= kNodeHeartbeatBaseCanId ||
                    message.identifier >
                        kNodeHeartbeatBaseCanId + kMaximumNodeId) {
                    return;
                }
                const auto node_id =
                    message.identifier - kNodeHeartbeatBaseCanId;
                auto& queue = event_queues_[node_id];
                if (queue.size() == kMaximumQueuedEventsPerNode) {
                    queue.pop_front();
                }
                queue.push_back(packet);
                state_changed_.notify_all();
            } else {
                if (message.identifier <= kNodeResponseBaseCanId ||
                    message.identifier >
                        kNodeResponseBaseCanId + kMaximumNodeId) {
                    return;
                }
                const auto key = request_key(
                    packet.header.session_id, packet.header.request_id);
                const auto target = request_can_ids_.find(key);
                const auto response_node_id =
                    message.identifier - kNodeResponseBaseCanId;
                if (target == request_can_ids_.end() ||
                    target->second !=
                        kNodeRequestBaseCanId + response_node_id) {
                    return;
                }
                const auto response =
                    requests_.accept_response(packet);
                if (response.status ==
                        remotebsp::toolbusd::ResponseStatus::Matched &&
                    response.response.has_value()) {
                    responses_[key] = *response.response;
                    state_changed_.notify_all();
                }
            }
        }
        if (assignment.has_value()) {
            send_packet(*assignment, kBroadcastRequestCanId);
        }
    }

    void process_request_timers() {
        struct Retry {
            remotebsp::protocol::Packet packet;
            std::uint32_t can_id{};
        };
        std::vector<Retry> retries;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (const auto& event : requests_.poll()) {
                if (event.type ==
                        remotebsp::toolbusd::RequestEventType::Retry &&
                    event.packet.has_value()) {
                    const auto key =
                        request_key(event.session_id, event.request_id);
                    const auto target = request_can_ids_.find(key);
                    if (target != request_can_ids_.end()) {
                        retries.push_back({*event.packet, target->second});
                    }
                } else if (event.type ==
                           remotebsp::toolbusd::RequestEventType::TimedOut) {
                    timed_out_.insert(
                        request_key(event.session_id, event.request_id));
                    request_can_ids_.erase(
                        request_key(event.session_id, event.request_id));
                    state_changed_.notify_all();
                }
            }
            if (!nodes_.expire().empty()) {
                state_changed_.notify_all();
            }
        }
        for (const auto& retry : retries) {
            send_packet(retry.packet, retry.can_id);
        }
    }

    void handle_client(int client) {
        try {
            auto ipc_request =
                remotebsp::toolbusd::read_ipc_request(client);
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::ListNodes) {
                std::vector<remotebsp::toolbusd::IpcNodeInfo> result;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    for (const auto& node : nodes_.records()) {
                        result.push_back(
                            {node.identity.uuid, node.node_id, node.online,
                             node.assigned,
                             node.identity.firmware_major,
                             node.identity.firmware_minor,
                             node.identity.firmware_patch,
                             node.identity.board_type,
                             node.identity.protocol_version});
                    }
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_node_list(result));
                return;
            }
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::NextEvent) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online ||
                    !node->assigned) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                const bool available = state_changed_.wait_for(
                    lock, std::chrono::milliseconds(1000), [&] {
                        const auto found =
                            event_queues_.find(ipc_request.node_id);
                        const auto* current =
                            nodes_.find_by_node_id(ipc_request.node_id);
                        return !running_ ||
                               (found != event_queues_.end() &&
                                !found->second.empty()) ||
                               current == nullptr || !current->online;
                    });
                auto found = event_queues_.find(ipc_request.node_id);
                if (available && found != event_queues_.end() &&
                    !found->second.empty()) {
                    auto event = std::move(found->second.front());
                    found->second.pop_front();
                    const auto encoded =
                        remotebsp::protocol::encode(std::move(event));
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::Ok,
                        encoded);
                    return;
                }
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::TimedOut, {});
                return;
            }
            auto request = std::move(ipc_request.packet);
            if (request.header.message_type !=
                remotebsp::protocol::MessageType::Request) {
                throw std::invalid_argument("本地客户端只能提交请求消息");
            }
            // 会话由守护进程拥有，避免客户端伪造会话，也避免 toolbusd
            // 重启后请求 ID 从头计数时与 MCU 中的旧去重缓存冲突。
            request.header.session_id = session_id_;

            remotebsp::toolbusd::Submission submission;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online ||
                    !node->assigned) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                submission = requests_.submit(std::move(request));
                request_can_ids_[request_key(
                    submission.packet.header.session_id,
                    submission.request_id)] =
                    kNodeRequestBaseCanId + ipc_request.node_id;
            }
            send_packet(submission.packet,
                        kNodeRequestBaseCanId + ipc_request.node_id);
            const auto key = request_key(
                submission.packet.header.session_id, submission.request_id);

            std::unique_lock<std::mutex> lock(state_mutex_);
            state_changed_.wait(lock, [&] {
                return !running_ || responses_.find(key) != responses_.end() ||
                       timed_out_.find(key) != timed_out_.end();
            });
            const auto response = responses_.find(key);
            if (response != responses_.end()) {
                const auto encoded =
                    remotebsp::protocol::encode(response->second);
                responses_.erase(response);
                request_can_ids_.erase(key);
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok, encoded);
                return;
            }
            timed_out_.erase(key);
            lock.unlock();
            remotebsp::toolbusd::write_ipc_response(
                client, remotebsp::toolbusd::IpcStatus::TimedOut,
                text_body("远端请求超时"));
        } catch (const std::exception& error) {
            try {
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Error,
                    text_body(error.what()));
            } catch (...) {
            }
        }
    }

    void open_server() {
        if (socket_path_.empty() ||
            socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::invalid_argument("本地套接字路径无效或过长");
        }
        struct stat existing {};
        if (::lstat(socket_path_.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) {
                throw std::runtime_error(
                    "本地套接字路径已存在且不是套接字");
            }
            if (::unlink(socket_path_.c_str()) < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "清理旧本地套接字失败");
            }
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(),
                                    "检查本地套接字路径失败");
        }

        server_socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_socket_ < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "创建本地 IPC 套接字失败");
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path_.c_str(),
                    socket_path_.size() + 1U);
        if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) < 0 ||
            ::listen(server_socket_, 8) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "绑定本地 IPC 套接字失败");
        }
    }

    void stop() {
        if (running_.exchange(false)) {
            state_changed_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        std::unique_lock<std::mutex> lock(client_mutex_);
        clients_finished_.wait(lock,
                               [this] { return active_clients_ == 0; });
    }

    void close_server() noexcept {
        if (server_socket_ >= 0) {
            ::close(server_socket_);
            server_socket_ = -1;
        }
        if (!socket_path_.empty()) {
            struct stat existing {};
            if (::lstat(socket_path_.c_str(), &existing) == 0 &&
                S_ISSOCK(existing.st_mode)) {
                ::unlink(socket_path_.c_str());
            }
        }
    }

    remotebsp::transport::SocketCanTransport transport_;
    remotebsp::protocol::Fragmenter fragmenter_;
    remotebsp::protocol::Reassembler reassembler_;
    remotebsp::toolbusd::RequestManager requests_;
    remotebsp::toolbusd::NodeRegistry nodes_;
    std::string socket_path_;
    int server_socket_{-1};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex client_mutex_;
    std::condition_variable clients_finished_;
    std::size_t active_clients_{0};
    std::mutex send_mutex_;
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::unordered_map<std::uint64_t, remotebsp::protocol::Packet> responses_;
    std::unordered_set<std::uint64_t> timed_out_;
    std::unordered_map<std::uint64_t, std::uint32_t> request_can_ids_;
    std::unordered_map<
        std::uint32_t, std::deque<remotebsp::protocol::Packet>>
        event_queues_;
    std::atomic<std::uint16_t> next_transfer_id_{1};
    std::uint32_t next_control_request_id_{0x80000000U};
    std::uint32_t next_node_id_{1};
    const std::uint32_t session_id_{make_session_id()};
};

}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "用法: toolbusd <SocketCAN接口> <classical|fd> "
                     "[Unix套接字路径]\n";
        return 2;
    }
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        ToolbusDaemon daemon(argv[1], parse_mode(argv[2]),
                             argc == 4 ? argv[3] : "/tmp/toolbusd.sock");
        daemon.run();
    } catch (const std::exception& error) {
        std::cerr << "toolbusd 启动失败: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
