#include "remotebsp/client.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    const remotebsp::Client client(argv[1]);

    const auto nodes = client.list_nodes();
    assert(nodes.size() == 1);
    assert(nodes[0].node_id == 1);
    assert(nodes[0].online);
    assert(nodes[0].ready);

    const std::vector<std::uint8_t> ping{'l', 'i', 'b'};
    assert(client.ping(ping) == ping);

    const auto info = client.get_info();
    assert(info.protocol_version ==
           remotebsp::protocol::kProtocolVersion);
    assert(client.get_capabilities() != 0);

    const auto resources = client.list_resources();
    assert(resources.size() == 24);
    const auto descriptor = client.describe_resource(0x02000007);
    assert(descriptor.type ==
           remotebsp::protocol::ResourceType::Uart);
    assert(descriptor.instance == 7);
    const auto status = client.resource_status(0x02000007);
    assert(status.health ==
           remotebsp::protocol::ResourceHealth::Normal);
    const auto event = client.next_event();
    assert(event.has_value());
    assert(event->header.command == static_cast<std::uint16_t>(
                                        remotebsp::protocol::Command::
                                            UartRxEvent));
    assert(event->header.object_id == 0x02000007);

    const auto gpio = client.gpio_create(
        12, remotebsp::GpioDirection::Output, false);
    client.gpio_write(gpio, true);
    assert(client.gpio_read(gpio));

    remotebsp::UartConfig config;
    config.port = 1;
    config.baud_rate = 115200;
    const auto uart = client.uart_create(config);
    client.uart_write(uart, {'l', 'i', 'b'});

    std::atomic<unsigned> successes{0};
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < 8; ++index) {
        threads.emplace_back([&client, &successes, index] {
            const std::string text = "thread-" + std::to_string(index);
            const std::vector<std::uint8_t> request(text.begin(), text.end());
            if (client.ping(request) == request) {
                ++successes;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    assert(successes == 8);
}
