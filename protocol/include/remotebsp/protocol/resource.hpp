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
    StepgenAxis = 9,
    TimedBitstream = 10,
    // Spi/I2c 保留为 v1 早期的扁平端点，新的静态资源目录应明确区分总线与设备。
    I2cBus = 11,
    I2cDevice = 12,
    SpiBus = 13,
    SpiDevice = 14,
    Stream = 15,
};

constexpr std::uint16_t kResourceFlagNative = 0x0001;
constexpr std::uint16_t kResourceFlagExpanded = 0x0002;
constexpr std::uint16_t kResourceContractVersion = 1;

constexpr std::uint16_t kResourceAccessReadable = 0x0001;
constexpr std::uint16_t kResourceAccessWritable = 0x0002;
constexpr std::uint16_t kResourceAccessSharedRead = 0x0004;
constexpr std::uint16_t kResourceAccessExclusiveWrite = 0x0008;
constexpr std::uint16_t kResourceAccessLeaseSupported = 0x0010;
constexpr std::uint16_t kResourceAccessLeaseRequired = 0x0020;

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

// 资源能力合同描述当前固件与运行时配置可以保证的边界。数值为零表示该指标
// 对此资源不适用或尚未声明，不能解释为无限制。
struct ResourceContract {
    std::uint32_t resource_id{};
    std::uint16_t version{kResourceContractVersion};
    std::uint16_t access_flags{};
    std::uint32_t timing_resolution_ns{};
    std::uint32_t worst_case_latency_us{};
    std::uint32_t maximum_operations_per_second{};
    std::uint32_t queue_capacity{};
    std::uint32_t maximum_rx_bits_per_second{};
    std::uint32_t maximum_tx_bits_per_second{};
};

enum class ResourceLeaseMode : std::uint8_t {
    None = 0,
    SharedRead = 1,
    Exclusive = 2,
};

struct ResourceLeaseRequest {
    std::uint32_t resource_id{};
    std::uint32_t duration_ms{};
    ResourceLeaseMode mode{ResourceLeaseMode::Exclusive};
};

struct ResourceLeaseTokenRequest {
    std::uint32_t resource_id{};
    std::uint64_t lease_id{};
    std::uint32_t duration_ms{};
};

struct ResourceLeaseInfo {
    std::uint32_t resource_id{};
    std::uint64_t lease_id{};
    std::uint32_t owner_session_id{};
    std::uint32_t granted_duration_ms{};
    std::uint32_t remaining_ms{};
    ResourceLeaseMode mode{ResourceLeaseMode::None};
    std::uint16_t active_lease_count{};
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
    InvalidContractVersion,
    InvalidLeaseMode,
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

std::vector<std::uint8_t> encode_resource_contract(
    const ResourceContract& contract);
ResourceContract decode_resource_contract(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_lease_request(
    const ResourceLeaseRequest& request);
ResourceLeaseRequest decode_resource_lease_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_lease_token_request(
    const ResourceLeaseTokenRequest& request);
ResourceLeaseTokenRequest decode_resource_lease_token_request(
    const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> encode_resource_lease_info(
    const ResourceLeaseInfo& info);
ResourceLeaseInfo decode_resource_lease_info(
    const std::vector<std::uint8_t>& payload);

}
