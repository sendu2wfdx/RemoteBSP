#include "remotebsp/mock_mcu/device_parameter_store.hpp"
#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/device_parameters.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>

using namespace remotebsp;

namespace {

protocol::Packet request(protocol::Command command,
                         std::vector<std::uint8_t> payload = {},
                         std::uint32_t session = 0x1234U) {
    protocol::Packet packet;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.message_type = protocol::MessageType::Request;
    packet.header.session_id = session;
    packet.payload = std::move(payload);
    return packet;
}

std::vector<std::uint8_t> body(const protocol::Packet& response) {
    assert(!response.payload.empty());
    assert(response.payload.front() == 0U);
    return {response.payload.begin() + 1U, response.payload.end()};
}

}

int main() {
    auto store = std::make_shared<mock_mcu::DeviceParameterStore>();
    mock_mcu::NodeInfo info;
    mock_mcu::RemoteCore core(
        info,
        mock_mcu::capability_mask(
            mock_mcu::Capability::DeviceParameters),
        nullptr, nullptr, {}, {}, nullptr, nullptr, store);
    const auto now = mock_mcu::RemoteCore::Clock::now();

    const auto status = protocol::decode_device_parameter_status(body(
        core.handle(request(protocol::Command::DeviceParameterStatus), now)));
    assert(status.generation == 0U);
    assert(status.definition_count == rbsp_device_param_definition_count());

    const auto definitions = protocol::decode_device_parameter_descriptors(
        body(core.handle(request(protocol::Command::DeviceParameterList), now)));
    assert(!definitions.empty());

    const auto unlock = protocol::decode_device_parameter_unlock_response(
        body(core.handle(
            request(
                protocol::Command::DeviceParameterUnlock,
                protocol::encode_device_parameter_unlock_request(
                    {0U, protocol::kDeviceParameterUnlockConfirmation})),
            now)));
    assert(unlock.token != 0U);

    const std::vector<std::uint8_t> serial{
        'R', 'B', 'S', 'P', '-', 'T', 'E', 'S', 'T'};
    const auto written = protocol::decode_device_parameter_status(body(
        core.handle(
            request(
                protocol::Command::DeviceParameterWrite,
                protocol::encode_device_parameter_write_request(
                    {0U, unlock.token, RBSP_DEVICE_PARAM_SERIAL_NUMBER,
                     serial})),
            now)));
    assert(written.generation == 1U);
    assert((written.flags & static_cast<std::uint16_t>(
               protocol::DeviceParameterStatusFlag::RestartRequired)) != 0U);

    const auto value = protocol::decode_device_parameter_value(body(
        core.handle(
            request(protocol::Command::DeviceParameterRead,
                    protocol::encode_device_parameter_read_request(
                        RBSP_DEVICE_PARAM_SERIAL_NUMBER)),
            now)));
    assert(value.value == serial);

    auto expired_response = core.handle(
        request(
            protocol::Command::DeviceParameterWrite,
            protocol::encode_device_parameter_write_request(
                {1U, unlock.token, RBSP_DEVICE_PARAM_HARDWARE_REVISION,
                 {'V', '1'}})),
        now + std::chrono::seconds(61));
    assert(expired_response.payload.front() == static_cast<std::uint8_t>(
        mock_mcu::StatusCode::AccessDenied));

    std::cout << "Mock设备参数维护事务测试通过\n";
    return 0;
}
