#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

protocol::Packet make_request(protocol::Command command,
                              std::uint32_t request_id,
                              std::uint32_t object_id = 0) {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(command);
    request.header.session_id = 2;
    request.header.request_id = request_id;
    request.header.object_id = object_id;
    return request;
}

mock_mcu::RemoteCore make_core(
    const std::shared_ptr<mock_mcu::MockUartBsp>& uart) {
    mock_mcu::NodeInfo info;
    return mock_mcu::RemoteCore(
        info, mock_mcu::capability_mask(mock_mcu::Capability::Uart),
        nullptr, uart);
}

void check_status(const protocol::Packet& response,
                  mock_mcu::StatusCode status) {
    CHECK(!response.payload.empty());
    CHECK(response.payload[0] == static_cast<std::uint8_t>(status));
}

protocol::Packet create_uart(mock_mcu::RemoteCore& core,
                             std::uint32_t request_id = 1,
                             bool streaming = false) {
    auto request = make_request(protocol::Command::UartCreate, request_id);
    request.payload = {2, 0x00, 0xC2, 0x01, 0x00, 8, 1,
                       static_cast<std::uint8_t>(
                           mock_mcu::UartParity::None)};
    if (streaming) {
        request.payload.push_back(1U);
    }
    return core.handle(request);
}

void test_create_write_read() {
    auto uart = std::make_shared<mock_mcu::MockUartBsp>();
    auto core = make_core(uart);
    const auto created = create_uart(core);
    check_status(created, mock_mcu::StatusCode::Ok);
    CHECK(created.header.object_id == 1);
    CHECK(uart->configure_count() == 1);
    const auto* config = uart->config(2);
    CHECK(config != nullptr);
    CHECK(config->baud_rate == 115200);
    CHECK(config->data_bits == 8);
    CHECK(config->stop_bits == 1);
    CHECK(config->parity == mock_mcu::UartParity::None);

    auto write = make_request(protocol::Command::UartWrite, 2,
                              created.header.object_id);
    write.payload = {0x01, 0x02, 0xFF};
    check_status(core.handle(write), mock_mcu::StatusCode::Ok);
    CHECK(uart->take_tx(2) ==
          std::vector<std::uint8_t>({0x01, 0x02, 0xFF}));

    uart->inject_rx(2, {10, 11, 12, 13});
    auto read = make_request(protocol::Command::UartRead, 3,
                             created.header.object_id);
    read.payload = {3, 0};
    const auto first = core.handle(read);
    CHECK(first.payload ==
          std::vector<std::uint8_t>({0, 10, 11, 12}));
    const auto second = core.handle(read);
    CHECK(second.payload == std::vector<std::uint8_t>({0, 13}));
}

void test_validation() {
    auto uart = std::make_shared<mock_mcu::MockUartBsp>();
    auto core = make_core(uart);

    auto invalid = make_request(protocol::Command::UartCreate, 1);
    invalid.payload = {0, 0, 0, 0, 0, 8, 1, 0};
    check_status(core.handle(invalid),
                 mock_mcu::StatusCode::InvalidPayload);

    const auto created = create_uart(core, 2);
    check_status(create_uart(core, 20),
                 mock_mcu::StatusCode::ResourceBusy);
    auto empty_write = make_request(protocol::Command::UartWrite, 3,
                                    created.header.object_id);
    check_status(core.handle(empty_write),
                 mock_mcu::StatusCode::InvalidPayload);

    auto unknown = make_request(protocol::Command::UartRead, 4, 999);
    unknown.payload = {1, 0};
    check_status(core.handle(unknown),
                 mock_mcu::StatusCode::ObjectNotFound);

    mock_mcu::RemoteCore unsupported({}, 0);
    check_status(unsupported.handle(invalid),
                 mock_mcu::StatusCode::UnsupportedCapability);
}

