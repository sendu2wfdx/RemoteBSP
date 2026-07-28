#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/packet.hpp"

#include <cstdint>
#include <iostream>
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

protocol::Packet make_request(protocol::Command command) {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(command);
    request.header.session_id = 0x11223344;
    request.header.request_id = 0x55667788;
    request.header.object_id = 0x1234;
    return request;
}

mock_mcu::RemoteCore make_core() {
    mock_mcu::NodeInfo info;
    for (std::size_t index = 0; index < info.uuid.size(); ++index) {
        info.uuid[index] = static_cast<std::uint8_t>(index);
    }
    info.firmware_major = 1;
    info.firmware_minor = 2;
    info.firmware_patch = 3;
    info.board_type = 0xAABBCCDD;
    const auto capabilities =
        mock_mcu::capability_mask(mock_mcu::Capability::Gpio) |
        mock_mcu::capability_mask(mock_mcu::Capability::Spi) |
        mock_mcu::capability_mask(mock_mcu::Capability::I2c);
    return mock_mcu::RemoteCore(info, capabilities);
}

void check_common_response(const protocol::Packet& response,
                           protocol::Command command) {
    CHECK(response.header.message_type == protocol::MessageType::Response);
    CHECK(response.header.command == static_cast<std::uint16_t>(command));
    CHECK(response.header.session_id == 0x11223344);
    CHECK(response.header.request_id == 0x55667788);
    CHECK(response.header.object_id == 0x1234);
}

void test_get_info() {
    const auto response =
        make_core().handle(make_request(protocol::Command::GetInfo));
    check_common_response(response, protocol::Command::GetInfo);
    CHECK(response.header.flags == 0);
    CHECK(response.payload.size() == 28);
    CHECK(response.payload[0] ==
          static_cast<std::uint8_t>(mock_mcu::StatusCode::Ok));
    CHECK(response.payload[1] == 0);
    CHECK(response.payload[16] == 15);
    CHECK(response.payload[17] == 1);
    CHECK(response.payload[19] == 2);
    CHECK(response.payload[21] == 3);
    CHECK(response.payload[23] == 0xDD);
    CHECK(response.payload[26] == 0xAA);
    CHECK(response.payload[27] == protocol::kProtocolVersion);

    const auto decoded = protocol::decode(protocol::encode(response));
    CHECK(decoded.payload == response.payload);
}

void test_get_capability() {
    const auto response =
        make_core().handle(make_request(protocol::Command::GetCapability));
    check_common_response(response, protocol::Command::GetCapability);
    CHECK(response.payload.size() == 9);
    CHECK(response.payload[1] == 0x0D);
    for (std::size_t index = 2; index < response.payload.size(); ++index) {
        CHECK(response.payload[index] == 0);
    }
}

void test_ping() {
    auto request = make_request(protocol::Command::Ping);
    request.payload = {0, 1, 2, 3, 0xFF};
    const auto response = make_core().handle(request);
    check_common_response(response, protocol::Command::Ping);
    CHECK(response.payload ==
          std::vector<std::uint8_t>({0, 0, 1, 2, 3, 0xFF}));

    request.payload.resize(protocol::kMaximumPayloadSize);
    const auto too_large_response = make_core().handle(request);
    CHECK(too_large_response.header.flags == mock_mcu::kResponseErrorFlag);
    CHECK(too_large_response.payload ==
          std::vector<std::uint8_t>({static_cast<std::uint8_t>(
              mock_mcu::StatusCode::InvalidPayload)}));
}

void test_errors() {
    auto invalid_payload = make_request(protocol::Command::GetInfo);
    invalid_payload.payload = {1};
    const auto invalid_response = make_core().handle(invalid_payload);
    CHECK(invalid_response.header.flags == mock_mcu::kResponseErrorFlag);
    CHECK(invalid_response.payload ==
          std::vector<std::uint8_t>({static_cast<std::uint8_t>(
              mock_mcu::StatusCode::InvalidPayload)}));

    auto unknown = make_request(protocol::Command::Ping);
    unknown.header.command = 0xFFFF;
    const auto unknown_response = make_core().handle(unknown);
    CHECK(unknown_response.header.flags == mock_mcu::kResponseErrorFlag);
    CHECK(unknown_response.payload[0] == static_cast<std::uint8_t>(
                                              mock_mcu::StatusCode::UnknownCommand));

    auto not_request = make_request(protocol::Command::Ping);
    not_request.header.message_type = protocol::MessageType::Event;
    try {
        make_core().handle(not_request);
        CHECK(false);
    } catch (const mock_mcu::CoreException& error) {
        CHECK(error.code() == mock_mcu::CoreError::NotRequest);
    }
}

}

int main() {
    test_get_info();
    test_get_capability();
    test_ping();
    test_errors();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有 Mock MCU 远程核心测试通过\n";
    return 0;
}
