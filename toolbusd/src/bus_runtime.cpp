#include "remotebsp/toolbusd/bus_runtime.hpp"

#include "remotebsp/protocol/resource.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace remotebsp::toolbusd {
namespace {

bool same_contract(const protocol::BusResourceContract& left,
                   const protocol::BusResourceContract& right) noexcept {
    return left.resource_id == right.resource_id &&
           left.version == right.version && left.kind == right.kind &&
           left.flags == right.flags &&
           left.parent_bus_resource_id == right.parent_bus_resource_id &&
           left.maximum_clock_hz == right.maximum_clock_hz &&
           left.maximum_transfer_bytes == right.maximum_transfer_bytes &&
           left.queue_capacity == right.queue_capacity &&
           left.minimum_timeout_us == right.minimum_timeout_us &&
           left.maximum_timeout_us == right.maximum_timeout_us &&
           left.maximum_operations_per_second ==
               right.maximum_operations_per_second;
}

bool resource_namespace_matches(
    protocol::BusResourceKind kind, std::uint32_t resource_id,
    std::uint32_t parent_bus_resource_id) noexcept {
    const auto resource_type = static_cast<std::uint8_t>(resource_id >> 24U);
    const auto parent_type =
        static_cast<std::uint8_t>(parent_bus_resource_id >> 24U);
    if (kind == protocol::BusResourceKind::I2cDevice) {
        return resource_type == static_cast<std::uint8_t>(
                                    protocol::ResourceType::I2cDevice) &&
               parent_type == static_cast<std::uint8_t>(
                                  protocol::ResourceType::I2cBus);
    }
    if (kind == protocol::BusResourceKind::SpiDevice) {
        return resource_type == static_cast<std::uint8_t>(
                                    protocol::ResourceType::SpiDevice) &&
               parent_type == static_cast<std::uint8_t>(
                                  protocol::ResourceType::SpiBus);
    }
    return false;
}

}  // namespace

BusRuntime::ContractLoadReservation::ContractLoadReservation(
    BusRuntime* owner, std::uint32_t node_id,
    std::uint32_t resource_id) noexcept
    : owner_(owner), node_id_(node_id), resource_id_(resource_id) {}

BusRuntime::ContractLoadReservation::~ContractLoadReservation() {
    release();
}

BusRuntime::ContractLoadReservation::ContractLoadReservation(
    ContractLoadReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      node_id_(std::exchange(other.node_id_, 0U)),
      resource_id_(std::exchange(other.resource_id_, 0U)) {}

BusRuntime::ContractLoadReservation&
BusRuntime::ContractLoadReservation::operator=(
    ContractLoadReservation&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        node_id_ = std::exchange(other.node_id_, 0U);
        resource_id_ = std::exchange(other.resource_id_, 0U);
    }
    return *this;
}

BusRuntime::ContractLoadReservation::operator bool() const noexcept {
    return owner_ != nullptr;
}

void BusRuntime::ContractLoadReservation::release() noexcept {
    if (owner_ != nullptr) {
        owner_->finish_contract_load(node_id_, resource_id_);
        owner_ = nullptr;
        node_id_ = 0U;
        resource_id_ = 0U;
    }
}

BusRuntime::Reservation::Reservation(BusRuntime* owner,
                                     std::uint64_t bus_key) noexcept
    : owner_(owner), bus_key_(bus_key) {}

BusRuntime::Reservation::~Reservation() { release(); }

BusRuntime::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      bus_key_(std::exchange(other.bus_key_, 0U)) {}

BusRuntime::Reservation& BusRuntime::Reservation::operator=(
    Reservation&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        bus_key_ = std::exchange(other.bus_key_, 0U);
    }
    return *this;
}

BusRuntime::Reservation::operator bool() const noexcept {
    return owner_ != nullptr;
}

void BusRuntime::Reservation::release() noexcept {
    if (owner_ != nullptr) {
        owner_->release(bus_key_);
        owner_ = nullptr;
        bus_key_ = 0U;
    }
}

BusRuntime::BusRuntime(std::size_t maximum_contracts)
    : maximum_contracts_(maximum_contracts) {
    if (maximum_contracts_ == 0U) {
        throw std::invalid_argument("总线合同缓存容量必须大于零");
    }
}

std::size_t BusRuntime::DeviceKeyHash::operator()(
    const DeviceKey& key) const noexcept {
    const auto combined = (static_cast<std::uint64_t>(key.node_id) << 32U) |
                          key.resource_id;
    return std::hash<std::uint64_t>{}(combined);
}

