#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using remotebsp::protocol::Command;
using remotebsp::protocol::MessageType;
using remotebsp::protocol::Packet;
using remotebsp::protocol::ResourceDescriptor;
using remotebsp::protocol::ResourceHealth;
using remotebsp::protocol::ResourceType;

Packet request(Command command, std::vector<std::uint8_t> payload = {}) {
    Packet packet;
    packet.header.message_type = MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.request_id = 7;
    packet.payload = std::move(payload);
    return packet;
}

std::vector<std::uint8_t> response_body(const Packet& response) {
    assert(!response.payload.empty());
    assert(response.payload[0] == 0);
    return {response.payload.begin() + 1, response.payload.end()};
}

}

int main() {
    const std::vector<ResourceDescriptor> resources{
        {0x02000000, ResourceType::Uart, 0,
         remotebsp::protocol::kResourceFlagNative, 4096, 4096},
        {0x02000007, ResourceType::Uart, 7,
         remotebsp::protocol::kResourceFlagExpanded, 4096, 4096},
    };

    const auto encoded = remotebsp::protocol::encode_resource_list(resources);
    const auto decoded = remotebsp::protocol::decode_resource_list(encoded);
    assert(decoded.size() == 2);
    assert(decoded[1].resource_id == 0x02000007);
    assert(decoded[1].instance == 7);
    assert((decoded[1].flags &
            remotebsp::protocol::kResourceFlagExpanded) != 0);

    auto uart =
        std::make_shared<remotebsp::mock_mcu::MockUartBsp>(4, 4);
    uart->configure(7, {115200, 8, 1,
                        remotebsp::mock_mcu::UartParity::None});
    uart->inject_rx(7, {1, 2, 3, 4, 5, 6});

    remotebsp::mock_mcu::NodeInfo info;
    remotebsp::mock_mcu::RemoteCore core(
        info,
        remotebsp::mock_mcu::capability_mask(
            remotebsp::mock_mcu::Capability::Uart),
        nullptr, uart, resources);

    const auto list_response = core.handle(request(Command::ResourceEnum));
    assert(remotebsp::protocol::decode_resource_list(
               response_body(list_response))
               .size() == 2);

    const auto describe_response = core.handle(request(
        Command::ResourceDescribe,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    const auto descriptor = remotebsp::protocol::decode_resource_descriptor(
        response_body(describe_response));
    assert(descriptor.instance == 7);

    const auto status_response = core.handle(request(
        Command::ResourceStatus,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    const auto status = remotebsp::protocol::decode_resource_status(
        response_body(status_response));
    assert(status.resource_id == 0x02000007);
    assert(status.health == ResourceHealth::Degraded);
    assert((status.error_flags &
            remotebsp::protocol::kResourceErrorRxOverflow) != 0);
    assert(status.rx_buffered == 4);
    assert(status.rx_overruns == 2);

    uart->set_failed(7, true);
    const auto failed_response = core.handle(request(
        Command::ResourceStatus,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    const auto failed = remotebsp::protocol::decode_resource_status(
        response_body(failed_response));
    assert(failed.health == ResourceHealth::Failed);
    assert((failed.error_flags &
            remotebsp::protocol::kResourceErrorBackendFailure) != 0);

    const auto reset_response = core.handle(request(
        Command::ResourceReset,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    assert(reset_response.payload.size() == 1);
    assert(reset_response.payload[0] == 0);
    const auto reset_status_response = core.handle(request(
        Command::ResourceStatus,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    const auto reset_status =
        remotebsp::protocol::decode_resource_status(
            response_body(reset_status_response));
    assert(reset_status.health == ResourceHealth::Normal);
    assert(reset_status.error_flags == 0);
    assert(reset_status.rx_buffered == 0);

    const auto healthy_response = core.handle(request(
        Command::ResourceStatus,
        remotebsp::protocol::encode_resource_id(0x02000000)));
    const auto healthy = remotebsp::protocol::decode_resource_status(
        response_body(healthy_response));
    assert(healthy.health == ResourceHealth::Normal);
    assert(healthy.error_flags == 0);

    const auto missing_response = core.handle(request(
        Command::ResourceStatus,
        remotebsp::protocol::encode_resource_id(0x0200FFFF)));
    assert(missing_response.payload[0] ==
           static_cast<std::uint8_t>(
               remotebsp::mock_mcu::StatusCode::ObjectNotFound));
}
