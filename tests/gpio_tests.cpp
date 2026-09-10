#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/mock_node.hpp"
#include "remotebsp/protocol/gpio.hpp"

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

void test_close_is_versioned_idempotent_and_safe() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    auto core = make_core(gpio);
    auto create = make_request(protocol::Command::GpioCreate, 20);
    create.payload = {9U, 0U,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Output),
                      1U};
    const auto created = core.handle(create);
    check_status(created, mock_mcu::StatusCode::Ok);
    CHECK(gpio->read(9U));

    auto close = make_request(protocol::Command::GpioClose, 21,
                              created.header.object_id);
    close.payload = protocol::encode_gpio_close();
    gpio->fail_next_write();
    check_status(core.handle(close), mock_mcu::StatusCode::ResourceFailed);
    CHECK(gpio->read(9U));
    check_status(core.handle(close), mock_mcu::StatusCode::Ok);
    CHECK(!gpio->read(9U));

    // 重复或未知对象关闭都表示“确定已不存在”，保持幂等成功。
    check_status(core.handle(close), mock_mcu::StatusCode::Ok);
    auto unknown = close;
    unknown.header.request_id = 22U;
    unknown.header.object_id = 999U;
    check_status(core.handle(unknown), mock_mcu::StatusCode::Ok);

    // 成功关闭后同一引脚可在同一节点代次重新创建。
    create.header.request_id = 23U;
    const auto recreated = core.handle(create);
    check_status(recreated, mock_mcu::StatusCode::Ok);
    CHECK(recreated.header.object_id != created.header.object_id);

    auto invalid = close;
    invalid.header.request_id = 24U;
    invalid.payload.clear();
    check_status(core.handle(invalid), mock_mcu::StatusCode::InvalidPayload);
    invalid.payload = {2U};
    check_status(core.handle(invalid), mock_mcu::StatusCode::InvalidPayload);
    invalid.payload = protocol::encode_gpio_close();
    invalid.header.object_id = 0U;
    check_status(core.handle(invalid), mock_mcu::StatusCode::InvalidPayload);

    CHECK(protocol::decode_gpio_close(protocol::encode_gpio_close()).version ==
          protocol::kGpioClosePayloadVersion);
    try {
        static_cast<void>(protocol::decode_gpio_close({}));
        CHECK(false);
    } catch (const protocol::GpioPayloadException& error) {
        CHECK(error.code() == protocol::GpioPayloadError::InvalidLength);
    }
}

void test_unleased_object_is_session_owned_and_released() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    auto core = make_core(gpio);

    auto create = make_request(protocol::Command::GpioCreate, 30U);
    create.payload = {10U, 0U,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Output),
                      1U};
    const auto created = core.handle(create);
    check_status(created, mock_mcu::StatusCode::Ok);
    CHECK(gpio->read(10U));

    auto other_read = make_request(
        protocol::Command::GpioRead, 31U, created.header.object_id);
    other_read.header.session_id = 2U;
    check_status(core.handle(other_read),
                 mock_mcu::StatusCode::AccessDenied);

    auto other_write = make_request(
        protocol::Command::GpioWrite, 32U, created.header.object_id);
    other_write.header.session_id = 2U;
    other_write.payload = {0U};
    check_status(core.handle(other_write),
                 mock_mcu::StatusCode::AccessDenied);
    CHECK(gpio->read(10U));

    auto other_close = make_request(
        protocol::Command::GpioClose, 33U, created.header.object_id);
    other_close.header.session_id = 2U;
    other_close.payload = protocol::encode_gpio_close();
    check_status(core.handle(other_close),
                 mock_mcu::StatusCode::AccessDenied);
    CHECK(gpio->read(10U));

    CHECK(core.release_session(1U) == 1U);
    CHECK(!gpio->read(10U));
    check_status(core.handle(make_request(
                     protocol::Command::GpioRead, 34U,
                     created.header.object_id)),
                 mock_mcu::StatusCode::ObjectNotFound);
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

