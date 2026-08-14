#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"
#include "remotebsp/toolbusd/request_manager.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"
#include "remotebsp/transport/link_routes.hpp"
#include "remotebsp/transport/link_transport.hpp"
#include "remotebsp/transport/libusb_transport.hpp"
#include "remotebsp/transport/mock_usb_transport.hpp"
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
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr auto kBroadcastRequestRoute =
    remotebsp::transport::kDiscoveryRoute;
constexpr auto kNodeRequestBaseRoute =
    remotebsp::transport::kNodeRequestBaseRoute;
constexpr auto kNodeResponseBaseRoute =
    remotebsp::transport::kNodeResponseBaseRoute;
constexpr auto kNodeEventBaseRoute =
    remotebsp::transport::kNodeEventBaseRoute;
constexpr auto kProvisionalResponseBaseRoute =
    remotebsp::transport::kProvisionalResponseBaseRoute;
constexpr auto kMaximumNodeId = remotebsp::transport::kMaximumNodeId;
constexpr std::size_t kMaximumQueuedEventsPerNode = 256;
constexpr std::size_t kUartStreamBufferCapacity = 64U * 1024U;
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

std::uint64_t uart_stream_key(std::uint32_t node_id,
                              std::uint32_t object_id) {
    return (static_cast<std::uint64_t>(node_id) << 32U) | object_id;
}

struct UartStreamBuffer {
    std::deque<std::uint8_t> bytes;
    std::uint64_t dropped_bytes{};
    std::uint64_t lost_events{};
    std::uint32_t last_sequence{};
    bool sequence_initialized{};
};

