#include "remotebsp/mock_mcu/board_manifest.hpp"
#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/visual_state.hpp"
#include "remotebsp/transport/socketcan_transport.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kBroadcastRequestCanId = 0x700;
constexpr std::uint32_t kNodeRequestBaseCanId = 0x600;
constexpr std::uint32_t kNodeResponseBaseCanId = 0x580;
constexpr std::uint32_t kNodeHeartbeatBaseCanId = 0x500;
constexpr std::uint32_t kProvisionalResponseBaseCanId = 0x480;
constexpr std::uint32_t kNodeId = 0;
constexpr auto kHeartbeatInterval = std::chrono::milliseconds(500);
constexpr auto kUartStreamInterval = std::chrono::milliseconds(100);
constexpr auto kVisualStateInterval = std::chrono::milliseconds(100);

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

void send_reply(remotebsp::transport::CanTransport& transport,
                std::uint32_t can_id,
                const remotebsp::mock_mcu::NodeReply& reply) {
    const bool bit_rate_switch =
        transport.mode() ==
        remotebsp::transport::CanMode::FlexibleDataRate;
    for (const auto& frame : reply.frames) {
        transport.send({can_id, false, frame, bit_rate_switch});
    }
}

remotebsp::mock_mcu::NodeReply make_uart_rx_event(
    std::size_t mtu, std::uint16_t transfer_id,
    std::uint32_t resource_id) {
    remotebsp::protocol::Packet event;
    event.header.message_type =
        remotebsp::protocol::MessageType::Event;
    event.header.command = static_cast<std::uint16_t>(
        remotebsp::protocol::Command::UartRxEvent);
    event.header.object_id = resource_id;
    const std::string text = "mock-uart7\n";
    event.payload.assign(text.begin(), text.end());
    remotebsp::mock_mcu::NodeReply reply;
    reply.transfer_id = transfer_id;
    reply.frames = remotebsp::protocol::Fragmenter(mtu).split(
        remotebsp::protocol::encode(event), transfer_id);
    return reply;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "用法: mock_mcu <SocketCAN接口> <classical|fd> "
                     "[--instance 1..127] [--uart-stream] "
                     "[--board JSON] [--fault-scenario JSON] "
                     "[--visual-state JSON]\n";
        return 2;
    }

    try {
        const auto mode = parse_mode(argv[2]);
        std::uint32_t instance = 1;
        bool uart_stream = false;
        std::string board_path = REMOTEBSP_DEFAULT_MOCK_BOARD_MANIFEST;
        std::string fault_scenario_path;
        std::string visual_state_path;
        for (int index = 3; index < argc;) {
            const std::string option = argv[index];
            if (option == "--uart-stream") {
                uart_stream = true;
                ++index;
                continue;
            }
            if ((option == "--board" || option == "--fault-scenario" ||
                 option == "--visual-state") &&
                index + 1 < argc) {
                if (option == "--board") {
                    board_path = argv[index + 1];
                } else if (option == "--fault-scenario") {
                    fault_scenario_path = argv[index + 1];
                } else {
                    visual_state_path = argv[index + 1];
                }
                index += 2;
                continue;
            }
            if (option != "--instance" || index + 1 >= argc) {
                throw std::invalid_argument("未知 Mock MCU 参数");
            }
            std::size_t consumed = 0;
            instance = static_cast<std::uint32_t>(
                std::stoul(argv[index + 1], &consumed, 0));
            if (consumed != std::string(argv[index + 1]).size() ||
                instance == 0 || instance > 127) {
                throw std::invalid_argument(
                    "Mock MCU 实例号必须位于 1～127");
            }
            index += 2;
        }
        auto manifest =
            remotebsp::mock_mcu::load_board_manifest(board_path);
        remotebsp::mock_mcu::FaultScenario fault_scenario;
        if (!fault_scenario_path.empty()) {
            fault_scenario =
                remotebsp::mock_mcu::load_fault_scenario(
                    fault_scenario_path);
        }
        remotebsp::mock_mcu::DigitalTwin twin(
            std::move(manifest), std::move(fault_scenario));
        std::uint32_t uart_stream_resource_id = 0;
        if (uart_stream) {
            const auto& resources = twin.manifest().resources;
            const auto found = std::find_if(
                resources.begin(), resources.end(), [](const auto& resource) {
                    return resource.type ==
                               remotebsp::protocol::ResourceType::Uart &&
                           resource.instance == 7;
                });
            if (found == resources.end()) {
                throw std::invalid_argument(
                    "--uart-stream 要求板卡描述包含 UART 7");
            }
            uart_stream_resource_id = found->resource_id;
        }
        remotebsp::transport::SocketCanTransport transport(argv[1], mode);
        remotebsp::mock_mcu::MockNode node(
            remotebsp::mock_mcu::make_remote_core(twin, instance),
            transport.mtu(), kNodeId);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        const auto started_at = std::chrono::steady_clock::now();
        auto next_heartbeat =
            started_at + kHeartbeatInterval;
        auto next_uart_stream =
            started_at + kUartStreamInterval;
        auto next_visual_state = started_at;
        std::cout << "Mock MCU 已连接 " << argv[1]
                  << "，板卡描述=" << twin.manifest().name
                  << "，按 Ctrl+C 退出\n";

        while (stop_requested == 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - started_at);
            twin.advance_to(static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, elapsed_ms.count())));
            if (!visual_state_path.empty() && now >= next_visual_state) {
                remotebsp::mock_mcu::write_visual_state(
                    twin,
                    static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed_ms.count())),
                    visual_state_path);
                next_visual_state = now + kVisualStateInterval;
            }
            if (now >= next_heartbeat) {
                if (twin.online()) {
                    const auto heartbeat_can_id =
                        node.node_id() == 0
                            ? kProvisionalResponseBaseCanId + instance
                            : kNodeHeartbeatBaseCanId + node.node_id();
                    send_reply(transport, heartbeat_can_id,
                               node.make_heartbeat());
                }
                next_heartbeat = now + kHeartbeatInterval;
            }
            if (uart_stream && node.node_id() != 0 &&
                now >= next_uart_stream) {
                if (twin.online()) {
                    const std::string stream_text = "mock-stream\n";
                    const std::vector<std::uint8_t> stream_data(
                        stream_text.begin(), stream_text.end());
                    if (twin.uart()) {
                        for (std::uint8_t port : {0U, 1U, 2U, 3U}) {
                            try {
                                twin.uart()->inject_rx(port, stream_data);
                            } catch (
                                const remotebsp::mock_mcu::UartException&) {
                            }
                        }
                    }
                    for (const auto& event :
                         node.poll_uart_events()) {
                        send_reply(
                            transport,
                            kNodeHeartbeatBaseCanId + node.node_id(),
                            event);
                    }
                    send_reply(
                        transport,
                        kNodeHeartbeatBaseCanId + node.node_id(),
                        make_uart_rx_event(
                            transport.mtu(),
                            node.allocate_transfer_id(),
                            uart_stream_resource_id));
                }
                next_uart_stream = now + kUartStreamInterval;
            }

            auto next_deadline =
                uart_stream && node.node_id() != 0
                    ? std::min(next_heartbeat, next_uart_stream)
                    : next_heartbeat;
            const auto next_fault = twin.next_event_ms();
            if (next_fault.has_value()) {
                next_deadline = std::min(
                    next_deadline,
                    started_at + std::chrono::milliseconds(*next_fault));
            }
            if (!visual_state_path.empty()) {
                next_deadline = std::min(next_deadline, next_visual_state);
            }
            const auto timeout =
                std::max(std::chrono::milliseconds(0),
                         std::chrono::duration_cast<
                             std::chrono::milliseconds>(
                             next_deadline -
                             std::chrono::steady_clock::now()));
            const auto message = transport.receive(timeout);
            if (!message.has_value()) {
                continue;
            }
            if (!twin.online()) {
                continue;
            }
            const bool broadcast =
                message->identifier == kBroadcastRequestCanId;
            const bool addressed =
                node.node_id() != 0 &&
                message->identifier ==
                    kNodeRequestBaseCanId + node.node_id();
            if ((!broadcast && !addressed) ||
                message->extended_identifier) {
                continue;
            }

            try {
                const auto reply = node.handle_frame(
                    message->identifier, message->data);
                if (reply.has_value()) {
                    const auto response_can_id =
                        node.node_id() == 0
                            ? kProvisionalResponseBaseCanId + instance
                            : kNodeResponseBaseCanId + node.node_id();
                    send_reply(transport, response_can_id, *reply);
                }
            } catch (const std::exception& error) {
                std::cerr << "忽略无效请求: " << error.what() << '\n';
            }
        }
        std::cout << "Mock MCU 已退出\n";
    } catch (const std::exception& error) {
        std::cerr << "Mock MCU 启动失败: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
