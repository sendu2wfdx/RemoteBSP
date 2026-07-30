#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"
#include "remotebsp/transport/socketcan_transport.hpp"

#include <algorithm>
#include <atomic>
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

remotebsp::mock_mcu::RemoteCore make_core(std::uint32_t instance) {
    remotebsp::mock_mcu::NodeInfo info;
    info.uuid = {0x52, 0x42, 0x53, 0x50, 0x2D, 0x4D, 0x4F, 0x43,
                 0x4B, 0x2D, 0x4E, 0x4F, 0x44, 0x45, 0x30, 0x31};
    info.uuid[15] = static_cast<std::uint8_t>(instance);
    info.firmware_major = 0;
    info.firmware_minor = 1;
    info.firmware_patch = 0;
    info.board_type = instance;

    using remotebsp::mock_mcu::Capability;
    using remotebsp::mock_mcu::capability_mask;
    const std::uint64_t capabilities =
        capability_mask(Capability::Gpio) |
        capability_mask(Capability::Uart) |
        capability_mask(Capability::Spi) |
        capability_mask(Capability::I2c) |
        capability_mask(Capability::Adc) |
        capability_mask(Capability::Pwm) |
        capability_mask(Capability::Timer) |
        capability_mask(Capability::Storage) |
        capability_mask(Capability::Bootloader);

    using remotebsp::protocol::ResourceDescriptor;
    using remotebsp::protocol::ResourceType;
    std::vector<ResourceDescriptor> resources;
    for (std::uint16_t pin = 0; pin < 16; ++pin) {
        resources.push_back(
            {0x01000000U + pin, ResourceType::Gpio, pin,
             remotebsp::protocol::kResourceFlagNative, 0, 0});
    }
    for (std::uint16_t port = 0; port < 8; ++port) {
        const auto source =
            port < 4 ? remotebsp::protocol::kResourceFlagNative
                     : remotebsp::protocol::kResourceFlagExpanded;
        resources.push_back(
            {0x02000000U + port, ResourceType::Uart, port,
             source, 4096, 4096});
    }
    return remotebsp::mock_mcu::RemoteCore(
        info, capabilities,
        std::make_shared<remotebsp::mock_mcu::MockGpioBsp>(),
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>(),
        std::move(resources));
}

void send_reply(remotebsp::transport::CanTransport& transport,
                std::uint32_t can_id,
                const remotebsp::mock_mcu::NodeReply& reply) {
    for (const auto& frame : reply.frames) {
        transport.send({can_id, false, frame});
    }
}

remotebsp::mock_mcu::NodeReply make_uart_rx_event(
    std::size_t mtu, std::uint16_t transfer_id) {
    remotebsp::protocol::Packet event;
    event.header.message_type =
        remotebsp::protocol::MessageType::Event;
    event.header.command = static_cast<std::uint16_t>(
        remotebsp::protocol::Command::UartRxEvent);
    event.header.object_id = 0x02000007U;
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
                     "[--instance 1..127] [--uart-stream]\n";
        return 2;
    }

    try {
        const auto mode = parse_mode(argv[2]);
        std::uint32_t instance = 1;
        bool uart_stream = false;
        for (int index = 3; index < argc;) {
            const std::string option = argv[index];
            if (option == "--uart-stream") {
                uart_stream = true;
                ++index;
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
        remotebsp::transport::SocketCanTransport transport(argv[1], mode);
        remotebsp::mock_mcu::MockNode node(make_core(instance), transport.mtu(),
                                           kNodeId);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        auto next_heartbeat =
            std::chrono::steady_clock::now() + kHeartbeatInterval;
        auto next_uart_stream =
            std::chrono::steady_clock::now() + kUartStreamInterval;
        std::cout << "Mock MCU 已连接 " << argv[1] << "，按 Ctrl+C 退出\n";

        while (stop_requested == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_heartbeat) {
                const auto heartbeat_can_id =
                    node.node_id() == 0
                        ? kProvisionalResponseBaseCanId + instance
                        : kNodeHeartbeatBaseCanId + node.node_id();
                send_reply(transport, heartbeat_can_id,
                           node.make_heartbeat());
                next_heartbeat = now + kHeartbeatInterval;
            }
            if (uart_stream && node.node_id() != 0 &&
                now >= next_uart_stream) {
                send_reply(
                    transport,
                    kNodeHeartbeatBaseCanId + node.node_id(),
                    make_uart_rx_event(transport.mtu(),
                                       node.allocate_transfer_id()));
                next_uart_stream = now + kUartStreamInterval;
            }

            const auto next_deadline =
                uart_stream && node.node_id() != 0
                    ? std::min(next_heartbeat, next_uart_stream)
                    : next_heartbeat;
            const auto timeout =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    next_deadline - std::chrono::steady_clock::now());
            const auto message = transport.receive(timeout);
            if (!message.has_value()) {
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
