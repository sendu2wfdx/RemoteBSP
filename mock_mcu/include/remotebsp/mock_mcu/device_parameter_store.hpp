#pragma once

#include "remotebsp/device_params/store.h"

#include <array>
#include <cstdint>
#include <vector>

namespace remotebsp::mock_mcu {

class DeviceParameterStore {
public:
    static constexpr std::size_t kPageSize = 2048U;
    static constexpr std::size_t kRegionSize = kPageSize * 2U;

    DeviceParameterStore();

    std::uint32_t generation() const noexcept;
    std::uint16_t stored_count() const noexcept;
    std::uint8_t last_error() const noexcept;
    bool read(std::uint16_t id, rbsp_device_param_record& record) const;
    bool write(std::uint32_t expected_generation, std::uint16_t id,
               const std::vector<std::uint8_t>& value);
    bool reboot();

private:
    static const std::uint8_t* map(void* context, std::uint32_t offset,
                                   std::size_t length);
    static bool erase(void* context, std::uint32_t offset,
                      std::size_t length);
    static bool program(void* context, std::uint32_t offset,
                        const std::uint8_t* data, std::size_t length);

    std::array<std::uint8_t, kRegionSize> flash_{};
    rbsp_device_param_store store_{};
    std::array<std::uint8_t, RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE>
        workspace_{};
};

}
