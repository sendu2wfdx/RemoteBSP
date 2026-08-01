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

    const remotebsp::protocol::ResourceContract contract{
        0x02000007,
        remotebsp::protocol::kResourceContractVersion,
        static_cast<std::uint16_t>(
            remotebsp::protocol::kResourceAccessReadable |
            remotebsp::protocol::kResourceAccessWritable |
            remotebsp::protocol::kResourceAccessLeaseSupported),
        1000, 500, 5000, 4096, 3000000, 3000000};
    const auto decoded_contract =
        remotebsp::protocol::decode_resource_contract(
            remotebsp::protocol::encode_resource_contract(contract));
    assert(decoded_contract.resource_id == contract.resource_id);
    assert(decoded_contract.version ==
           remotebsp::protocol::kResourceContractVersion);
    assert(decoded_contract.queue_capacity == 4096);
    assert(decoded_contract.maximum_tx_bits_per_second == 3000000);

    const remotebsp::protocol::ResourceLeaseRequest lease_request{
        0x02000007, 1000,
        remotebsp::protocol::ResourceLeaseMode::Exclusive};
    const auto decoded_lease_request =
        remotebsp::protocol::decode_resource_lease_request(
            remotebsp::protocol::encode_resource_lease_request(
                lease_request));
    assert(decoded_lease_request.resource_id == 0x02000007);
    assert(decoded_lease_request.duration_ms == 1000);
    assert(decoded_lease_request.mode ==
           remotebsp::protocol::ResourceLeaseMode::Exclusive);

    const remotebsp::protocol::ResourceLeaseTokenRequest token_request{
        0x02000007, 0x1122334455667788ULL, 2000};
    const auto decoded_token_request =
        remotebsp::protocol::decode_resource_lease_token_request(
            remotebsp::protocol::encode_resource_lease_token_request(
                token_request));
    assert(decoded_token_request.lease_id ==
           0x1122334455667788ULL);
    assert(decoded_token_request.duration_ms == 2000);

    const remotebsp::protocol::ResourceLeaseInfo lease_info{
        0x02000007, 42, 7, 2000, 1500,
        remotebsp::protocol::ResourceLeaseMode::Exclusive, 1};
    const auto decoded_lease_info =
        remotebsp::protocol::decode_resource_lease_info(
            remotebsp::protocol::encode_resource_lease_info(lease_info));
    assert(decoded_lease_info.lease_id == 42);
    assert(decoded_lease_info.owner_session_id == 7);
    assert(decoded_lease_info.remaining_ms == 1500);
    assert(decoded_lease_info.active_lease_count == 1);

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
        nullptr, uart, resources, {contract});

    const auto contract_response = core.handle(request(
        Command::ResourceContract,
        remotebsp::protocol::encode_resource_id(0x02000007)));
    const auto remote_contract =
        remotebsp::protocol::decode_resource_contract(
            response_body(contract_response));
    assert(remote_contract.resource_id == 0x02000007);
    assert(remote_contract.worst_case_latency_us == 500);

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
