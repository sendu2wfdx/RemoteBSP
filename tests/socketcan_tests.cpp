#include "remotebsp/transport/socketcan_transport.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace remotebsp::transport;

namespace {

bool test_mode(const std::string& interface_name, CanMode mode,
               std::size_t payload_size, std::uint32_t identifier) {
    SocketCanTransport receiver(interface_name, mode);
    SocketCanTransport sender(interface_name, mode);

    CanMessage sent;
    sent.identifier = identifier;
    sent.data.resize(payload_size);
    for (std::size_t i = 0; i < sent.data.size(); ++i) {
        sent.data[i] = static_cast<std::uint8_t>((i * 19U) & 0xFFU);
    }

    sender.send(sent);
    const auto received = receiver.receive(std::chrono::milliseconds(500));
    return received.has_value() &&
           received->identifier == sent.identifier &&
           received->extended_identifier == sent.extended_identifier &&
           received->data == sent.data;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "用法: socketcan_tests <vcan接口>\n";
        return 2;
    }

    try {
        if (!test_mode(argv[1], CanMode::Classical, 8, 0x321)) {
            std::cerr << "Classical CAN 回环测试失败\n";
            return 1;
        }
        if (!test_mode(argv[1], CanMode::FlexibleDataRate, 64, 0x322)) {
            std::cerr << "CAN-FD 回环测试失败\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "SocketCAN 测试异常: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Classical CAN 和 CAN-FD 回环测试通过\n";
    return 0;
}
