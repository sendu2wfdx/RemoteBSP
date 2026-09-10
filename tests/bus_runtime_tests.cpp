#include "remotebsp/toolbusd/bus_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace remotebsp;

protocol::BusResourceContract i2c_contract(
    std::uint32_t device, std::uint32_t parent,
    std::uint32_t maximum_operations_per_second = 1000U) {
    return {device, protocol::kBusResourceContractVersion,
            protocol::BusResourceKind::I2cDevice,
            static_cast<std::uint8_t>(protocol::kBusContractRepeatedStart |
                                      protocol::kBusContractRecovery),
            parent, 400000U, 32U, 2U, 100U, 10000U,
            maximum_operations_per_second};
}

protocol::BusResourceContract spi_contract(
    std::uint32_t device, std::uint32_t parent,
    std::uint32_t maximum_operations_per_second = 1000U) {
    return {device, protocol::kBusResourceContractVersion,
            protocol::BusResourceKind::SpiDevice,
            static_cast<std::uint8_t>(protocol::kBusContractFullDuplex |
                                      protocol::kBusContractKeepChipSelect),
            parent, 12000000U, 32U, 2U, 100U, 10000U,
            maximum_operations_per_second};
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

void check_device_rate_shaping_and_isolation() {
    std::uint64_t now_us = 1000000U;
    toolbusd::BusRuntime runtime(
        8U, [&now_us] { return now_us; });
    const auto limited =
        i2c_contract(0x0C000001U, 0x0B000000U, 2U);
    const auto peer =
        i2c_contract(0x0C000002U, 0x0B000000U, 2U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, limited) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, peer) ==
           toolbusd::BusContractUpdate::Added);

    auto first = runtime.admit_i2c(
        1U, {limited.resource_id, 1000U, 0U, 1U, {0U}});
    assert(first.status == toolbusd::BusAdmissionStatus::Accepted);
    first.reservation = {};

    // 2 ops/s 被整形成 500 ms 的设备级间隔，准入非阻塞并给出确定重试时间。
    auto throttled = runtime.admit_i2c(
        1U, {limited.resource_id, 1000U, 0U, 1U, {0U}});
    assert(throttled.status == toolbusd::BusAdmissionStatus::RateLimited);
    assert(throttled.retry_after_us == 500000U);

    // 同父总线的另一设备有独立窗口；一个设备被限速不形成队首阻塞。
    auto peer_admission = runtime.admit_i2c(
        1U, {peer.resource_id, 1000U, 0U, 1U, {0U}});
    assert(peer_admission.status == toolbusd::BusAdmissionStatus::Accepted);
    peer_admission.reservation = {};

    now_us += 499999U;
    throttled = runtime.admit_i2c(
        1U, {limited.resource_id, 1000U, 0U, 1U, {0U}});
    assert(throttled.status == toolbusd::BusAdmissionStatus::RateLimited);
    assert(throttled.retry_after_us == 1U);
    ++now_us;
    assert(runtime.admit_i2c(
               1U, {limited.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::Accepted);
    const auto telemetry = runtime.telemetry_snapshot();
    assert(telemetry.version == toolbusd::BusTelemetrySnapshot::kVersion);
    assert(telemetry.admitted_total == 3U);
    assert(telemetry.rate_limited_total == 2U);
    assert(telemetry.busy_total == 0U);
    assert(telemetry.resources.size() == 2U);
    assert(telemetry.resources[0U].resource_id == limited.resource_id);
    assert(telemetry.resources[0U].admitted_total == 2U);
    assert(telemetry.resources[0U].rate_limited_total == 2U);
    assert(telemetry.resources[1U].resource_id == peer.resource_id);
    assert(telemetry.resources[1U].admitted_total == 1U);
}

void check_bus_contention_does_not_consume_device_quota() {
    std::uint64_t now_us = 2000000U;
    toolbusd::BusRuntime runtime(
        8U, [&now_us] { return now_us; });
    const auto holder =
        i2c_contract(0x0C000001U, 0x0B000000U, 1U);
    const auto contender =
        i2c_contract(0x0C000002U, 0x0B000000U, 1U);
    for (const auto& contract : {holder, contender}) {
        assert(runtime.remember_contract(
                   1U, protocol::BusResourceKind::I2cDevice, contract) ==
               toolbusd::BusContractUpdate::Added);
    }
    auto held = runtime.admit_i2c(
        1U, {holder.resource_id, 1000U, 0U, 1U, {0U}});
    assert(held.status == toolbusd::BusAdmissionStatus::Accepted);
    assert(runtime.admit_i2c(
               1U, {contender.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::ResourceBusy);
    held.reservation = {};
    assert(runtime.admit_i2c(
               1U, {contender.resource_id, 1000U, 0U, 1U, {0U}}).status ==
           toolbusd::BusAdmissionStatus::Accepted);
    const auto telemetry = runtime.telemetry_snapshot();
    assert(telemetry.admitted_total == 2U && telemetry.busy_total == 1U);
    assert(telemetry.resources[0U].busy_total == 0U);
    assert(telemetry.resources[1U].busy_total == 1U);
}

void check_spi_rate_shaping_and_contract_refresh() {
    std::uint64_t now_us = 3000000U;
    toolbusd::BusRuntime runtime(
        4U, [&now_us] { return now_us; });
    auto contract = spi_contract(0x0E000001U, 0x0D000000U, 4U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice, contract) ==
           toolbusd::BusContractUpdate::Added);
    auto first = runtime.admit_spi(
        1U, {contract.resource_id, 1000U, 0U, 0U, 0xFFU, {0x55U}});
    assert(first.status == toolbusd::BusAdmissionStatus::Accepted);
    first.reservation = {};
    auto limited = runtime.admit_spi(
        1U, {contract.resource_id, 1000U, 0U, 0U, 0xFFU, {0x55U}});
    assert(limited.status == toolbusd::BusAdmissionStatus::RateLimited);
    assert(limited.retry_after_us == 250000U);

    // 新合同替换旧限额时清空旧窗口，避免旧设备状态跨合同版本泄漏。
    contract.maximum_operations_per_second = 8U;
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::SpiDevice, contract) ==
           toolbusd::BusContractUpdate::Replaced);
    assert(runtime.admit_spi(
               1U, {contract.resource_id, 1000U, 0U, 0U, 0xFFU,
                    {0x55U}}).status ==
           toolbusd::BusAdmissionStatus::Accepted);
}

