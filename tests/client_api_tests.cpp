#include "remotebsp/client.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// 标准 assert 在 Release/NDEBUG 下会被移除；进程级闭环必须保留检查。
#define assert(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("测试断言失败: " #condition);           \
        }                                                                     \
    } while (false)

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
    const auto node_health = client.node_health_snapshot();
    assert(node_health.source ==
           remotebsp::protocol::HealthSource::RemoteCore);
    // toolbusd 的健康生产者与客户端请求可能并发采样；只要求序号有效且单调，
    // 不能把“第一次由本测试读取”误当成“节点的第一次采样”。
    assert(node_health.sample_sequence > 0U);
    assert(node_health.producer_generation != 0U);
    const auto* node_cpu = remotebsp::protocol::find_health_metric(
        node_health, remotebsp::protocol::HealthMetricId::CpuLoadPermille);
    assert(node_cpu != nullptr && node_cpu->availability ==
           remotebsp::protocol::MetricAvailability::Unavailable);

    const auto resources = client.list_resources();
    assert(resources.size() == 35);
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
    client.gpio_close(gpio);
    client.gpio_close(gpio);

    const auto gpio_input = client.gpio_create(
        11, remotebsp::GpioDirection::Input, false);
    remotebsp::protocol::GpioInputSubscription gpio_subscription;
    gpio_subscription.debounce_us = 2500U;
    gpio_subscription.queue_capacity = 4U;
    client.gpio_input_subscribe(gpio_input, gpio_subscription);
    const auto gpio_event_status =
        client.gpio_input_event_status(gpio_input);
    assert(gpio_event_status.queued_events == 0U);
    assert(gpio_event_status.queue_capacity == 4U);
    assert(gpio_event_status.dropped_events == 0U);
    try {
        static_cast<void>(client.gpio_input_event_status(0U));
        assert(false);
    } catch (const remotebsp::ClientException&) {
    }
    auto invalid_gpio_subscription = gpio_subscription;
    invalid_gpio_subscription.queue_capacity = 0U;
    try {
        client.gpio_input_subscribe(gpio_input, invalid_gpio_subscription);
        assert(false);
    } catch (const remotebsp::protocol::GpioPayloadException&) {
    }
    client.gpio_close(gpio_input);

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