void test_buffer_and_fault_isolation() {
    auto uart = std::make_shared<mock_mcu::MockUartBsp>(4, 4);
    auto core = make_core(uart);
    const auto first = create_uart(core, 1);
    check_status(first, mock_mcu::StatusCode::Ok);

    auto full_write = make_request(protocol::Command::UartWrite, 2,
                                   first.header.object_id);
    full_write.payload = {1, 2, 3, 4};
    check_status(core.handle(full_write), mock_mcu::StatusCode::Ok);

    auto overflow = make_request(protocol::Command::UartWrite, 3,
                                 first.header.object_id);
    overflow.payload = {5};
    check_status(core.handle(overflow),
                 mock_mcu::StatusCode::ResourceExhausted);
    CHECK(uart->status(2).tx_buffered == 4);
    CHECK(uart->status(2).tx_overruns == 1);

    uart->set_failed(2, true);
    auto failed_read = make_request(protocol::Command::UartRead, 4,
                                    first.header.object_id);
    failed_read.payload = {1, 0};
    check_status(core.handle(failed_read),
                 mock_mcu::StatusCode::ResourceFailed);

    auto second_create =
        make_request(protocol::Command::UartCreate, 5);
    second_create.payload = {3, 0x00, 0xC2, 0x01, 0x00, 8, 1, 0};
    const auto second = core.handle(second_create);
    check_status(second, mock_mcu::StatusCode::Ok);
    uart->inject_rx(3, {0x42});
    auto healthy_read = make_request(protocol::Command::UartRead, 6,
                                     second.header.object_id);
    healthy_read.payload = {1, 0};
    const auto response = core.handle(healthy_read);
    check_status(response, mock_mcu::StatusCode::Ok);
    CHECK(response.payload ==
          std::vector<std::uint8_t>({0, 0x42}));
}

std::optional<mock_mcu::NodeReply> send_request(
    mock_mcu::MockNode& node, const protocol::Packet& request,
    std::uint16_t transfer_id) {
    const auto frames = protocol::Fragmenter(64).split(
        protocol::encode(request), transfer_id);
    std::optional<mock_mcu::NodeReply> reply;
    for (const auto& frame : frames) {
        const auto current = node.handle_frame(0x600, frame);
        if (current.has_value()) {
            reply = current;
        }
    }
    return reply;
}

protocol::Packet decode_reply(const mock_mcu::NodeReply& reply) {
    protocol::Reassembler reassembler(64, std::chrono::milliseconds(100));
    protocol::ReassemblyResult result;
    for (const auto& frame : reply.frames) {
        result = reassembler.accept(0x580, frame);
    }
    return protocol::decode(*result.packet);
}

void test_duplicate_write() {
    auto uart = std::make_shared<mock_mcu::MockUartBsp>();
    mock_mcu::MockNode node(make_core(uart), 64, 1);

    auto create_request =
        make_request(protocol::Command::UartCreate, 1);
    create_request.payload = {0, 0x00, 0xE1, 0x00, 0x00, 8, 1, 0};
    const auto created =
        decode_reply(*send_request(node, create_request, 1));

    auto write = make_request(protocol::Command::UartWrite, 2,
                              created.header.object_id);
    write.payload = {'A', 'B', 'C'};
    CHECK(send_request(node, write, 2).has_value());
    CHECK(uart->write_count() == 1);

    CHECK(send_request(node, write, 3).has_value());
    CHECK(uart->write_count() == 1);
    CHECK(uart->take_tx(0) ==
          std::vector<std::uint8_t>({'A', 'B', 'C'}));
}

void test_streaming_receive() {
    auto uart = std::make_shared<mock_mcu::MockUartBsp>();
    auto core = make_core(uart);
    const auto created = create_uart(core, 1, true);
    check_status(created, mock_mcu::StatusCode::Ok);

    uart->inject_rx(2, {'G', 'P', 'S', '\n'});
    const auto events = core.poll_uart_events(3);
    CHECK(events.size() == 1);
    if (!events.empty()) {
        CHECK(events[0].header.message_type ==
              protocol::MessageType::Event);
        CHECK(events[0].header.command ==
              static_cast<std::uint16_t>(
                  protocol::Command::UartRxEvent));
        CHECK(events[0].header.object_id ==
              created.header.object_id);
        CHECK(events[0].header.request_id == 1);
        CHECK(events[0].payload ==
              std::vector<std::uint8_t>({'G', 'P', 'S'}));
    }
    const auto second = core.poll_uart_events(3);
    CHECK(second.size() == 1);
    if (!second.empty()) {
        CHECK(second[0].header.request_id == 2);
        CHECK(second[0].payload ==
              std::vector<std::uint8_t>({'\n'}));
    }

    auto read = make_request(protocol::Command::UartRead, 2,
                             created.header.object_id);
    read.payload = {1, 0};
    check_status(core.handle(read),
                 mock_mcu::StatusCode::AccessDenied);
}

}

int main() {
    test_create_write_read();
    test_validation();
    test_duplicate_write();
    test_buffer_and_fault_isolation();
    test_streaming_receive();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有远程 UART 测试通过\n";
    return 0;
}
