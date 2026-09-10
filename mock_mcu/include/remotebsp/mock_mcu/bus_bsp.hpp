#pragma once

#include "remotebsp/protocol/bus_stream.hpp"

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

class BusBsp {
public:
    virtual ~BusBsp() = default;
    virtual protocol::BusTransferResult i2c_transfer(
        const protocol::I2cTransferRequest& request) = 0;
    virtual protocol::BusTransferResult spi_transfer(
        const protocol::SpiTransferRequest& request) = 0;
    virtual const protocol::BusResourceContract* contract(
        std::uint32_t resource_id) const noexcept = 0;
    virtual bool reset(std::uint32_t resource_id) = 0;
    virtual bool failed(std::uint32_t resource_id) const noexcept = 0;
};

enum class MockBusError {
    InvalidContract,
    DuplicateDevice,
    DeviceNotFound,
    WrongDeviceKind,
};

class MockBusException : public std::runtime_error {
public:
    MockBusException(MockBusError code, const char* message);
    MockBusError code() const noexcept;

private:
    MockBusError code_;
};

struct MockBusDeviceSnapshot {
    std::uint32_t resource_id{};
    protocol::BusTransactionStatus next_status{
        protocol::BusTransactionStatus::Ok};
    bool failed{};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> spi_response;
};

// 面向主机测试的确定性后端。每个设备拥有独立故障状态，便于验证故障隔离。
class MockBusBsp final : public BusBsp {
public:
    void add_device(const protocol::BusResourceContract& contract,
                    std::vector<std::uint8_t> initial_data = {});
    void set_next_status(std::uint32_t resource_id,
                         protocol::BusTransactionStatus status);
    void set_spi_response(std::uint32_t resource_id,
                          std::vector<std::uint8_t> data);
    void set_failed(std::uint32_t resource_id, bool failed);
    void set_reset_failure(std::uint32_t resource_id, bool fail);

    protocol::BusTransferResult i2c_transfer(
        const protocol::I2cTransferRequest& request) override;
    protocol::BusTransferResult spi_transfer(
        const protocol::SpiTransferRequest& request) override;
    const protocol::BusResourceContract* contract(
        std::uint32_t resource_id) const noexcept override;
    bool reset(std::uint32_t resource_id) override;
    bool failed(std::uint32_t resource_id) const noexcept override;
    std::vector<MockBusDeviceSnapshot> snapshot() const;

private:
    struct DeviceState {
        protocol::BusResourceContract contract;
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> spi_response;
        protocol::BusTransactionStatus next_status{
            protocol::BusTransactionStatus::Ok};
        bool failed{};
        bool reset_failure{};
    };

    DeviceState& require_device(std::uint32_t resource_id,
                                protocol::BusResourceKind kind);
    protocol::BusTransferResult take_injected_result(DeviceState& device);
    std::unordered_map<std::uint32_t, DeviceState> devices_;
};

}  // namespace remotebsp::mock_mcu
