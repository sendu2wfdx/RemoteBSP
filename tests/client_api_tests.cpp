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
    assert(resources.size() == 30);
    const auto descriptor = client.describe_resource(0x02000007);
    assert(descriptor.type ==
           remotebsp::protocol::ResourceType::Uart);
    assert(descriptor.instance == 7);
    const auto status = client.resource_status(0x02000007);
    assert(status.health ==
           remotebsp::protocol::ResourceHealth::Normal);
    const auto contract = client.resource_contract(0x02000007);
    assert(contract.resource_id == 0x02000007);
    assert((contract.access_flags &
            remotebsp::protocol::kResourceAccessLeaseSupported) != 0);
    const auto lease = client.acquire_resource(
        0x02000002, 1000,
        remotebsp::protocol::ResourceLeaseMode::Exclusive);
    assert(lease.lease_id != 0);
    assert(lease.active_lease_count == 1);
    const auto renewed =
        client.renew_resource(0x02000002, lease.lease_id, 2000);
    assert(renewed.granted_duration_ms == 2000);
    assert(client.resource_lease_status(0x02000002)
               .active_lease_count == 1);
    client.release_resource(0x02000002, lease.lease_id);
    assert(client.resource_lease_status(0x02000002)
               .active_lease_count == 0);
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
    config.receive_mode = remotebsp::UartReceiveMode::Streaming;
    const auto uart = client.uart_create(config);
    client.uart_write_all(
        uart, std::vector<std::uint8_t>(256, 0x5A));
    const auto uart_chunk = client.uart_stream_read(uart, 64, 2000);
    assert(uart_chunk.has_value());
    assert(!uart_chunk->data.empty());
    assert(uart_chunk->dropped_bytes == 0);
    assert(uart_chunk->lost_events == 0);

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

    const auto traffic = client.traffic_status();
    assert(traffic.arbitration_bits_per_second != 0);
    assert(traffic.data_bits_per_second != 0);
    assert(traffic.maximum_utilization_permille == 700);
    assert(traffic.admitted_packets != 0);
    assert(traffic.admitted_frames != 0);

    const auto motion_contract = client.motion_contract(true);
    assert(motion_contract.axes.size() == 3U);
    assert(motion_contract.queue_capacity == 32U);
    assert(motion_contract.maximum_total_step_rate_hz == 200000U);
    remotebsp::protocol::MotionSegmentPayload excessive_motion{
        2U, 0U, 1000000000ULL, true,
        {{0x09000000U, 100000},
         {0x09000001U, 100000},
         {0x09000002U, 1}}};
    try {
        static_cast<void>(client.motion_enqueue(excessive_motion));
        assert(false);
    } catch (const remotebsp::ClientException& error) {
        assert(std::string(error.what()).find(
                   "主机运动能力准入拒绝") != std::string::npos);
    }

    /* 放在最后，模拟真实设备回复后进入 Bootloader 的语义。 */
    client.enter_bootloader();
    client.enter_usb_bootloader();
}
