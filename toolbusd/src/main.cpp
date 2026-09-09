#include "remotebsp/protocol/fragmentation.hpp"
#include "remotebsp/toolbusd/clock_sync_manager.hpp"
#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/motion_group_service.hpp"
#include "remotebsp/toolbusd/motion_group_dispatch_gate.hpp"
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
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

std::uint64_t steady_time_ns(
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());
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
                  remotebsp::toolbusd::TrafficConfig traffic_config,
                  remotebsp::toolbusd::ClockSyncManagerConfig
                      clock_sync_config,
                  remotebsp::toolbusd::MotionGroupServiceConfig
                      motion_group_config)
        : transport_(std::move(transport)),
          fragmenter_(transport_->mtu()),
          reassembler_(transport_->mtu(), std::chrono::milliseconds(500)),
          traffic_(std::move(traffic_config)),
          clock_sync_(std::move(clock_sync_config)),
          motion_group_(std::move(motion_group_config)),
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
    bool send_motion_dispatches(
        const std::vector<remotebsp::toolbusd::MotionGroupDispatch>&
            dispatches) {
        bool non_abort_failed = false;
        for (const auto& dispatch : dispatches) {
            bool sent = false;
            try {
                sent = send_packet(
                    dispatch.submission.packet,
                    kNodeRequestBaseRoute + dispatch.node_id,
                    dispatch.phase == remotebsp::toolbusd::
                                          MotionGroupActionPhase::Abort
                        ? remotebsp::toolbusd::AdmissionPolicy::Guaranteed
                        : remotebsp::toolbusd::AdmissionPolicy::Enforce);
            } catch (const std::exception& error) {
                std::cerr << "运动组动作发送失败: " << error.what()
                          << '\n';
            }
            if (!sent &&
                dispatch.phase != remotebsp::toolbusd::
                                      MotionGroupActionPhase::Abort) {
                non_abort_failed = true;
            }
        }
        if (!non_abort_failed) {
            return true;
        }

        std::vector<remotebsp::toolbusd::MotionGroupDispatch> aborts;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto state = motion_group_.state();
            if (state == remotebsp::toolbusd::MotionGroupState::Preparing ||
                state == remotebsp::toolbusd::MotionGroupState::Ready ||
                state == remotebsp::toolbusd::MotionGroupState::Committing) {
                aborts = motion_group_.cancel(requests_).dispatches;
                state_changed_.notify_all();
            }
        }
        // ABORT 使用安全业务保证通道。首次发送异常时请求仍留在
        // RequestManager 中，由主循环有界重试，不能阻塞接收线程。
        for (const auto& abort : aborts) {
            try {
                static_cast<void>(send_packet(
                    abort.submission.packet,
                    kNodeRequestBaseRoute + abort.node_id,
                    remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
            } catch (const std::exception& error) {
                std::cerr << "运动组 ABORT 首次发送失败，将等待重试: "
                          << error.what() << '\n';
            }
        }
        return false;
    }

    static bool motion_group_identity_matches(
        const remotebsp::toolbusd::IpcRequest& request,
        const remotebsp::toolbusd::MotionGroupServiceSnapshot& snapshot) {
        return request.transaction_id == snapshot.transaction_id &&
               request.group_id == snapshot.group_id &&
               request.plan_generation == snapshot.plan_generation;
    }

    bool send_packet(
        const remotebsp::protocol::Packet& packet, std::uint32_t route,
        remotebsp::toolbusd::AdmissionPolicy policy =
            remotebsp::toolbusd::AdmissionPolicy::Enforce,
        const std::function<void()>& on_first_frame = {}) {
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
        bool first_frame = true;
        for (const auto& frame : frames) {
            if (first_frame && on_first_frame) {
                on_first_frame();
            }
            first_frame = false;
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
                process_clock_sync_schedule();
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
        /* 完整响应形成后立即取时，避免解码和锁等待混入往返时延。 */
        const auto response_complete_at =
            remotebsp::toolbusd::ClockSyncManager::Clock::now();
        const auto packet = remotebsp::protocol::decode(*result.packet);
        const auto command =
            static_cast<remotebsp::protocol::Command>(packet.header.command);
        // 错误 command 也可能携带运动组 request_id，并会触发全组
        // fail_active。因此除发现/时钟同步外的所有普通响应均经过运动门，
        // 不能只按响应 command 判断。
        const bool needs_motion_dispatch_gate =
            packet.header.message_type ==
                remotebsp::protocol::MessageType::Response &&
            command != remotebsp::protocol::Command::DiscoveryResponse &&
            command != remotebsp::protocol::Command::TimeSync;
        std::optional<remotebsp::toolbusd::MotionGroupDispatchGate::Guard>
            motion_guard;
        if (needs_motion_dispatch_gate) {
            motion_guard.emplace(motion_dispatch_gate_.lock());
        }

        std::optional<remotebsp::protocol::Packet> assignment;
        std::vector<remotebsp::toolbusd::MotionGroupDispatch>
            motion_dispatches;
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
                    const auto* previous = nodes_.find(identity.uuid);
                    if (previous != nullptr && previous->node_id != 0U) {
                        static_cast<void>(clock_sync_.cancel_node(
                            requests_, previous->node_id));
                    }
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
                const auto response_node_id =
                    message.route - kNodeResponseBaseRoute;
                if (packet.header.command == static_cast<std::uint16_t>(
                        remotebsp::protocol::Command::TimeSync)) {
                    static_cast<void>(clock_sync_.accept_response(
                        requests_, nodes_, packet, response_node_id,
                        response_complete_at));
                    return;
                }
                const auto group = motion_group_.accept_response(
                    requests_, nodes_, response_node_id, packet,
                    steady_time_ns(response_complete_at),
                    response_complete_at);
                if (group.status != remotebsp::toolbusd::
                                        MotionGroupServiceStatus::Unrelated) {
                    motion_dispatches = group.dispatches;
                    state_changed_.notify_all();
                } else {
                    const auto target = request_routes_.find(key);
                    if (target == request_routes_.end() ||
                        target->second !=
                            kNodeRequestBaseRoute + response_node_id) {
                        return;
                    }
                    const auto response = requests_.accept_response(packet);
                    if (response.status ==
                            remotebsp::toolbusd::ResponseStatus::Matched &&
                        response.response.has_value()) {
                        responses_[key] = *response.response;
                        state_changed_.notify_all();
                    }
                }
            }
        }
        if (assignment.has_value()) {
            static_cast<void>(send_packet(
                *assignment, kBroadcastRequestRoute,
                remotebsp::toolbusd::AdmissionPolicy::Guaranteed));
        }
        if (!motion_dispatches.empty()) {
            static_cast<void>(send_motion_dispatches(motion_dispatches));
        }
    }

    void process_request_timers() {
        auto motion_guard = motion_dispatch_gate_.lock();
        struct Retry {
            remotebsp::protocol::Packet packet;
            std::uint32_t route{};
        };
        std::vector<Retry> retries;
        std::vector<remotebsp::toolbusd::MotionGroupDispatch>
            motion_dispatches;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto now = std::chrono::steady_clock::now();
            const auto request_events = requests_.poll(now);
            const auto offline = nodes_.expire(now);
            for (const auto& uuid : offline) {
                const auto* node = nodes_.find(uuid);
                if (node != nullptr && node->node_id != 0U) {
                    static_cast<void>(clock_sync_.cancel_node(
                        requests_, node->node_id));
                }
            }
            const auto group = motion_group_.poll(
                requests_, nodes_, request_events, steady_time_ns(now), now);
            motion_dispatches = group.dispatches;
            for (const auto& event : group.unhandled_events) {
                if (event.type ==
                        remotebsp::toolbusd::RequestEventType::Retry &&
                    event.packet.has_value()) {
                    const auto key =
                        request_key(event.session_id, event.request_id);
                    const auto target = request_routes_.find(key);
                    if (event.packet->header.command ==
                        static_cast<std::uint16_t>(
                            remotebsp::protocol::Command::TimeSync)) {
                        retries.push_back(
                            {*event.packet,
                             kNodeRequestBaseRoute +
                                 event.packet->header.object_id});
                    } else if (target != request_routes_.end()) {
                        retries.push_back({*event.packet, target->second});
                    }
                } else if (event.type ==
                           remotebsp::toolbusd::RequestEventType::TimedOut) {
                    const bool clock_sync_timeout =
                        clock_sync_.handle_request_event(requests_, event);
                    if (!clock_sync_timeout) {
                        timed_out_.insert(request_key(
                            event.session_id, event.request_id));
                        request_routes_.erase(request_key(
                            event.session_id, event.request_id));
                        state_changed_.notify_all();
                    }
                }
            }
            if (!offline.empty() ||
                group.status != remotebsp::toolbusd::
                                    MotionGroupServiceStatus::Accepted) {
                state_changed_.notify_all();
            }
        }
        if (!motion_dispatches.empty()) {
            static_cast<void>(send_motion_dispatches(motion_dispatches));
        }
        motion_guard.unlock();
        for (const auto& retry : retries) {
            const bool is_clock_sync =
                retry.packet.header.command ==
                static_cast<std::uint16_t>(
                    remotebsp::protocol::Command::TimeSync);
            static_cast<void>(send_packet(
                retry.packet, retry.route,
                is_clock_sync
                    ? remotebsp::toolbusd::AdmissionPolicy::Enforce
                    : remotebsp::toolbusd::AdmissionPolicy::Guaranteed,
                is_clock_sync
                    ? std::function<void()>([this, &retry] {
                          std::lock_guard<std::mutex> lock(state_mutex_);
                          static_cast<void>(clock_sync_.mark_sent(
                              retry.packet.header.session_id,
                              retry.packet.header.request_id));
                      })
                    : std::function<void()>{}));
        }
    }

    void process_clock_sync_schedule() {
        std::vector<remotebsp::toolbusd::ClockSyncDispatch> dispatches;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            dispatches = clock_sync_.poll_schedule(
                requests_, nodes_, session_id_);
        }
        for (const auto& dispatch : dispatches) {
            const auto& packet = dispatch.submission.packet;
            const bool sent = send_packet(
                packet, kNodeRequestBaseRoute + dispatch.node_id,
                remotebsp::toolbusd::AdmissionPolicy::Enforce,
                [this, &packet] {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    static_cast<void>(clock_sync_.mark_sent(
                        packet.header.session_id,
                        packet.header.request_id));
                });
            if (!sent) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                static_cast<void>(clock_sync_.cancel(
                    requests_, packet.header.session_id,
                    packet.header.request_id));
            }
        }
    }

    std::vector<std::uint8_t> request_snapshot_resource(
        std::uint32_t node_id, remotebsp::protocol::Command command,
        std::vector<std::uint8_t> payload,
        std::chrono::steady_clock::time_point deadline) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Runtime 快照达到总时间上限");
        }
        remotebsp::protocol::Packet request;
        request.header.message_type =
            remotebsp::protocol::MessageType::Request;
        request.header.command = static_cast<std::uint16_t>(command);
        request.header.object_id = 0U;
        request.header.session_id = session_id_;
        request.payload = std::move(payload);

        remotebsp::toolbusd::Submission submission;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto* node = nodes_.find_by_node_id(node_id);
            if (node == nullptr || !node->online || !node->assigned) {
                throw std::runtime_error(
                    "Runtime 快照期间目标节点不可用");
            }
            submission = requests_.submit(std::move(request));
            request_routes_[request_key(session_id_, submission.request_id)] =
                kNodeRequestBaseRoute + node_id;
        }
        const auto key = request_key(session_id_, submission.request_id);
        if (!send_packet(submission.packet,
                         kNodeRequestBaseRoute + node_id)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            requests_.cancel(session_id_, submission.request_id);
            request_routes_.erase(key);
            throw std::runtime_error(
                "Runtime 快照查询被带宽准入拒绝");
        }

        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool completed = state_changed_.wait_until(lock, deadline, [&] {
            return !running_ || responses_.find(key) != responses_.end() ||
                   timed_out_.find(key) != timed_out_.end();
        });
        const auto response = responses_.find(key);
        if (completed && response != responses_.end()) {
            auto packet = std::move(response->second);
            responses_.erase(response);
            request_routes_.erase(key);
            lock.unlock();
            if (packet.payload.empty() || packet.payload.front() != 0U ||
                (packet.header.flags &
                 remotebsp::protocol::kErrorResponseFlag) != 0U) {
                throw std::runtime_error(
                    "Runtime 快照远端只读查询失败");
            }
            return {packet.payload.begin() + 1U, packet.payload.end()};
        }
        requests_.cancel(session_id_, submission.request_id);
        request_routes_.erase(key);
        timed_out_.erase(key);
        throw std::runtime_error(
            completed ? "Runtime 快照远端请求超时"
                      : "Runtime 快照达到总时间上限");
    }

    remotebsp::toolbusd::IpcRuntimeSnapshot build_runtime_snapshot(
        std::uint16_t maximum_resources,
        std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        remotebsp::toolbusd::IpcRuntimeSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (const auto& node : nodes_.records()) {
                snapshot.nodes.push_back(
                    {node.identity.uuid, node.node_id, node.online,
                     node.assigned, node.identity.firmware_major,
                     node.identity.firmware_minor,
                     node.identity.firmware_patch,
                     node.identity.board_type,
                     node.identity.protocol_version});
            }
        }
        for (const auto& node : snapshot.nodes) {
            if (!node.online || !node.ready) {
                continue;
            }
            std::vector<std::uint8_t> descriptor_body;
            try {
                descriptor_body = request_snapshot_resource(
                    node.node_id,
                    remotebsp::protocol::Command::ResourceEnum, {},
                    deadline);
            } catch (const std::runtime_error&) {
                snapshot.node_issues.push_back(
                    {node.node_id,
                     remotebsp::toolbusd::IpcRuntimeNodeError::
                         ResourceInventoryUnavailable});
                continue;
            }
            const auto descriptors =
                remotebsp::protocol::decode_resource_list(descriptor_body);
            if (descriptors.size() >
                static_cast<std::size_t>(maximum_resources) -
                    snapshot.resources.size()) {
                throw std::runtime_error(
                    "Runtime 快照资源数量超过请求上限");
            }
            for (const auto& descriptor : descriptors) {
                std::vector<std::uint8_t> status_body;
                try {
                    status_body = request_snapshot_resource(
                        node.node_id,
                        remotebsp::protocol::Command::ResourceStatus,
                        remotebsp::protocol::encode_resource_id(
                            descriptor.resource_id),
                        deadline);
                } catch (const std::runtime_error&) {
                    remotebsp::protocol::ResourceStatusPayload unavailable;
                    unavailable.resource_id = descriptor.resource_id;
                    snapshot.resources.push_back(
                        {node.node_id, false, descriptor, unavailable});
                    continue;
                }
                const auto status =
                    remotebsp::protocol::decode_resource_status(status_body);
                if (status.resource_id != descriptor.resource_id) {
                    throw std::runtime_error(
                        "Runtime 快照资源状态 ID 不匹配");
                }
                snapshot.resources.push_back(
                    {node.node_id, true, descriptor, status});
            }
        }

        std::vector<remotebsp::toolbusd::IpcNodeInfo> final_nodes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto host_now_count =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (host_now_count < 0) {
                throw std::runtime_error(
                    "主机单调时钟不能转换为 Runtime 快照时间");
            }
            const auto host_now_ns =
                static_cast<std::uint64_t>(host_now_count);
            for (const auto& node : nodes_.records()) {
                final_nodes.push_back(
                    {node.identity.uuid, node.node_id, node.online,
                     node.assigned, node.identity.firmware_major,
                     node.identity.firmware_minor,
                     node.identity.firmware_patch,
                     node.identity.board_type,
                     node.identity.protocol_version});
                remotebsp::toolbusd::IpcRuntimeClockQuality clock;
                clock.node_id = node.node_id;
                const auto quality =
                    nodes_.clock_quality(node.node_id, host_now_ns);
                if (quality.has_value()) {
                    if (quality->estimate.sample_count >
                            std::numeric_limits<std::uint16_t>::max() ||
                        quality->estimate.selected_sample_count >
                            std::numeric_limits<std::uint16_t>::max()) {
                        throw std::runtime_error(
                            "Runtime 快照时钟样本数量超过 IPC 上限");
                    }
                    clock.registered = true;
                    clock.estimate_valid = quality->estimate.valid;
                    clock.state = quality->estimate.state;
                    clock.boot_epoch = quality->boot_epoch;
                    clock.model_generation = quality->model_generation;
                    clock.sample_count = static_cast<std::uint16_t>(
                        quality->estimate.sample_count);
                    clock.selected_sample_count =
                        static_cast<std::uint16_t>(
                            quality->estimate.selected_sample_count);
                    if (quality->estimate.valid) {
                        const auto rate_ppb = std::round(
                            quality->estimate.rate_deviation_ppm * 1000.0L);
                        if (!std::isfinite(rate_ppb) ||
                            rate_ppb < static_cast<long double>(
                                std::numeric_limits<std::int32_t>::min()) ||
                            rate_ppb > static_cast<long double>(
                                std::numeric_limits<std::int32_t>::max())) {
                            throw std::runtime_error(
                                "Runtime 快照时钟漂移超过 IPC 上限");
                        }
                        clock.rate_deviation_ppb =
                            static_cast<std::int32_t>(rate_ppb);
                        clock.drift_uncertainty_ppm =
                            quality->estimate.drift_uncertainty_ppm;
                        clock.minimum_network_rtt_ns =
                            quality->estimate.minimum_network_rtt_ns;
                        clock.error_bound_ns =
                            quality->estimate.error_bound_ns;
                        clock.sample_age_ns =
                            quality->estimate.sample_age_ns;
                        clock.last_sample_host_time_ns =
                            quality->estimate.last_sample_host_time_ns;
                    }
                }
                snapshot.clocks.push_back(clock);
            }
        }
        const auto same_node = [](const auto& left, const auto& right) {
            return left.uuid == right.uuid &&
                   left.node_id == right.node_id &&
                   left.online == right.online &&
                   left.ready == right.ready &&
                   left.firmware_major == right.firmware_major &&
                   left.firmware_minor == right.firmware_minor &&
                   left.firmware_patch == right.firmware_patch &&
                   left.board_type == right.board_type &&
                   left.protocol_version == right.protocol_version;
        };
        if (snapshot.nodes.size() != final_nodes.size() ||
            !std::equal(snapshot.nodes.begin(), snapshot.nodes.end(),
                        final_nodes.begin(), same_node)) {
            throw std::runtime_error(
                "Runtime 快照期间节点拓扑发生变化");
        }
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            snapshot.traffic = traffic_.snapshot();
        }
        snapshot.sequence = next_runtime_snapshot_sequence_++;
        if (snapshot.sequence == 0U) {
            snapshot.sequence = next_runtime_snapshot_sequence_++;
        }
        return snapshot;
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
                remotebsp::toolbusd::IpcRequestKind::RuntimeSnapshot) {
                const auto snapshot_deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ipc_request.timeout_ms);
                std::unique_lock<std::timed_mutex> snapshot_lock(
                    runtime_snapshot_mutex_, std::defer_lock);
                if (!snapshot_lock.try_lock_until(snapshot_deadline)) {
                    throw std::runtime_error(
                        "等待其他 Runtime 快照达到总时间上限");
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= snapshot_deadline) {
                    throw std::runtime_error(
                        "Runtime 快照达到总时间上限");
                }
                const auto remaining =
                    std::max(std::chrono::milliseconds(1),
                             std::chrono::duration_cast<
                                 std::chrono::milliseconds>(
                                 snapshot_deadline - now));
                const auto snapshot = build_runtime_snapshot(
                    static_cast<std::uint16_t>(
                        ipc_request.maximum_length),
                    remaining);
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_runtime_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupSubmit) {
                auto motion_guard = motion_dispatch_gate_.lock();
                std::vector<remotebsp::toolbusd::MotionGroupDispatch>
                    dispatches;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    const auto state = motion_group_.state();
                    if (state ==
                            remotebsp::toolbusd::MotionGroupState::Committed ||
                        state ==
                            remotebsp::toolbusd::MotionGroupState::Aborted) {
                        motion_group_.reset();
                    } else if (state !=
                               remotebsp::toolbusd::MotionGroupState::Idle) {
                        throw std::runtime_error(
                            "已有运动组事务正在执行；全局一次只允许一个事务");
                    }
                    dispatches = motion_group_.start(
                        requests_, ipc_request.motion_group_plan, nodes_,
                        session_id_, steady_time_ns()).dispatches;
                }
                static_cast<void>(send_motion_dispatches(dispatches));
                motion_guard.unlock();
                remotebsp::toolbusd::MotionGroupServiceSnapshot snapshot;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    snapshot = motion_group_.snapshot();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_motion_group_snapshot(
                        snapshot));
                return;
            }
            if (ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupStatus ||
                ipc_request.kind == remotebsp::toolbusd::
                                        IpcRequestKind::MotionGroupCancel) {
                std::vector<remotebsp::toolbusd::MotionGroupDispatch>
                    dispatches;
                remotebsp::toolbusd::MotionGroupServiceSnapshot snapshot;
                std::optional<
                    remotebsp::toolbusd::MotionGroupDispatchGate::Guard>
                    motion_guard;
                if (ipc_request.kind == remotebsp::toolbusd::
                                            IpcRequestKind::
                                                MotionGroupCancel) {
                    motion_guard.emplace(motion_dispatch_gate_.lock());
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    snapshot = motion_group_.snapshot();
                    if (!motion_group_identity_matches(
                            ipc_request, snapshot)) {
                        throw std::runtime_error(
                            "运动组事务身份与当前记录不匹配");
                    }
                    const auto state = motion_group_.state();
                    if (ipc_request.kind == remotebsp::toolbusd::
                                                IpcRequestKind::
                                                    MotionGroupCancel &&
                        (state == remotebsp::toolbusd::
                                      MotionGroupState::Preparing ||
                         state == remotebsp::toolbusd::
                                      MotionGroupState::Ready ||
                         state == remotebsp::toolbusd::
                                      MotionGroupState::Committing)) {
                        dispatches = motion_group_.cancel(requests_)
                                         .dispatches;
                        snapshot = motion_group_.snapshot();
                    }
                }
                if (!dispatches.empty()) {
                    static_cast<void>(send_motion_dispatches(dispatches));
                }
                if (motion_guard.has_value()) {
                    motion_guard->unlock();
                }
                remotebsp::toolbusd::write_ipc_response(
                    client, remotebsp::toolbusd::IpcStatus::Ok,
                    remotebsp::toolbusd::encode_ipc_motion_group_snapshot(
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
            const auto command = static_cast<remotebsp::protocol::Command>(
                request.header.command);
            if (command ==
                    remotebsp::protocol::Command::MotionGroupPrepare ||
                command ==
                    remotebsp::protocol::Command::MotionGroupCommit ||
                command ==
                    remotebsp::protocol::Command::MotionGroupAbort) {
                throw std::invalid_argument(
                    "跨板运动组命令必须通过事务 IPC 提交，不能直通节点");
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
            if (existing.st_uid != ::geteuid()) {
                throw std::runtime_error(
                    "本地套接字路径属于其他用户，拒绝删除");
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
                   sizeof(address)) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "绑定本地 IPC 套接字失败");
        }
        struct stat created {};
        const int created_stat = ::lstat(socket_path_.c_str(), &created);
        if (created_stat < 0 ||
            !S_ISSOCK(created.st_mode) ||
            created.st_uid != ::geteuid()) {
            const int saved_errno = created_stat < 0 ? errno : EINVAL;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "确认本地 IPC 套接字身份失败");
        }
        socket_device_ = created.st_dev;
        socket_inode_ = created.st_ino;
        socket_identity_known_ = true;
        // 不依赖服务进程的 umask。套接字在开始监听前固定为 0660，
        // 由部署时的所有者/组决定哪些本地进程能够发出硬件命令。
        if (::chmod(socket_path_.c_str(),
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "设置本地 IPC 套接字权限失败");
        }
        if (::listen(server_socket_, 8) < 0) {
            const int saved_errno = errno;
            close_server();
            throw std::system_error(saved_errno, std::generic_category(),
                                    "监听本地 IPC 套接字失败");
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
                S_ISSOCK(existing.st_mode) && socket_identity_known_ &&
                existing.st_dev == socket_device_ &&
                existing.st_ino == socket_inode_) {
                ::unlink(socket_path_.c_str());
            }
        }
        socket_identity_known_ = false;
        socket_device_ = 0;
        socket_inode_ = 0;
    }

    std::unique_ptr<remotebsp::transport::LinkTransport> transport_;
    remotebsp::protocol::Fragmenter fragmenter_;
    remotebsp::protocol::Reassembler reassembler_;
    remotebsp::toolbusd::TrafficController traffic_;
    remotebsp::toolbusd::RequestManager requests_;
    remotebsp::toolbusd::ClockSyncManager clock_sync_;
    remotebsp::toolbusd::MotionGroupService motion_group_;
    remotebsp::toolbusd::NodeRegistry nodes_;
    std::string socket_path_;
    int server_socket_{-1};
    dev_t socket_device_{};
    ino_t socket_inode_{};
    bool socket_identity_known_{};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex client_mutex_;
    std::condition_variable clients_finished_;
    std::size_t active_clients_{0};
    std::mutex send_mutex_;
    remotebsp::toolbusd::MotionGroupDispatchGate motion_dispatch_gate_;
    std::timed_mutex runtime_snapshot_mutex_;
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
    std::uint64_t next_runtime_snapshot_sequence_{1U};
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
                     "[--burst-window-ms 毫秒] "
                     "[--motion-max-clock-error-ns 纳秒]\n";
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
        remotebsp::toolbusd::ClockSyncManagerConfig clock_sync_config;
        remotebsp::toolbusd::MotionGroupServiceConfig motion_group_config;
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
            } else if (option == "--motion-max-clock-error-ns") {
                if (value > 10000000U) {
                    throw std::invalid_argument(
                        "跨板运动最大时钟误差必须位于 1～10000000 ns");
                }
                motion_group_config.coordinator.maximum_clock_error_ns =
                    value;
                // 协调器不能使用已被 ClockModel 标成 degraded 的模型。
                // 因此同一准入上限同时约束模型状态和事务冻结，避免把
                // 高于模型默认 250 us 的配置变成表面可配、实际无效。
                clock_sync_config.clock_model_config.maximum_error_bound_ns =
                    value;
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
                             traffic_config, clock_sync_config,
                             motion_group_config);
        daemon.run();
    } catch (const std::exception& error) {
        std::cerr << "toolbusd 启动失败: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