void test_input_debounce_events_and_bounded_overflow() {
    auto gpio = std::make_shared<mock_mcu::MockGpioBsp>();
    auto core = make_core(gpio);
    const auto base = mock_mcu::RemoteCore::TimePoint{} +
                      std::chrono::seconds(1);

    auto create = make_request(protocol::Command::GpioCreate, 40U);
    create.payload = {6U, 0U,
                      static_cast<std::uint8_t>(
                          mock_mcu::GpioDirection::Input),
                      0U};
    const auto created = core.handle(create, base);
    check_status(created, mock_mcu::StatusCode::Ok);

    protocol::GpioInputSubscription config;
    config.queue_capacity = 1U;
    config.debounce_us = 1000U;
    auto subscribe = make_request(protocol::Command::GpioInputSubscribe,
                                  41U, created.header.object_id);
    subscribe.payload = protocol::encode_gpio_input_subscription(config);
    check_status(core.handle(subscribe, base), mock_mcu::StatusCode::Ok);

    // 小于 1 ms 的往返抖动不能产生事件。
    gpio->set_input_value(6U, true);
    core.sample_gpio_inputs(base + std::chrono::microseconds(100U));
    gpio->set_input_value(6U, false);
    core.sample_gpio_inputs(base + std::chrono::microseconds(500U));
    CHECK(core.poll_gpio_input_events(
              4U, base + std::chrono::microseconds(999U)).empty());

    // 稳定达到窗口后只产生一个上升沿；先不取走以制造有界队列压力。
    gpio->set_input_value(6U, true);
    core.sample_gpio_inputs(base + std::chrono::microseconds(2000U));
    core.sample_gpio_inputs(base + std::chrono::microseconds(3000U));
    gpio->set_input_value(6U, false);
    core.sample_gpio_inputs(base + std::chrono::microseconds(4000U));
    core.sample_gpio_inputs(base + std::chrono::microseconds(5000U));

    auto status_request = make_request(
        protocol::Command::GpioInputEventStatus, 42U,
        created.header.object_id);
    const auto status_response = core.handle(status_request);
    check_status(status_response, mock_mcu::StatusCode::Ok);
    const auto status = protocol::decode_gpio_input_event_status(
        {status_response.payload.begin() + 1U, status_response.payload.end()});
    CHECK(status.queued_events == 1U);
    CHECK(status.queue_capacity == 1U);
    CHECK(status.dropped_events == 1U);
    CHECK(status.last_sequence == 2U);

    const auto events = core.poll_gpio_input_events(
        1U, base + std::chrono::microseconds(5001U));
    CHECK(events.size() == 1U);
    CHECK(events[0].header.object_id == created.header.object_id);
    CHECK(events[0].header.command == static_cast<std::uint16_t>(
        protocol::Command::GpioInputEvent));
    const auto event = protocol::decode_gpio_input_event(events[0].payload);
    CHECK(event.sequence == 1U);
    CHECK(event.value);
    CHECK(event.edge == protocol::kGpioEdgeRising);
    CHECK(event.timestamp_us == 1003000U);
    CHECK(event.dropped_events == 1U);

    // 输出对象和非所有者都不能订阅输入事件。
    auto other = subscribe;
    other.header.request_id = 43U;
    other.header.session_id = 2U;
    check_status(core.handle(other), mock_mcu::StatusCode::AccessDenied);

    const auto decoded = protocol::decode_gpio_input_subscription(
        protocol::encode_gpio_input_subscription(config));
    CHECK(decoded.debounce_us == 1000U);
    CHECK(decoded.queue_capacity == 1U);
}

}

int main() {
    test_create_read_write();
    test_input_and_errors();
    test_close_is_versioned_idempotent_and_safe();
    test_unleased_object_is_session_owned_and_released();
    test_duplicate_write_is_atomic();
    test_input_debounce_events_and_bounded_overflow();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有远程 GPIO 测试通过\n";
    return 0;
}
