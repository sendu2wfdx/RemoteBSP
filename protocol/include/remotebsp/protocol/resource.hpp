#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

enum class ResourceType : std::uint8_t {
    Gpio = 1,
    Uart = 2,
    Spi = 3,
    I2c = 4,
    Adc = 5,
    Pwm = 6,
    Timer = 7,
    Storage = 8,
};

constexpr std::uint16_t kResourceFlagNative = 0x0001;
constexpr std::uint16_t kResourceFlagExpanded = 0x0002;

struct ResourceDescriptor {
    std::uint32_t resource_id{};
    ResourceType type{ResourceType::Gpio};
    std::uint16_t instance{};
    std::uint16_t flags{};
    std::uint32_t rx_capacity{};
    std::uint32_t tx_capacity{};
};

enum class ResourceHealth : std::uint8_t {
    Normal = 0,
    Busy = 1,
    Degraded = 2,
    Failed = 3,
    Disabled = 4,
};

struct ResourceStatusPayload {
    std::uint32_t resource_id{};
    ResourceHealth health{ResourceHealth::Normal};
    std::uint32_t error_flags{};
    std::uint32_t rx_buffered{};
    std::uint32_t tx_buffered{};
    std::uint32_t rx_overruns{};
    std::uint32_t tx_overruns{};
};

constexpr std::uint32_t kResourceErrorRxOverflow = 0x00000001U;
constexpr std::uint32_t kResourceErrorTxOverflow = 0x00000002U;
constexpr std::uint32_t kResourceErrorBackendFailure = 0x00000004U;
constexpr std::uint32_t kResourceErrorRxHighWater = 0x00000008U;
constexpr std::uint32_t kResourceErrorTxHighWater = 0x00000010U;

enum class ResourcePayloadError {
    InvalidLength,
    InvalidType,
    InvalidHealth,
    TooManyResources,
};

class ResourcePayloadException : public std::runtime_error {
public:
    ResourcePayloadException(ResourcePayloadError code,
                             const char* message);
    ResourcePayloadError code() const noexcept;

private:
    ResourcePayloadError code_;
};

std::vector<std::uint8_t> encode_resource_id(std::uint32_t resource_id);
std::uint32_t decode_resource_id(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_descriptor(
    const ResourceDescriptor& descriptor);
ResourceDescriptor decode_resource_descriptor(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_list(
    const std::vector<ResourceDescriptor>& resources);
std::vector<ResourceDescriptor> decode_resource_list(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_status(
    const ResourceStatusPayload& status);
ResourceStatusPayload decode_resource_status(
    const std::vector<std::uint8_t>& payload);

}
