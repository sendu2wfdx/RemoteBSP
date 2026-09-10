#include "remotebsp/mock_mcu/bus_bsp.hpp"
#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace remotebsp;

protocol::Packet request(protocol::Command command,
                         std::vector<std::uint8_t> payload) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.session_id = 1;
    packet.header.request_id = 1;
    packet.payload = std::move(payload);
    return packet;
}

std::vector<std::uint8_t> body(const protocol::Packet& response) {
    assert(!response.payload.empty());
    assert(response.payload.front() == static_cast<std::uint8_t>(
                                          mock_mcu::StatusCode::Ok));
    return {response.payload.begin() + 1, response.payload.end()};
}

}  // namespace

int main() {
    constexpr std::uint32_t i2c_bus_id = 0x0B000000;
    constexpr std::uint32_t i2c_device_id = 0x0C000001;
    constexpr std::uint32_t spi_bus_id = 0x0D000000;
    constexpr std::uint32_t spi_device_id = 0x0E000001;

    const protocol::BusResourceContract i2c_contract{
        i2c_device_id, protocol::kBusResourceContractVersion,
        protocol::BusResourceKind::I2cDevice,
        static_cast<std::uint8_t>(protocol::kBusContractRepeatedStart |
                                  protocol::kBusContractRecovery),
        i2c_bus_id, 400000, 16, 2, 100, 10000, 1000};
    const protocol::BusResourceContract spi_contract{
        spi_device_id, protocol::kBusResourceContractVersion,
        protocol::BusResourceKind::SpiDevice,
        static_cast<std::uint8_t>(protocol::kBusContractFullDuplex |
                                  protocol::kBusContractKeepChipSelect),
        spi_bus_id, 12000000, 16, 2, 100, 10000, 1000};

    auto bus = std::make_shared<mock_mcu::MockBusBsp>();
    bus->add_device(i2c_contract, {0, 1, 2, 3, 4, 5, 6, 7});
    bus->add_device(spi_contract);
    bus->set_spi_response(spi_device_id, {0xAA, 0x55, 0x11});

    const std::vector<protocol::ResourceDescriptor> resources{
        {i2c_bus_id, protocol::ResourceType::I2cBus, 0,
         protocol::kResourceFlagNative, 0, 0},
        {i2c_device_id, protocol::ResourceType::I2cDevice, 1,
         protocol::kResourceFlagNative, 16, 16},
        {spi_bus_id, protocol::ResourceType::SpiBus, 0,
         protocol::kResourceFlagNative, 0, 0},
        {spi_device_id, protocol::ResourceType::SpiDevice, 1,
         protocol::kResourceFlagExpanded, 16, 16},
    };
    const auto access = static_cast<std::uint16_t>(
        protocol::kResourceAccessReadable |
        protocol::kResourceAccessWritable |
        protocol::kResourceAccessExclusiveWrite |
        protocol::kResourceAccessLeaseSupported);
    const std::vector<protocol::ResourceContract> resource_contracts{
        {i2c_device_id, protocol::kResourceContractVersion, access,
         1000U, 100U, 10000U, 16U, 0U, 0U},
        {spi_device_id, protocol::kResourceContractVersion, access,
         1000U, 100U, 10000U, 16U, 0U, 0U},
    };
    mock_mcu::RemoteCore core(
        {}, mock_mcu::capability_mask(mock_mcu::Capability::I2c) |
                mock_mcu::capability_mask(mock_mcu::Capability::Spi),
        nullptr, nullptr, resources, resource_contracts,
        nullptr, nullptr, nullptr, bus);

    const auto remote_i2c_contract =
        protocol::decode_bus_resource_contract(body(core.handle(request(
            protocol::Command::I2cContract,
            protocol::encode_resource_id(i2c_device_id)))));
    assert(remote_i2c_contract.parent_bus_resource_id == i2c_bus_id);

    const auto i2c_result = protocol::decode_bus_transfer_result(body(
        core.handle(request(protocol::Command::I2cTransfer,
                            protocol::encode_i2c_transfer_request(
                                {i2c_device_id, 1000,
                                 protocol::kI2cTransferRepeatedStart, 3,
                                 {2}})))));
    assert(i2c_result.status == protocol::BusTransactionStatus::Ok);
    assert(i2c_result.data == std::vector<std::uint8_t>({2, 3, 4}));

    bus->set_next_status(i2c_device_id,
                         protocol::BusTransactionStatus::Nack);
    const auto nack = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::I2cTransfer,
                protocol::encode_i2c_transfer_request(
                    {i2c_device_id, 1000, 0, 1, {0}})))));
    assert(nack.status == protocol::BusTransactionStatus::Nack);

    // I2C 设备的 NACK 不污染同节点另一条 SPI 设备资源。
    const auto spi_result = protocol::decode_bus_transfer_result(body(
        core.handle(request(protocol::Command::SpiTransfer,
                            protocol::encode_spi_transfer_request(
                                {spi_device_id, 1000, 0, 3, 0xFF,
                                 {0x80}})))));
    assert(spi_result.status == protocol::BusTransactionStatus::Ok);
    assert(spi_result.data ==
           std::vector<std::uint8_t>({0xAA, 0x55, 0x11}));

    const auto timeout = protocol::decode_bus_transfer_result(body(
        core.handle(request(protocol::Command::SpiTransfer,
                            protocol::encode_spi_transfer_request(
                                {spi_device_id, 50, 0, 1, 0xFF,
                                 {0x80}})))));
    assert(timeout.status == protocol::BusTransactionStatus::Timeout);

    const auto limited = protocol::decode_bus_transfer_result(body(
        core.handle(request(protocol::Command::SpiTransfer,
                            protocol::encode_spi_transfer_request(
                                {spi_device_id, 1000, 0, 17, 0xFF,
                                 std::vector<std::uint8_t>(8, 0)})))));
    assert(limited.status == protocol::BusTransactionStatus::LimitExceeded);

    // 总线恢复必须持有目标设备独占租约；单设备 poison 不污染同节点其他资源。
    auto reset = core.handle(request(
        protocol::Command::ResourceReset,
        protocol::encode_resource_id(i2c_device_id)));
    assert(reset.payload.front() == static_cast<std::uint8_t>(
        mock_mcu::StatusCode::AccessDenied));
    body(core.handle(request(
        protocol::Command::ResourceAcquire,
        protocol::encode_resource_lease_request(
            {i2c_device_id, 1000U,
             protocol::ResourceLeaseMode::Exclusive}))));
    bus->set_next_status(i2c_device_id,
                         protocol::BusTransactionStatus::Fault);
    const auto fault = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::I2cTransfer,
                protocol::encode_i2c_transfer_request(
                    {i2c_device_id, 1000, 0, 1, {0}})))));
    assert(fault.status == protocol::BusTransactionStatus::Fault);
    const auto poisoned = protocol::decode_resource_status(body(core.handle(
        request(protocol::Command::ResourceStatus,
                protocol::encode_resource_id(i2c_device_id)))));
    assert(poisoned.health == protocol::ResourceHealth::Failed);
    const auto peer = protocol::decode_resource_status(body(core.handle(
        request(protocol::Command::ResourceStatus,
                protocol::encode_resource_id(spi_device_id)))));
    assert(peer.health == protocol::ResourceHealth::Normal);
    bus->set_reset_failure(i2c_device_id, true);
    reset = core.handle(request(protocol::Command::ResourceReset,
                                protocol::encode_resource_id(i2c_device_id)));
    assert(reset.payload.front() == static_cast<std::uint8_t>(
        mock_mcu::StatusCode::ResourceFailed));
    assert(bus->failed(i2c_device_id));
    bus->set_reset_failure(i2c_device_id, false);
    body(core.handle(request(protocol::Command::ResourceReset,
                             protocol::encode_resource_id(i2c_device_id))));
    assert(!bus->failed(i2c_device_id));
    reset = core.handle(request(protocol::Command::ResourceReset,
                                protocol::encode_resource_id(i2c_device_id)));
    assert(reset.payload.front() == static_cast<std::uint8_t>(
        mock_mcu::StatusCode::AccessDenied));
}
