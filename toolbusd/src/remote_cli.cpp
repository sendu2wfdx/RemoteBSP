#include "remotebsp/client.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint32_t parse_u32(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint8_t parse_hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("十六进制数据包含非法字符");
}

std::vector<std::uint8_t> parse_hex(const std::string& text) {
    if (text.empty() || (text.size() % 2U) != 0U) {
        throw std::invalid_argument(
            "十六进制数据必须非空，且每个字节使用两个字符");
    }
    std::vector<std::uint8_t> result;
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        result.push_back(static_cast<std::uint8_t>(
            (parse_hex_digit(text[index]) << 4U) |
            parse_hex_digit(text[index + 1U])));
    }
    return result;
}

const char* resource_type(remotebsp::protocol::ResourceType type) {
    using remotebsp::protocol::ResourceType;
    switch (type) {
        case ResourceType::Gpio: return "gpio";
        case ResourceType::Uart: return "uart";
        case ResourceType::Spi: return "spi";
        case ResourceType::I2c: return "i2c";
        case ResourceType::Adc: return "adc";
        case ResourceType::Pwm: return "pwm";
        case ResourceType::Timer: return "timer";
        case ResourceType::Storage: return "storage";
    }
    return "unknown";
}

const char* resource_source(std::uint16_t flags) {
    return (flags & remotebsp::protocol::kResourceFlagExpanded) != 0
               ? "expanded"
               : "native";
}

const char* health_name(remotebsp::protocol::ResourceHealth health) {
    using remotebsp::protocol::ResourceHealth;
    switch (health) {
        case ResourceHealth::Normal: return "normal";
        case ResourceHealth::Busy: return "busy";
        case ResourceHealth::Degraded: return "degraded";
        case ResourceHealth::Failed: return "failed";
        case ResourceHealth::Disabled: return "disabled";
    }
    return "unknown";
}

void print_resource(const remotebsp::protocol::ResourceDescriptor& resource,
                    bool include_type) {
    std::cout << "resource_id=0x" << std::hex << resource.resource_id
              << std::dec;
    if (include_type) {
        std::cout << " type=" << resource_type(resource.type);
    }
    std::cout << " instance=" << resource.instance
              << " source=" << resource_source(resource.flags)
              << " rx_capacity=" << resource.rx_capacity
              << " tx_capacity=" << resource.tx_capacity << '\n';
}

void print_hex(const std::vector<std::uint8_t>& data) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto value : data) {
        std::cout << digits[value >> 4U] << digits[value & 0x0FU];
    }
}

void print_usage() {
    std::cerr
        << "用法: remote-cli [--socket 路径] [--node 节点ID] <命令> [参数]\n"
        << "命令:\n"
        << "  ping <文本>\n"
        << "  node-list\n"
        << "  event-wait\n"
        << "  get-info | get-capability\n"
        << "  resource-list\n"
        << "  resource-describe <资源ID>\n"
        << "  resource-status <资源ID>\n"
        << "  resource-reset <资源ID>\n"
        << "  gpio-create <引脚> <input|output> [初始电平]\n"
        << "  gpio-read <对象ID>\n"
        << "  gpio-write <对象ID> <0|1>\n"
        << "  uart-create <端口> <波特率> [数据位] [none|odd|even] [停止位]\n"
        << "  uart-read <对象ID> <最大长度>\n"
        << "  uart-write <对象ID> <文本>\n"
        << "  uart-write-hex <对象ID> <十六进制字节串>\n";
}

