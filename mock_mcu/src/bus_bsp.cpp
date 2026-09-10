#include "remotebsp/mock_mcu/bus_bsp.hpp"

#include <algorithm>
#include <cstddef>

namespace remotebsp::mock_mcu {

MockBusException::MockBusException(MockBusError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MockBusError MockBusException::code() const noexcept { return code_; }

void MockBusBsp::add_device(
    const protocol::BusResourceContract& contract,
    std::vector<std::uint8_t> initial_data) {
    if (contract.resource_id == 0 || contract.parent_bus_resource_id == 0 ||
        (contract.kind != protocol::BusResourceKind::I2cDevice &&
         contract.kind != protocol::BusResourceKind::SpiDevice)) {
        throw MockBusException(MockBusError::InvalidContract,
                               "Mock 总线设备合同无效");
    }
    // 借用协议编码器执行全部边界验证，避免 Mock 与线协议的限制漂移。
    static_cast<void>(protocol::encode_bus_resource_contract(contract));
    if (initial_data.size() > contract.maximum_transfer_bytes) {
        throw MockBusException(MockBusError::InvalidContract,
                               "Mock 总线初始数据超过设备合同");
    }
    const auto inserted = devices_.emplace(
        contract.resource_id,
        DeviceState{contract, std::move(initial_data), {},
                    protocol::BusTransactionStatus::Ok, false, false});
    if (!inserted.second) {
        throw MockBusException(MockBusError::DuplicateDevice,
                               "Mock 总线设备 ID 重复");
    }
}

void MockBusBsp::set_next_status(
    std::uint32_t resource_id, protocol::BusTransactionStatus status) {
    auto found = devices_.find(resource_id);
    if (found == devices_.end()) {
        throw MockBusException(MockBusError::DeviceNotFound,
                               "Mock 总线设备不存在");
    }
    found->second.next_status = status;
}

void MockBusBsp::set_spi_response(std::uint32_t resource_id,
                                  std::vector<std::uint8_t> data) {
    auto& device =
        require_device(resource_id, protocol::BusResourceKind::SpiDevice);
    if (data.size() > device.contract.maximum_transfer_bytes) {
        throw MockBusException(MockBusError::InvalidContract,
                               "Mock SPI 确定性响应超过设备合同");
    }
    device.spi_response = std::move(data);
}

void MockBusBsp::set_failed(std::uint32_t resource_id, bool failed) {
    auto found = devices_.find(resource_id);
    if (found == devices_.end())
        throw MockBusException(MockBusError::DeviceNotFound,
                               "Mock 总线设备不存在");
    found->second.failed = failed;
}

void MockBusBsp::set_reset_failure(std::uint32_t resource_id, bool fail) {
    auto found = devices_.find(resource_id);
    if (found == devices_.end())
        throw MockBusException(MockBusError::DeviceNotFound,
                               "Mock 总线设备不存在");
    found->second.reset_failure = fail;
}

MockBusBsp::DeviceState& MockBusBsp::require_device(
    std::uint32_t resource_id, protocol::BusResourceKind kind) {
    auto found = devices_.find(resource_id);
    if (found == devices_.end()) {
        throw MockBusException(MockBusError::DeviceNotFound,
                               "Mock 总线设备不存在");
    }
    if (found->second.contract.kind != kind) {
        throw MockBusException(MockBusError::WrongDeviceKind,
                               "总线事务与设备类型不匹配");
    }
    return found->second;
}

protocol::BusTransferResult MockBusBsp::take_injected_result(
    DeviceState& device) {
    const auto status = device.next_status;
    device.next_status = protocol::BusTransactionStatus::Ok;
    if (status == protocol::BusTransactionStatus::Fault) device.failed = true;
    return {status, 0, 0, {}};
}

protocol::BusTransferResult MockBusBsp::i2c_transfer(
    const protocol::I2cTransferRequest& request) {
    auto& device = require_device(request.device_resource_id,
                                  protocol::BusResourceKind::I2cDevice);
    if (device.next_status != protocol::BusTransactionStatus::Ok) {
        return take_injected_result(device);
    }
    const auto& contract = device.contract;
    const auto total = request.write_data.size() + request.read_length;
    if (total > contract.maximum_transfer_bytes) {
        return {protocol::BusTransactionStatus::LimitExceeded, 0, 0, {}};
    }
    if (request.timeout_us < contract.minimum_timeout_us ||
        request.timeout_us > contract.maximum_timeout_us) {
        return {protocol::BusTransactionStatus::Timeout, 0, 0, {}};
    }

    std::size_t offset = 0;
    if (!request.write_data.empty()) {
        offset = request.write_data.front();
        if (request.write_data.size() > 1) {
            const auto bytes = request.write_data.size() - 1;
            if (device.data.size() < offset + bytes) {
                device.data.resize(offset + bytes, 0);
            }
            std::copy(request.write_data.begin() + 1,
                      request.write_data.end(),
                      device.data.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    std::vector<std::uint8_t> received(request.read_length, 0);
    for (std::size_t index = 0; index < received.size(); ++index) {
        if (offset + index < device.data.size()) {
            received[index] = device.data[offset + index];
        }
    }
    return {protocol::BusTransactionStatus::Ok,
            static_cast<std::uint16_t>(request.write_data.size()),
            request.read_length, std::move(received)};
}

protocol::BusTransferResult MockBusBsp::spi_transfer(
    const protocol::SpiTransferRequest& request) {
    auto& device = require_device(request.device_resource_id,
                                  protocol::BusResourceKind::SpiDevice);
    if (device.next_status != protocol::BusTransactionStatus::Ok) {
        return take_injected_result(device);
    }
    const auto& contract = device.contract;
    const auto total = std::max(request.transmit_data.size(),
                                static_cast<std::size_t>(
                                    request.receive_length));
    if (total > contract.maximum_transfer_bytes) {
        return {protocol::BusTransactionStatus::LimitExceeded, 0, 0, {}};
    }
    if (request.timeout_us < contract.minimum_timeout_us ||
        request.timeout_us > contract.maximum_timeout_us) {
        return {protocol::BusTransactionStatus::Timeout, 0, 0, {}};
    }
    std::vector<std::uint8_t> received(request.receive_length,
                                       request.dummy_byte);
    const auto count = std::min(received.size(), device.spi_response.size());
    std::copy_n(device.spi_response.begin(), count, received.begin());
    return {protocol::BusTransactionStatus::Ok,
            static_cast<std::uint16_t>(request.transmit_data.size()),
            request.receive_length, std::move(received)};
}

const protocol::BusResourceContract* MockBusBsp::contract(
    std::uint32_t resource_id) const noexcept {
    const auto found = devices_.find(resource_id);
    return found == devices_.end() ? nullptr : &found->second.contract;
}

bool MockBusBsp::reset(std::uint32_t resource_id) {
    auto found = devices_.find(resource_id);
    if (found == devices_.end())
        throw MockBusException(MockBusError::DeviceNotFound,
                               "Mock 总线设备不存在");
    if (found->second.reset_failure) return false;
    found->second.failed = false;
    found->second.next_status = protocol::BusTransactionStatus::Ok;
    return true;
}

bool MockBusBsp::failed(std::uint32_t resource_id) const noexcept {
    const auto found = devices_.find(resource_id);
    return found != devices_.end() && found->second.failed;
}

std::vector<MockBusDeviceSnapshot> MockBusBsp::snapshot() const {
    std::vector<MockBusDeviceSnapshot> result;
    result.reserve(devices_.size());
    for (const auto& entry : devices_) {
        result.push_back({entry.first, entry.second.next_status,
                          entry.second.failed,
                          entry.second.data, entry.second.spi_response});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return left.resource_id < right.resource_id;
              });
    return result;
}

}  // namespace remotebsp::mock_mcu
