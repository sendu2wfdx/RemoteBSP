#include "remotebsp/toolbusd/bus_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace remotebsp;

protocol::BusResourceContract i2c_contract(
    std::uint32_t device, std::uint32_t parent) {
    return {device, protocol::kBusResourceContractVersion,
            protocol::BusResourceKind::I2cDevice,
            static_cast<std::uint8_t>(protocol::kBusContractRepeatedStart |
                                      protocol::kBusContractRecovery),
            parent, 400000U, 32U, 2U, 100U, 10000U, 1000U};
}

protocol::BusResourceContract spi_contract(
    std::uint32_t device, std::uint32_t parent) {
    return {device, protocol::kBusResourceContractVersion,
            protocol::BusResourceKind::SpiDevice,
            static_cast<std::uint8_t>(protocol::kBusContractFullDuplex |
                                      protocol::kBusContractKeepChipSelect),
            parent, 12000000U, 32U, 2U, 100U, 10000U, 1000U};
}

void check_contract_cache_bounds() {
    toolbusd::BusRuntime runtime(2U);
    const auto first = i2c_contract(0x0C000001U, 0x0B000000U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, first) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, first) ==
           toolbusd::BusContractUpdate::Unchanged);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice, first) ==
           toolbusd::BusContractUpdate::Invalid);
    auto wrong_device_namespace = first;
    wrong_device_namespace.resource_id = 0x01000001U;
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice,
               wrong_device_namespace) ==
           toolbusd::BusContractUpdate::Invalid);
    auto wrong_parent_namespace = first;
    wrong_parent_namespace.parent_bus_resource_id = 0x0D000000U;
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice,
               wrong_parent_namespace) ==
           toolbusd::BusContractUpdate::Invalid);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice,
               spi_contract(0x0E000001U, 0x0D000000U)) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.remember_contract(
               2U, protocol::BusResourceKind::I2cDevice,
               i2c_contract(0x0C000002U, 0x0B000000U)) ==
           toolbusd::BusContractUpdate::CapacityReached);
    assert(runtime.contract_count() == 2U);
    runtime.invalidate_node(1U);
    assert(runtime.contract_count() == 0U);
}

void check_parent_bus_arbitration_and_isolation() {
    toolbusd::BusRuntime runtime;
    const auto device_a = i2c_contract(0x0C000001U, 0x0B000000U);
    const auto device_b = i2c_contract(0x0C000002U, 0x0B000000U);
    const auto device_c = i2c_contract(0x0C000003U, 0x0B000001U);
    for (const auto& contract : {device_a, device_b, device_c}) {
        assert(runtime.remember_contract(
                   1U, protocol::BusResourceKind::I2cDevice, contract) ==
               toolbusd::BusContractUpdate::Added);
    }
    assert(runtime.remember_contract(
               2U, protocol::BusResourceKind::I2cDevice, device_a) ==
           toolbusd::BusContractUpdate::Added);

    auto held = runtime.admit_i2c(
        1U, {device_a.resource_id, 1000U, 0U, 1U, {0U}});
    assert(held.status == toolbusd::BusAdmissionStatus::Accepted);
    assert(runtime.active_bus_count() == 1U);

    // 同节点、同一父总线的不同子设备必须互斥。
    auto same_bus = runtime.admit_i2c(
        1U, {device_b.resource_id, 1000U, 0U, 1U, {0U}});
    assert(same_bus.status == toolbusd::BusAdmissionStatus::ResourceBusy);

    // 同节点不同总线，以及另一节点的同 ID 总线均不被阻塞。
    auto other_bus = runtime.admit_i2c(
        1U, {device_c.resource_id, 1000U, 0U, 1U, {0U}});
    auto other_node = runtime.admit_i2c(
        2U, {device_a.resource_id, 1000U, 0U, 1U, {0U}});
    assert(other_bus.status == toolbusd::BusAdmissionStatus::Accepted);
    assert(other_node.status == toolbusd::BusAdmissionStatus::Accepted);
    assert(runtime.active_bus_count() == 3U);

    // 异常退出路径由 RAII 释放，不会永久阻塞所属总线。
    held.reservation = {};
    auto retried = runtime.admit_i2c(
        1U, {device_b.resource_id, 1000U, 0U, 1U, {0U}});
    assert(retried.status == toolbusd::BusAdmissionStatus::Accepted);
}