int run(const std::vector<std::string>& arguments,
        const std::string& socket_path, std::uint32_t node_id) {
    if (arguments.empty()) {
        print_usage();
        return 2;
    }
    const auto& name = arguments[0];
    remotebsp::Client client(socket_path, node_id);

    if (name == "node-list" && arguments.size() == 1) {
        for (const auto& node : client.list_nodes()) {
            std::cout << "node_id=" << node.node_id
                      << " online=" << (node.online ? 1 : 0)
                      << " ready=" << (node.ready ? 1 : 0)
                      << " board_type=0x" << std::hex
                      << node.identity.board_type << std::dec
                      << " firmware=" << node.identity.firmware_major << '.'
                      << node.identity.firmware_minor << '.'
                      << node.identity.firmware_patch
                      << " protocol_version="
                      << static_cast<unsigned>(
                             node.identity.protocol_version)
                      << " uuid=";
            const std::vector<std::uint8_t> uuid(
                node.identity.uuid.begin(), node.identity.uuid.end());
            print_hex(uuid);
            std::cout << '\n';
        }
        return 0;
    }
    if (name == "event-wait" && arguments.size() == 1) {
        const auto event = client.next_event();
        if (!event.has_value()) {
            std::cout << "timeout\n";
            return 3;
        }
        std::cout << "command=0x" << std::hex
                  << event->header.command
                  << " resource_id=0x" << event->header.object_id
                  << std::dec << " data_hex=";
        print_hex(event->payload);
        std::cout << '\n';
        return 0;
    }
    if (name == "ping" && arguments.size() == 2) {
        const std::vector<std::uint8_t> input(arguments[1].begin(),
                                               arguments[1].end());
        const auto output = client.ping(input);
        std::cout << "pong="
                  << std::string(output.begin(), output.end()) << '\n';
        return 0;
    }
    if (name == "get-info" && arguments.size() == 1) {
        const auto info = client.get_info();
        std::cout << "uuid=";
        const std::vector<std::uint8_t> uuid(info.uuid.begin(),
                                              info.uuid.end());
        print_hex(uuid);
        std::cout << '\n'
                  << "firmware=" << info.firmware_major << '.'
                  << info.firmware_minor << '.' << info.firmware_patch << '\n'
                  << "board_type=0x" << std::hex << info.board_type
                  << std::dec << '\n'
                  << "protocol_version="
                  << static_cast<unsigned>(info.protocol_version) << '\n';
        return 0;
    }
    if (name == "get-capability" && arguments.size() == 1) {
        // 请求失败时不要提前输出半截结果，避免错误信息看起来像协议损坏。
        const auto capabilities = client.get_capabilities();
        std::cout << "capabilities=0x" << std::hex << capabilities
                  << std::dec << '\n';
        return 0;
    }
    if (name == "resource-list" && arguments.size() == 1) {
        for (const auto& resource : client.list_resources()) {
            print_resource(resource, true);
        }
        return 0;
    }
    if (name == "resource-describe" && arguments.size() == 2) {
        print_resource(
            client.describe_resource(parse_u32(arguments[1], "资源 ID")),
            false);
        return 0;
    }
    if (name == "resource-status" && arguments.size() == 2) {
        const auto status =
            client.resource_status(parse_u32(arguments[1], "资源 ID"));
        std::cout << "resource_id=0x" << std::hex << status.resource_id
                  << std::dec << " health="
                  << static_cast<unsigned>(status.health)
                  << " error_flags=0x" << std::hex << status.error_flags
                  << std::dec
                  << " health_name=" << health_name(status.health)
                  << " rx_buffered=" << status.rx_buffered
                  << " tx_buffered=" << status.tx_buffered
                  << " rx_overruns=" << status.rx_overruns
                  << " tx_overruns=" << status.tx_overruns << '\n';
        return 0;
    }
    if (name == "resource-reset" && arguments.size() == 2) {
        client.reset_resource(parse_u32(arguments[1], "资源 ID"));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "gpio-create" &&
        (arguments.size() == 3 || arguments.size() == 4)) {
        const auto pin = parse_u32(arguments[1], "GPIO 引脚");
        if (pin > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("GPIO 引脚超过 16 位范围");
        }
        remotebsp::GpioDirection direction;
        if (arguments[2] == "input") {
            direction = remotebsp::GpioDirection::Input;
        } else if (arguments[2] == "output") {
            direction = remotebsp::GpioDirection::Output;
        } else {
            throw std::invalid_argument("GPIO 方向必须是 input 或 output");
        }
        const auto initial =
            arguments.size() == 4
                ? parse_u32(arguments[3], "GPIO 初始电平")
                : 0;
        if (initial > 1) {
            throw std::invalid_argument("GPIO 电平必须是 0 或 1");
        }
        std::cout << "object_id="
                  << client.gpio_create(static_cast<std::uint16_t>(pin),
                                        direction, initial != 0)
                  << '\n';
        return 0;
    }
    if (name == "gpio-read" && arguments.size() == 2) {
        std::cout << "value="
                  << (client.gpio_read(
                          parse_u32(arguments[1], "GPIO 对象 ID"))
                          ? 1
                          : 0)
                  << '\n';
        return 0;
    }
    if (name == "gpio-write" && arguments.size() == 3) {
        const auto value = parse_u32(arguments[2], "GPIO 电平");
        if (value > 1) {
            throw std::invalid_argument("GPIO 电平必须是 0 或 1");
        }
        client.gpio_write(parse_u32(arguments[1], "GPIO 对象 ID"),
                          value != 0);
        std::cout << "ok\n";
        return 0;
    }
    if (name == "uart-create" &&
        arguments.size() >= 3 && arguments.size() <= 6) {
        const auto port = parse_u32(arguments[1], "UART 端口");
        if (port > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("UART 端口超过 8 位范围");
        }
        remotebsp::UartConfig config;
        config.port = static_cast<std::uint8_t>(port);
        config.baud_rate = parse_u32(arguments[2], "UART 波特率");
        if (arguments.size() >= 4) {
            config.data_bits = static_cast<std::uint8_t>(
                parse_u32(arguments[3], "UART 数据位"));
        }
        if (arguments.size() >= 5) {
            if (arguments[4] == "none") {
                config.parity = remotebsp::UartParity::None;
            } else if (arguments[4] == "odd") {
                config.parity = remotebsp::UartParity::Odd;
            } else if (arguments[4] == "even") {
                config.parity = remotebsp::UartParity::Even;
            } else {
                throw std::invalid_argument(
                    "UART 奇偶校验必须是 none、odd 或 even");
            }
        }
        if (arguments.size() >= 6) {
            config.stop_bits = static_cast<std::uint8_t>(
                parse_u32(arguments[5], "UART 停止位"));
        }
        std::cout << "object_id=" << client.uart_create(config) << '\n';
        return 0;
    }
    if (name == "uart-read" && arguments.size() == 3) {
        const auto data = client.uart_read(
            parse_u32(arguments[1], "UART 对象 ID"),
            parse_u32(arguments[2], "UART 最大读取长度"));
        std::cout << "data_hex=";
        print_hex(data);
        std::cout << '\n';
        return 0;
    }
    if (name == "uart-write" && arguments.size() == 3) {
        const std::vector<std::uint8_t> data(arguments[2].begin(),
                                             arguments[2].end());
        client.uart_write(parse_u32(arguments[1], "UART 对象 ID"), data);
        std::cout << "ok\n";
        return 0;
    }
    if (name == "uart-write-hex" && arguments.size() == 3) {
        client.uart_write(parse_u32(arguments[1], "UART 对象 ID"),
                          parse_hex(arguments[2]));
        std::cout << "ok\n";
        return 0;
    }

    print_usage();
    throw std::invalid_argument("未知命令或参数数量错误");
}

}

int main(int argc, char** argv) {
    try {
        std::string socket_path = "/tmp/toolbusd.sock";
        std::uint32_t node_id = 1;
        int index = 1;
        while (index < argc) {
            const std::string option = argv[index];
            if (option == "--socket" && index + 1 < argc) {
                socket_path = argv[index + 1];
                index += 2;
            } else if (option == "--node" && index + 1 < argc) {
                node_id = parse_u32(argv[index + 1], "节点 ID");
                index += 2;
            } else {
                break;
            }
        }
        std::vector<std::string> arguments;
        for (; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        return run(arguments, socket_path, node_id);
    } catch (const std::exception& error) {
        std::cerr << "remote-cli 错误: " << error.what() << '\n';
        return 1;
    }
}
