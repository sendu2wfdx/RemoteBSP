#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/mock_node.hpp"

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
    request.header.session_id = 1;
    request.header.request_id = request_id;
    request.header.object_id = object_id;
    return request;
}

mock_mcu::RemoteCore make_core(
    const std::shared_ptr<mock_mcu::MockGpioBsp>& gpio) {
    mock_mcu::NodeInfo info;
    return mock_mcu::RemoteCore(
        info, mock_mcu::capability_mask(mock_mcu::Capability::Gpio), gpio);
}

void check_status(const protocol::Packet& response,
                  mock_mcu::StatusCode status) {
    CHECK(!response.payload.empty());
    CHECK(response.payload[0] == static_cast<std::uint8_t>(status));
    CHECK(response.header.flags ==
          (status == mock_mcu::StatusCode::Ok
               ? 0U
               : mock_mcu::kResponseErrorFlag));
}

void test_create_read_write() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    auto core = make_core(gpio);

    auto create = make_request(protocol::Command::GpioCreate, 1);
    create.payload = {13, 0,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Output),
                      0};
    const auto created = core.handle(create);
    check_status(created, mock_mcu::StatusCode::Ok);
    CHECK(created.header.object_id == 1);
    CHECK(gpio->configure_count() == 1);

    auto write = make_request(protocol::Command::GpioWrite, 2,
                              created.header.object_id);
    write.payload = {1};
    check_status(core.handle(write), mock_mcu::StatusCode::Ok);
    CHECK(gpio->write_count() == 1);

    const auto read = core.handle(make_request(
        protocol::Command::GpioRead, 3, created.header.object_id));
    check_status(read, mock_mcu::StatusCode::Ok);
    CHECK(read.payload.size() == 2);
    CHECK(read.payload[1] == 1);
    CHECK(gpio->read_count() == 1);
}

void test_input_and_errors() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    auto core = make_core(gpio);

    auto create = make_request(protocol::Command::GpioCreate, 1);
    create.payload = {5, 0,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Input),
                      0};
    const auto created = core.handle(create);
    gpio->set_input_value(5, true);
    const auto read = core.handle(make_request(
        protocol::Command::GpioRead, 2, created.header.object_id));
    CHECK(read.payload[1] == 1);

    auto write = make_request(protocol::Command::GpioWrite, 3,
                              created.header.object_id);
    write.payload = {0};
    check_status(core.handle(write), mock_mcu::StatusCode::AccessDenied);
    CHECK(gpio->write_count() == 0);

    check_status(core.handle(make_request(
                     protocol::Command::GpioRead, 4, 999)),
                 mock_mcu::StatusCode::ObjectNotFound);

    auto invalid = make_request(protocol::Command::GpioCreate, 5);
    invalid.payload = {1, 0, 9, 0};
    check_status(core.handle(invalid),
                 mock_mcu::StatusCode::InvalidPayload);

    mock_mcu::RemoteCore unsupported({}, 0);
    check_status(unsupported.handle(create),
                 mock_mcu::StatusCode::UnsupportedCapability);
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

void test_duplicate_write_is_atomic() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    mock_mcu::MockNode node(make_core(gpio), 64, 1);

    auto create = make_request(protocol::Command::GpioCreate, 1);
    create.payload = {7, 0,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Output),
                      0};
    const auto created = decode_reply(*send_request(node, create, 1));

    auto write = make_request(protocol::Command::GpioWrite, 2,
                              created.header.object_id);
    write.payload = {1};
    CHECK(send_request(node, write, 2).has_value());
    CHECK(gpio->write_count() == 1);

    const auto retried = send_request(node, write, 3);
    CHECK(retried.has_value());
    check_status(decode_reply(*retried), mock_mcu::StatusCode::Ok);
    CHECK(gpio->write_count() == 1);
    CHECK(node.cached_response_count() == 1);
}

}

int main() {
    test_create_read_write();
    test_input_and_errors();
    test_duplicate_write_is_atomic();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有远程 GPIO 测试通过\n";
    return 0;
}