std::uint32_t parse_positive_u32(const std::string& text,
                                 const char* name) {
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint32_t>(value);
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
    ToolbusDaemon(
                  std::unique_ptr<remotebsp::transport::LinkTransport> transport,
                  std::string socket_path,
                  remotebsp::toolbusd::TrafficConfig traffic_config)
        : transport_(std::move(transport)),
          fragmenter_(transport_->mtu()),
          reassembler_(transport_->mtu(), std::chrono::milliseconds(500)),
          traffic_(std::move(traffic_config)),
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
    bool send_packet(
        const remotebsp::protocol::Packet& packet, std::uint32_t route,
        remotebsp::toolbusd::AdmissionPolicy policy =
            remotebsp::toolbusd::AdmissionPolicy::Enforce) {
        /*
         * 同一把锁同时保护传输 ID 分配和整包发送。这样多个 IPC 客户端
         * 不会竞争 next_transfer_id_，不同远程包的分片也不会相互穿插。
         */
        std::lock_guard<std::mutex> lock(send_mutex_);
        const auto frames = fragmenter_.split(
            remotebsp::protocol::encode(packet), next_transfer_id_++);
        std::vector<std::size_t> frame_lengths;
        frame_lengths.reserve(frames.size());
        for (const auto& frame : frames) {
            frame_lengths.push_back(frame.size());
        }
        const auto traffic_class =
            remotebsp::toolbusd::classify_traffic(packet);
        if (traffic_class ==
            remotebsp::toolbusd::TrafficClass::Safety) {
            policy =
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed;
        }
        if (!traffic_.admit(traffic_class, frame_lengths, policy)) {
            return false;
        }
        for (const auto& frame : frames) {
            transport_->send({route, frame});
        }
        return true;
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
                    transport_->receive(std::chrono::milliseconds(50));
                if (message.has_value() &&
                    ((message->route >
                          kNodeResponseBaseRoute &&
                      message->route <=
                          kNodeResponseBaseRoute + kMaximumNodeId) ||
                     (message->route >
                          kNodeEventBaseRoute &&
                      message->route <=
                          kNodeEventBaseRoute + kMaximumNodeId) ||
                     (message->route >
                          kProvisionalResponseBaseRoute &&
                      message->route <=
                          kProvisionalResponseBaseRoute +
                              kMaximumNodeId))) {
                    handle_link_frame(*message);
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
        static_cast<void>(
            send_packet(request, kBroadcastRequestRoute));
    }

    void handle_link_frame(
        const remotebsp::transport::LinkFrame& message) {
        const auto result =
            reassembler_.accept(message.route, message.data);
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
                if (message.route > kProvisionalResponseBaseRoute &&
                    message.route <=
                        kProvisionalResponseBaseRoute + kMaximumNodeId) {
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
                if (message.route <= kNodeEventBaseRoute ||
                    message.route >
                        kNodeEventBaseRoute + kMaximumNodeId) {
                    return;
                }
                const auto node_id =
                    message.route - kNodeEventBaseRoute;
                if (packet.header.command ==
                        static_cast<std::uint16_t>(
                            remotebsp::protocol::Command::UartRxEvent) &&
                    packet.header.object_id != 0U) {
                    auto& stream = uart_stream_buffers_[uart_stream_key(
                        node_id, packet.header.object_id)];
                    bool accept = true;
                    const auto sequence = packet.header.request_id;
                    if (sequence != 0U) {
                        if (!stream.sequence_initialized) {
                            stream.sequence_initialized = true;
                            stream.last_sequence = sequence;
                        } else {
                            const std::uint32_t distance =
                                sequence - stream.last_sequence;
                            if (distance == 0U ||
                                distance >= 0x80000000U) {
                                accept = false;
                            } else {
                                if (distance > 1U) {
                                    stream.lost_events += distance - 1U;
                                }
                                stream.last_sequence = sequence;
                            }
                        }
                    }
                    if (accept) {
                        for (const auto byte : packet.payload) {
                            if (stream.bytes.size() ==
                                kUartStreamBufferCapacity) {
                                stream.bytes.pop_front();
                                ++stream.dropped_bytes;
                            }
                            stream.bytes.push_back(byte);
                        }
                    }
                }
                auto& queue = event_queues_[node_id];
                if (queue.size() == kMaximumQueuedEventsPerNode) {
                    queue.pop_front();
                }
                queue.push_back(packet);
                state_changed_.notify_all();
            } else {
                if (message.route <= kNodeResponseBaseRoute ||
                    message.route >
                        kNodeResponseBaseRoute + kMaximumNodeId) {
                    return;
                }
                const auto key = request_key(
                    packet.header.session_id, packet.header.request_id);
                const auto target = request_routes_.find(key);
                const auto response_node_id =
                    message.route - kNodeResponseBaseRoute;
                if (target == request_routes_.end() ||
                    target->second !=
                        kNodeRequestBaseRoute + response_node_id) {
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
            static_cast<void>(send_packet(
                *assignment, kBroadcastRequestRoute,
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
        }
    }

    void process_request_timers() {
        struct Retry {
            remotebsp::protocol::Packet packet;
            std::uint32_t route{};
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
                    const auto target = request_routes_.find(key);
                    if (target != request_routes_.end()) {
                        retries.push_back({*event.packet, target->second});
                    }
                } else if (event.type ==
                           remotebsp::toolbusd::RequestEventType::TimedOut) {
                    timed_out_.insert(
                        request_key(event.session_id, event.request_id));
                    request_routes_.erase(
                        request_key(event.session_id, event.request_id));
                    state_changed_.notify_all();
                }
            }
            if (!nodes_.expire().empty()) {
                state_changed_.notify_all();
            }
        }
        for (const auto& retry : retries) {
            static_cast<void>(send_packet(
                retry.packet, retry.route,
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
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
                remotebsp::toolbusd::IpcRequestKind::TrafficStatus) {
                remotebsp::toolbusd::TrafficSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    snapshot = traffic_.snapshot();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_traffic_status(
                        snapshot));
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
            if (ipc_request.kind ==
                remotebsp::toolbusd::IpcRequestKind::UartStreamRead) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                const auto* node =
                    nodes_.find_by_node_id(ipc_request.node_id);
                if (node == nullptr || !node->online ||
                    !node->assigned) {
                    throw std::runtime_error(
                        "目标节点尚未发现或已经离线");
                }
                const auto key = uart_stream_key(
                    ipc_request.node_id, ipc_request.object_id);
                const bool available = state_changed_.wait_for(
                    lock,
                    std::chrono::milliseconds(ipc_request.timeout_ms),
                    [&] {
                        const auto found =
                            uart_stream_buffers_.find(key);
                        const auto* current =
                            nodes_.find_by_node_id(ipc_request.node_id);
                        return !running_ ||
                               (found != uart_stream_buffers_.end() &&
                                !found->second.bytes.empty()) ||
                               current == nullptr || !current->online;
                    });
                auto found = uart_stream_buffers_.find(key);
                if (!available || found == uart_stream_buffers_.end() ||
                    found->second.bytes.empty()) {
                    lock.unlock();
                    remotebsp::toolbusd::write_ipc_response(
                        client, remotebsp::toolbusd::IpcStatus::TimedOut,
                        {});
                    return;
                }
                remotebsp::toolbusd::UartStreamChunk chunk;
                chunk.dropped_bytes = found->second.dropped_bytes;
                chunk.lost_events = found->second.lost_events;
                const auto count = std::min<std::size_t>(
                    ipc_request.maximum_length,
                    found->second.bytes.size());
                chunk.data.reserve(count);
                for (std::size_t index = 0; index < count; ++index) {
                    chunk.data.push_back(found->second.bytes.front());
                    found->second.bytes.pop_front();
                }
                lock.unlock();
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_uart_stream_chunk(
                        chunk));
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
                request_routes_[request_key(
                    submission.packet.header.session_id,
                    submission.request_id)] =
                    kNodeRequestBaseRoute + ipc_request.node_id;
            }
            const auto key = request_key(
                submission.packet.header.session_id, submission.request_id);
            const auto traffic_class =
                remotebsp::toolbusd::classify_traffic(
                    submission.packet);
            const auto policy =
                traffic_class ==
                        remotebsp::toolbusd::TrafficClass::Safety
                    ? remotebsp::toolbusd::AdmissionPolicy::Guaranteed
                    : remotebsp::toolbusd::AdmissionPolicy::Enforce;
            if (!send_packet(
                    submission.packet,
                    kNodeRequestBaseRoute + ipc_request.node_id,
                    policy)) {
                {
                    std::lock_guard<std::mutex> state_lock(state_mutex_);
                    requests_.cancel(
                        submission.packet.header.session_id,
                        submission.request_id);
                    request_routes_.erase(key);
                }
                throw std::runtime_error(
                    "CAN 带宽准入拒绝：当前业务类别预算不足");
            }

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
                request_routes_.erase(key);
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

    std::unique_ptr<remotebsp::transport::LinkTransport> transport_;
    remotebsp::protocol::Fragmenter fragmenter_;
    remotebsp::protocol::Reassembler reassembler_;
    remotebsp::toolbusd::TrafficController traffic_;
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
    std::unordered_map<std::uint64_t, std::uint32_t> request_routes_;
    std::unordered_map<
        std::uint32_t, std::deque<remotebsp::protocol::Packet>>
        event_queues_;
    std::unordered_map<std::uint64_t, UartStreamBuffer>
        uart_stream_buffers_;
    std::atomic<std::uint16_t> next_transfer_id_{1};
    std::uint32_t next_control_request_id_{0x80000000U};
    std::uint32_t next_node_id_{1};
    const std::uint32_t session_id_{make_session_id()};
};

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法: toolbusd <链路端点> <classical|fd|usb|usb-mock> "
                     "[Unix套接字路径] "
                     "[--arbitration-bitrate bit/s] "
                     "[--data-bitrate bit/s] "
                     "[--max-utilization-permille 1..1000] "
                     "[--burst-window-ms 毫秒]\n";
        return 2;
    }
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        const std::string mode_text = argv[2];
        const bool mock_usb = mode_text == "usb-mock";
        const bool usb = mode_text == "usb";
        const auto mode = (mock_usb || usb)
                              ? remotebsp::transport::CanMode::Classical
                              : parse_mode(mode_text);
        std::string socket_path = "/tmp/toolbusd.sock";
        remotebsp::toolbusd::TrafficConfig traffic_config;
        traffic_config.mode = (mock_usb || usb)
                                  ? remotebsp::toolbusd::TrafficBusMode::Usb
                                  : mode == remotebsp::transport::CanMode::Classical
                                        ? remotebsp::toolbusd::TrafficBusMode::Classical
                                        : remotebsp::toolbusd::TrafficBusMode::CanFd;
        traffic_config.arbitration_bits_per_second =
            (mock_usb || usb) ? 12000000U
            : mode == remotebsp::transport::CanMode::Classical
                ? 1000000U
                : 500000U;
        traffic_config.data_bits_per_second =
            (mock_usb || usb) ? 12000000U
            : mode == remotebsp::transport::CanMode::Classical
                ? 1000000U
                : 2000000U;
        int index = 3;
        if (index < argc &&
            std::string(argv[index]).rfind("--", 0) != 0) {
            socket_path = argv[index++];
        }
        while (index < argc) {
            const std::string option = argv[index++];
            if (index >= argc) {
                throw std::invalid_argument(
                    "toolbusd 选项缺少参数");
            }
            const std::uint32_t value =
                parse_positive_u32(argv[index++], option.c_str());
            if (option == "--arbitration-bitrate") {
                traffic_config.arbitration_bits_per_second = value;
            } else if (option == "--data-bitrate") {
                traffic_config.data_bits_per_second = value;
            } else if (option == "--max-utilization-permille") {
                if (value > 1000U) {
                    throw std::invalid_argument(
                        "最大总线利用率必须位于 1～1000 千分比");
                }
                traffic_config.maximum_utilization_permille =
                    static_cast<std::uint16_t>(value);
            } else if (option == "--burst-window-ms") {
                if (value > 60000U) {
                    throw std::invalid_argument(
                        "突发窗口必须位于 1～60000 ms");
                }
                traffic_config.burst_window =
                    std::chrono::milliseconds(value);
            } else {
                throw std::invalid_argument("未知 toolbusd 选项");
            }
        }
        if (!mock_usb && !usb &&
            mode == remotebsp::transport::CanMode::Classical) {
            traffic_config.data_bits_per_second =
                traffic_config.arbitration_bits_per_second;
        }
        std::unique_ptr<remotebsp::transport::LinkTransport> transport;
        if (mock_usb) {
            transport =
                std::make_unique<remotebsp::transport::MockUsbTransport>(
                    argv[1], remotebsp::transport::MockUsbRole::Host);
        } else if (usb) {
            transport =
                std::make_unique<remotebsp::transport::LibusbTransport>(
                    remotebsp::transport::parse_usb_device_selector(
                        argv[1]));
        } else {
            transport =
                std::make_unique<remotebsp::transport::SocketCanTransport>(
                    argv[1], mode);
        }
        ToolbusDaemon daemon(std::move(transport), std::move(socket_path),
                             traffic_config);
        daemon.run();
    } catch (const std::exception& error) {
        std::cerr << "toolbusd 启动失败: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
