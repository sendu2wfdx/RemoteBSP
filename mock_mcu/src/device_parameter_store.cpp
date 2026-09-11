#include "remotebsp/mock_mcu/device_parameter_store.hpp"

#include <algorithm>
#include <stdexcept>

namespace remotebsp::mock_mcu {

DeviceParameterStore::DeviceParameterStore() {
    flash_.fill(0xFFU);
    const rbsp_device_param_backend backend{
        this, static_cast<std::uint32_t>(flash_.size()),
        static_cast<std::uint32_t>(kPageSize), 8U,
        map, erase, program,
        RBSP_DEVICE_PARAM_BACKEND_CONTRACT_VERSION,
        RBSP_DEVICE_PARAM_MEDIUM_MOCK,
        RBSP_DEVICE_PARAM_BACKEND_ERASED_VALUE,
        RBSP_DEVICE_PARAM_BACKEND_FLAG_ERASE_BEFORE_PROGRAM |
            RBSP_DEVICE_PARAM_BACKEND_FLAG_ONE_TO_ZERO_ONLY |
            RBSP_DEVICE_PARAM_BACKEND_FLAG_COMMIT_MARKER_LAST};
    if (!rbsp_device_param_store_init(&store_, &backend) ||
        !rbsp_device_param_store_boot(&store_)) {
        throw std::runtime_error("无法初始化Mock设备参数存储");
    }
}

std::uint32_t DeviceParameterStore::generation() const noexcept {
    return store_.generation;
}

std::uint16_t DeviceParameterStore::stored_count() const noexcept {
    return store_.record_count;
}

std::uint8_t DeviceParameterStore::last_error() const noexcept {
    return static_cast<std::uint8_t>(store_.last_error);
}

bool DeviceParameterStore::read(
    std::uint16_t id, rbsp_device_param_record& record) const {
    return rbsp_device_param_store_get(&store_, id, &record);
}

bool DeviceParameterStore::write(
    std::uint32_t expected_generation, std::uint16_t id,
    const std::vector<std::uint8_t>& value) {
    return rbsp_device_param_store_set(
        &store_, expected_generation, id,
        value.empty() ? nullptr : value.data(), value.size(),
        workspace_.data(), workspace_.size());
}

bool DeviceParameterStore::reboot() {
    return rbsp_device_param_store_boot(&store_);
}

const std::uint8_t* DeviceParameterStore::map(
    void* context, std::uint32_t offset, std::size_t length) {
    const auto* self = static_cast<const DeviceParameterStore*>(context);
    if (offset > self->flash_.size() ||
        length > self->flash_.size() - offset) {
        return nullptr;
    }
    return self->flash_.data() + offset;
}

bool DeviceParameterStore::erase(
    void* context, std::uint32_t offset, std::size_t length) {
    auto* self = static_cast<DeviceParameterStore*>(context);
    if (offset > self->flash_.size() || length != kPageSize ||
        offset % kPageSize != 0U ||
        length > self->flash_.size() - offset) {
        return false;
    }
    std::fill(self->flash_.begin() + offset,
              self->flash_.begin() + offset + length, 0xFFU);
    return true;
}

bool DeviceParameterStore::program(
    void* context, std::uint32_t offset, const std::uint8_t* data,
    std::size_t length) {
    auto* self = static_cast<DeviceParameterStore*>(context);
    if (data == nullptr || offset > self->flash_.size() ||
        offset % 8U != 0U || length == 0U || length % 8U != 0U ||
        length > self->flash_.size() - offset) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        if ((self->flash_[offset + index] & data[index]) != data[index]) {
            return false;
        }
    }
    for (std::size_t index = 0; index < length; ++index) {
        self->flash_[offset + index] &= data[index];
    }
    return true;
}

}