void check_remote_result_classification_and_isolation() {
    toolbusd::BusRuntime runtime(4U);
    const auto first = i2c_contract(0x0C000001U, 0x0B000000U, 0U);
    const auto peer = i2c_contract(0x0C000002U, 0x0B000000U, 0U);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, first) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.remember_contract(
               1U, protocol::BusResourceKind::I2cDevice, peer) ==
           toolbusd::BusContractUpdate::Added);
    assert(runtime.observe_remote_result(
        1U, first.resource_id, protocol::BusTransactionStatus::Ok));
    assert(runtime.observe_remote_result(
        1U, first.resource_id, protocol::BusTransactionStatus::Nack));
    assert(runtime.observe_remote_result(
        1U, first.resource_id, protocol::BusTransactionStatus::Timeout));
    assert(runtime.observe_remote_result(
        1U, first.resource_id, protocol::BusTransactionStatus::Fault));
    assert(runtime.observe_remote_result(
        1U, peer.resource_id, protocol::BusTransactionStatus::Nack));
    // 未知状态和无合同资源不进入任何已知类别。
    assert(!runtime.observe_remote_result(
        1U, first.resource_id,
        static_cast<protocol::BusTransactionStatus>(0x7FU)));
    assert(!runtime.observe_remote_result(
        2U, first.resource_id, protocol::BusTransactionStatus::Fault));

    const auto telemetry = runtime.telemetry_snapshot();
    assert(telemetry.remote_ok_total == 1U);
    assert(telemetry.remote_nack_total == 2U);
    assert(telemetry.remote_timeout_total == 1U);
    assert(telemetry.remote_fault_total == 1U);
    assert(telemetry.resources[0U].remote_nack_total == 1U);
    assert(telemetry.resources[1U].remote_nack_total == 1U);
    runtime.invalidate_node(1U);
    assert(runtime.telemetry_snapshot().resources.empty());
}

}  // namespace

int main() {
    check_contract_cache_bounds();
    check_parent_bus_arbitration_and_isolation();
    check_contract_preflight();
    check_contract_load_single_flight_and_raii();
    check_invalidation_keeps_inflight_bus_reserved();
    check_device_rate_shaping_and_isolation();
    check_bus_contention_does_not_consume_device_quota();
    check_spi_rate_shaping_and_contract_refresh();
    check_remote_result_classification_and_isolation();
}