BusContractUpdate BusRuntime::remember_contract(
    std::uint32_t node_id, protocol::BusResourceKind expected_kind,
    const protocol::BusResourceContract& contract) {
    if (node_id == 0U || contract.resource_id == 0U ||
        contract.parent_bus_resource_id == 0U ||
        contract.kind != expected_kind ||
        (expected_kind != protocol::BusResourceKind::I2cDevice &&
         expected_kind != protocol::BusResourceKind::SpiDevice) ||
        !resource_namespace_matches(contract.kind, contract.resource_id,
                                    contract.parent_bus_resource_id)) {
        return BusContractUpdate::Invalid;
    }
    // 复用协议编码器执行版本、标志与所有数值边界校验。
    try {
        static_cast<void>(protocol::encode_bus_resource_contract(contract));
    } catch (const protocol::BusStreamPayloadException&) {
        return BusContractUpdate::Invalid;
    }

    const DeviceKey key{node_id, contract.resource_id};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contracts_.find(key);
    if (found == contracts_.end()) {
        if (contracts_.size() >= maximum_contracts_) {
            return BusContractUpdate::CapacityReached;
        }
        contracts_.emplace(key, contract);
        return BusContractUpdate::Added;
    }
    if (same_contract(found->second, contract)) {
        return BusContractUpdate::Unchanged;
    }
    const auto old_bus_key = make_bus_key(
        node_id, found->second.parent_bus_resource_id);
    if (active_buses_.find(old_bus_key) != active_buses_.end()) {
        return BusContractUpdate::Invalid;
    }
    found->second = contract;
    return BusContractUpdate::Replaced;
}

std::optional<protocol::BusResourceContract> BusRuntime::find_contract(
    std::uint32_t node_id, std::uint32_t device_resource_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contracts_.find({node_id, device_resource_id});
    return found == contracts_.end()
               ? std::nullopt
               : std::optional<protocol::BusResourceContract>(found->second);
}

BusRuntime::ContractLoadReservation BusRuntime::begin_contract_load(
    std::uint32_t node_id, std::uint32_t device_resource_id) {
    if (node_id == 0U || device_resource_id == 0U) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const DeviceKey key{node_id, device_resource_id};
    if (loading_contracts_.size() >= maximum_contracts_) {
        return {};
    }
    if (!loading_contracts_.insert(key).second) {
        return {};
    }
    return ContractLoadReservation(this, node_id, device_resource_id);
}

void BusRuntime::invalidate_node(std::uint32_t node_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = contracts_.begin(); entry != contracts_.end();) {
        if (entry->first.node_id == node_id) {
            entry = contracts_.erase(entry);
        } else {
            ++entry;
        }
    }
    // 活动请求仍由其 Reservation 释放。节点不可用后不会接纳新请求；
    // 这里不提前释放，避免旧请求与节点快速重连后的新请求交叉。
}

BusRuntime::Admission BusRuntime::admit_i2c(
    std::uint32_t node_id, const protocol::I2cTransferRequest& request) {
    std::uint8_t required_flags = 0U;
    if ((request.flags & protocol::kI2cTransferRepeatedStart) != 0U) {
        required_flags |= protocol::kBusContractRepeatedStart;
    }
    if ((request.flags & protocol::kI2cTransferAllowRecovery) != 0U) {
        required_flags |= protocol::kBusContractRecovery;
    }
    return admit(node_id, request.device_resource_id,
                 protocol::BusResourceKind::I2cDevice, request.timeout_us,
                 request.write_data.size() + request.read_length,
                 required_flags);
}

BusRuntime::Admission BusRuntime::admit_spi(
    std::uint32_t node_id, const protocol::SpiTransferRequest& request) {
    auto required_flags =
        (request.flags & protocol::kSpiTransferKeepChipSelect) != 0U
            ? protocol::kBusContractKeepChipSelect
            : 0U;
    if (!request.transmit_data.empty() && request.receive_length != 0U) {
        required_flags |= protocol::kBusContractFullDuplex;
    }
    return admit(node_id, request.device_resource_id,
                 protocol::BusResourceKind::SpiDevice, request.timeout_us,
                 std::max<std::size_t>(request.transmit_data.size(),
                                       request.receive_length),
                 required_flags);
}

BusRuntime::Admission BusRuntime::admit(
    std::uint32_t node_id, std::uint32_t resource_id,
    protocol::BusResourceKind expected_kind, std::uint32_t timeout_us,
    std::size_t transfer_bytes, std::uint8_t required_contract_flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = contracts_.find({node_id, resource_id});
    if (found == contracts_.end()) {
        return {BusAdmissionStatus::ContractMissing, {}};
    }
    const auto& contract = found->second;
    if (contract.kind != expected_kind || timeout_us < contract.minimum_timeout_us ||
        timeout_us > contract.maximum_timeout_us ||
        transfer_bytes > contract.maximum_transfer_bytes ||
        (required_contract_flags &
         static_cast<std::uint8_t>(~contract.flags)) != 0U) {
        return {BusAdmissionStatus::ContractMismatch, {}};
    }
    const auto bus_key = make_bus_key(node_id,
                                      contract.parent_bus_resource_id);
    if (!active_buses_.insert(bus_key).second) {
        return {BusAdmissionStatus::ResourceBusy, {}};
    }
    return {BusAdmissionStatus::Accepted,
            Reservation(this, bus_key)};
}

void BusRuntime::release(std::uint64_t bus_key) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    active_buses_.erase(bus_key);
}

void BusRuntime::finish_contract_load(std::uint32_t node_id,
                                      std::uint32_t resource_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    loading_contracts_.erase({node_id, resource_id});
}

std::uint64_t BusRuntime::make_bus_key(std::uint32_t node_id,
                                      std::uint32_t parent_bus_id) noexcept {
    return (static_cast<std::uint64_t>(node_id) << 32U) | parent_bus_id;
}

std::size_t BusRuntime::contract_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return contracts_.size();
}

std::size_t BusRuntime::active_bus_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_buses_.size();
}

}  // namespace remotebsp::toolbusd