void check_contract_preflight() {
    toolbusd::BusRuntime runtime;
    const auto i2c = i2c_contract(0x0C000001U, 0x0B000000U);
    assert(runtime.admit_i2c(
               1U, {i2c.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::ContractMissing);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, i2c) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.admit_i2c(
               1U, {i2c.resource_id, 50U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::ContractMismatch);
    assert(runtime.admit_i2c(
               1U, {i2c.resource_id, 1000U, 0U, 32U,
                    std::vector<std::uint8_t>(1U, 0U)}).status ==
           toolbusd::BusAdmissionStatus::ContractMismatch);

    auto accepted = runtime.admit_i2c(
        1U, {i2c.resource_id, 1000U,
             protocol::kI2cTransferRepeatedStart, 1U, {0U}});
    assert(accepted.status == toolbusd::BusAdmissionStatus::Accepted);

    const auto spi = spi_contract(0x0E000001U, 0x0D000000U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice, spi) ==
           toolbusd::BusContractUpdate::Added);
    auto half_duplex_only = spi;
    half_duplex_only.resource_id = 0x0E000002U;
    half_duplex_only.flags = protocol::kBusContractKeepChipSelect;
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice,
               half_duplex_only) == toolbusd::BusContractUpdate::Added);
    assert(runtime.admit_spi(
               1U, {half_duplex_only.resource_id, 1000U, 0U, 1U, 0xFFU,
                    {0x80U}}).status ==
           toolbusd::BusAdmissionStatus::ContractMismatch);
}

void check_contract_load_single_flight_and_raii() {
    toolbusd::BusRuntime runtime;
    constexpr std::uint32_t device = 0x0C000001U;
    auto first = runtime.begin_contract_load(1U, device);
    assert(first);
    auto duplicate = runtime.begin_contract_load(1U, device);
    assert(!duplicate);

    // 不同节点和不同设备互不阻塞。
    auto other_node = runtime.begin_contract_load(2U, device);
    auto other_device = runtime.begin_contract_load(1U, 0x0C000002U);
    assert(other_node);
    assert(other_device);

    // 移动及异常退出后的析构都只释放一次，允许后续重试。
    auto moved = std::move(first);
    assert(!first);
    assert(moved);
    moved = {};
    auto retried = runtime.begin_contract_load(1U, device);
    assert(retried);

    toolbusd::BusRuntime bounded(1U);
    auto only = bounded.begin_contract_load(1U, device);
    assert(only);
    assert(!bounded.begin_contract_load(1U, 0x0C000002U));
}

void check_invalidation_keeps_inflight_bus_reserved() {
    toolbusd::BusRuntime runtime;
    const auto contract = i2c_contract(0x0C000001U, 0x0B000000U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, contract) ==
           toolbusd::BusContractUpdate::Added);
    auto held = runtime.admit_i2c(
        1U, {contract.resource_id, 1000U, 0U, 1U, {0U}});
    assert(held.status == toolbusd::BusAdmissionStatus::Accepted);

    runtime.invalidate_node(1U);
    assert(runtime.contract_count() == 0U);
    assert(runtime.active_bus_count() == 1U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, contract) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.admit_i2c(
               1U, {contract.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::ResourceBusy);

    held.reservation = {};
    assert(runtime.admit_i2c(
               1U, {contract.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::Accepted);
}

}  // namespace

int main() {
    check_contract_cache_bounds();
    check_parent_bus_arbitration_and_isolation();
    check_contract_preflight();
    check_contract_load_single_flight_and_raii();
    check_invalidation_keeps_inflight_bus_reserved();
}
