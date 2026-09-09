#pragma once

#include "remotebsp/protocol/bus_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace remotebsp::toolbusd {

enum class BusContractUpdate : std::uint8_t {
    Added = 0,
    Unchanged,
    Replaced,
    CapacityReached,
    Invalid,
};

enum class BusAdmissionStatus : std::uint8_t {
    Accepted = 0,
    ContractMissing,
    ResourceBusy,
    ContractMismatch,
};

/*
 * toolbusd 只缓存 MCU 已公布的设备合同，并按 (node_id, parent_bus_id)
 * 建立仲裁域。它不解释传感器或厂商寄存器，也不接触 SocketCAN。
 */
class BusRuntime {
public:
    class ContractLoadReservation {
    public:
        ContractLoadReservation() = default;
        ~ContractLoadReservation();
        ContractLoadReservation(const ContractLoadReservation&) = delete;
        ContractLoadReservation& operator=(const ContractLoadReservation&) =
            delete;
        ContractLoadReservation(ContractLoadReservation&& other) noexcept;
        ContractLoadReservation& operator=(
            ContractLoadReservation&& other) noexcept;

        explicit operator bool() const noexcept;

    private:
        friend class BusRuntime;
        ContractLoadReservation(BusRuntime* owner, std::uint32_t node_id,
                                std::uint32_t resource_id) noexcept;
        void release() noexcept;

        BusRuntime* owner_{};
        std::uint32_t node_id_{};
        std::uint32_t resource_id_{};
    };

    class Reservation {
    public:
        Reservation() = default;
        ~Reservation();
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;

        explicit operator bool() const noexcept;

    private:
        friend class BusRuntime;
        Reservation(BusRuntime* owner, std::uint64_t bus_key) noexcept;
        void release() noexcept;

        BusRuntime* owner_{};
        std::uint64_t bus_key_{};
    };

    struct Admission {
        BusAdmissionStatus status{BusAdmissionStatus::ContractMissing};
        Reservation reservation;
    };

    explicit BusRuntime(std::size_t maximum_contracts = 4096U);

    BusContractUpdate remember_contract(
        std::uint32_t node_id,
        protocol::BusResourceKind expected_kind,
        const protocol::BusResourceContract& contract);
    std::optional<protocol::BusResourceContract> find_contract(
        std::uint32_t node_id, std::uint32_t device_resource_id) const;
    ContractLoadReservation begin_contract_load(
        std::uint32_t node_id, std::uint32_t device_resource_id);
    void invalidate_node(std::uint32_t node_id) noexcept;

    Admission admit_i2c(std::uint32_t node_id,
                        const protocol::I2cTransferRequest& request);
    Admission admit_spi(std::uint32_t node_id,
                        const protocol::SpiTransferRequest& request);

    std::size_t contract_count() const noexcept;
    std::size_t active_bus_count() const noexcept;

private:
    struct DeviceKey {
        std::uint32_t node_id{};
        std::uint32_t resource_id{};

        bool operator==(const DeviceKey& other) const noexcept {
            return node_id == other.node_id &&
                   resource_id == other.resource_id;
        }
    };

    struct DeviceKeyHash {
        std::size_t operator()(const DeviceKey& key) const noexcept;
    };

    Admission admit(std::uint32_t node_id, std::uint32_t resource_id,
                    protocol::BusResourceKind expected_kind,
                    std::uint32_t timeout_us, std::size_t transfer_bytes,
                    std::uint8_t required_contract_flags);
    void release(std::uint64_t bus_key) noexcept;
    void finish_contract_load(std::uint32_t node_id,
                              std::uint32_t resource_id) noexcept;
    static std::uint64_t make_bus_key(std::uint32_t node_id,
                                      std::uint32_t parent_bus_id) noexcept;

    const std::size_t maximum_contracts_;
    mutable std::mutex mutex_;
    std::unordered_map<DeviceKey, protocol::BusResourceContract,
                       DeviceKeyHash>
        contracts_;
    std::unordered_set<std::uint64_t> active_buses_;
    std::unordered_set<DeviceKey, DeviceKeyHash> loading_contracts_;
};

}  // namespace remotebsp::toolbusd
