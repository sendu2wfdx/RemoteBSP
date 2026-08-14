#include "remotebsp/protocol/resource.hpp"

#include <cstddef>
namespace remotebsp::protocol {
namespace {

constexpr std::size_t kDescriptorSize = 17;
constexpr std::size_t kContractSize = 32;
constexpr std::size_t kLeaseRequestSize = 9;
constexpr std::size_t kLeaseTokenRequestSize = 16;
constexpr std::size_t kLeaseInfoSize = 27;

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(
            static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(input[0]) |
        (static_cast<std::uint16_t>(input[1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t read_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[index])
                 << (index * 8U);
    }
    return value;
}

bool valid_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(ResourceType::Gpio) &&
           value <= static_cast<std::uint8_t>(ResourceType::TimedBitstream);
}

bool valid_lease_mode(std::uint8_t value, bool allow_none) {
    const auto minimum =
        allow_none ? ResourceLeaseMode::None
                   : ResourceLeaseMode::SharedRead;
    return value >= static_cast<std::uint8_t>(minimum) &&
           value <= static_cast<std::uint8_t>(
                        ResourceLeaseMode::Exclusive);
}

}

ResourcePayloadException::ResourcePayloadException(
    ResourcePayloadError code, const char* message)
    : std::runtime_error(message), code_(code) {}

ResourcePayloadError ResourcePayloadException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_resource_id(std::uint32_t resource_id) {
    std::vector<std::uint8_t> output;
    append_u32(output, resource_id);
    return output;
}

std::uint32_t decode_resource_id(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 4) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源 ID 载荷长度无效");
    }
    return read_u32(payload.data());
}

std::vector<std::uint8_t> encode_resource_descriptor(
    const ResourceDescriptor& descriptor) {
    if (!valid_type(static_cast<std::uint8_t>(descriptor.type))) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidType,
                                       "资源类型无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kDescriptorSize);
    append_u32(output, descriptor.resource_id);
    output.push_back(static_cast<std::uint8_t>(descriptor.type));
    append_u16(output, descriptor.instance);
    append_u16(output, descriptor.flags);
    append_u32(output, descriptor.rx_capacity);
    append_u32(output, descriptor.tx_capacity);
    return output;
}

ResourceDescriptor decode_resource_descriptor(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kDescriptorSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源描述载荷长度无效");
    }
    if (!valid_type(payload[4])) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidType,
                                       "资源类型无效");
    }
    return {read_u32(payload.data()),
            static_cast<ResourceType>(payload[4]),
            read_u16(payload.data() + 5),
            read_u16(payload.data() + 7),
            read_u32(payload.data() + 9),
            read_u32(payload.data() + 13)};
}

std::vector<std::uint8_t> encode_resource_list(
    const std::vector<ResourceDescriptor>& resources) {
    if (resources.size() > 0xFFFFU) {
        throw ResourcePayloadException(
            ResourcePayloadError::TooManyResources, "资源数量超过协议范围");
    }
    std::vector<std::uint8_t> output;
    output.reserve(2 + resources.size() * kDescriptorSize);
    append_u16(output, static_cast<std::uint16_t>(resources.size()));
    for (const auto& resource : resources) {
        const auto encoded = encode_resource_descriptor(resource);
        output.insert(output.end(), encoded.begin(), encoded.end());
    }
    return output;
}

std::vector<ResourceDescriptor> decode_resource_list(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 2) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源列表载荷长度无效");
    }
    const std::size_t count = read_u16(payload.data());
    if (payload.size() != 2 + count * kDescriptorSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源列表数量与长度不匹配");
    }
    std::vector<ResourceDescriptor> resources;
    resources.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto begin =
            payload.begin() + static_cast<std::ptrdiff_t>(
                                  2 + index * kDescriptorSize);
        const std::vector<std::uint8_t> descriptor_payload(
            begin, begin + static_cast<std::ptrdiff_t>(kDescriptorSize));
        resources.push_back(
            decode_resource_descriptor(descriptor_payload));
    }
    return resources;
}

std::vector<std::uint8_t> encode_resource_status(
    const ResourceStatusPayload& status) {
    if (status.health > ResourceHealth::Disabled) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidHealth,
                                       "资源健康状态无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(25);
    append_u32(output, status.resource_id);
    output.push_back(static_cast<std::uint8_t>(status.health));
    append_u32(output, status.error_flags);
    append_u32(output, status.rx_buffered);
    append_u32(output, status.tx_buffered);
    append_u32(output, status.rx_overruns);
    append_u32(output, status.tx_overruns);
    return output;
}

ResourceStatusPayload decode_resource_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 25) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源状态载荷长度无效");
    }
    if (payload[4] >
        static_cast<std::uint8_t>(ResourceHealth::Disabled)) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidHealth,
                                       "资源健康状态无效");
    }
    return {read_u32(payload.data()),
            static_cast<ResourceHealth>(payload[4]),
            read_u32(payload.data() + 5),
            read_u32(payload.data() + 9),
            read_u32(payload.data() + 13),
            read_u32(payload.data() + 17),
            read_u32(payload.data() + 21)};
}

std::vector<std::uint8_t> encode_resource_contract(
    const ResourceContract& contract) {
    if (contract.version != kResourceContractVersion) {
        throw ResourcePayloadException(
            ResourcePayloadError::InvalidContractVersion,
            "资源能力合同版本无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kContractSize);
    append_u32(output, contract.resource_id);
    append_u16(output, contract.version);
    append_u16(output, contract.access_flags);
    append_u32(output, contract.timing_resolution_ns);
    append_u32(output, contract.worst_case_latency_us);
    append_u32(output, contract.maximum_operations_per_second);
    append_u32(output, contract.queue_capacity);
    append_u32(output, contract.maximum_rx_bits_per_second);
    append_u32(output, contract.maximum_tx_bits_per_second);
    return output;
}

ResourceContract decode_resource_contract(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kContractSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源能力合同载荷长度无效");
    }
    if (read_u16(payload.data() + 4) != kResourceContractVersion) {
        throw ResourcePayloadException(
            ResourcePayloadError::InvalidContractVersion,
            "资源能力合同版本不受支持");
    }
    return {read_u32(payload.data()),
            read_u16(payload.data() + 4),
            read_u16(payload.data() + 6),
            read_u32(payload.data() + 8),
            read_u32(payload.data() + 12),
            read_u32(payload.data() + 16),
            read_u32(payload.data() + 20),
            read_u32(payload.data() + 24),
            read_u32(payload.data() + 28)};
}

std::vector<std::uint8_t> encode_resource_lease_request(
    const ResourceLeaseRequest& request) {
    if (!valid_lease_mode(static_cast<std::uint8_t>(request.mode), false)) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLeaseMode,
                                       "资源租约模式无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kLeaseRequestSize);
    append_u32(output, request.resource_id);
    append_u32(output, request.duration_ms);
    output.push_back(static_cast<std::uint8_t>(request.mode));
    return output;
}

ResourceLeaseRequest decode_resource_lease_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kLeaseRequestSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源租约请求载荷长度无效");
    }
    if (!valid_lease_mode(payload[8], false)) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLeaseMode,
                                       "资源租约请求模式无效");
    }
    return {read_u32(payload.data()),
            read_u32(payload.data() + 4),
            static_cast<ResourceLeaseMode>(payload[8])};
}

std::vector<std::uint8_t> encode_resource_lease_token_request(
    const ResourceLeaseTokenRequest& request) {
    std::vector<std::uint8_t> output;
    output.reserve(kLeaseTokenRequestSize);
    append_u32(output, request.resource_id);
    append_u64(output, request.lease_id);
    append_u32(output, request.duration_ms);
    return output;
}

ResourceLeaseTokenRequest decode_resource_lease_token_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kLeaseTokenRequestSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源租约令牌载荷长度无效");
    }
    return {read_u32(payload.data()),
            read_u64(payload.data() + 4),
            read_u32(payload.data() + 12)};
}

std::vector<std::uint8_t> encode_resource_lease_info(
    const ResourceLeaseInfo& info) {
    if (!valid_lease_mode(static_cast<std::uint8_t>(info.mode), true)) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLeaseMode,
                                       "资源租约状态模式无效");
    }
    std::vector<std::uint8_t> output;
    output.reserve(kLeaseInfoSize);
    append_u32(output, info.resource_id);
    append_u64(output, info.lease_id);
    append_u32(output, info.owner_session_id);
    append_u32(output, info.granted_duration_ms);
    append_u32(output, info.remaining_ms);
    output.push_back(static_cast<std::uint8_t>(info.mode));
    append_u16(output, info.active_lease_count);
    return output;
}

ResourceLeaseInfo decode_resource_lease_info(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != kLeaseInfoSize) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLength,
                                       "资源租约状态载荷长度无效");
    }
    if (!valid_lease_mode(payload[24], true)) {
        throw ResourcePayloadException(ResourcePayloadError::InvalidLeaseMode,
                                       "资源租约状态模式无效");
    }
    return {read_u32(payload.data()),
            read_u64(payload.data() + 4),
            read_u32(payload.data() + 12),
            read_u32(payload.data() + 16),
            read_u32(payload.data() + 20),
            static_cast<ResourceLeaseMode>(payload[24]),
            read_u16(payload.data() + 25)};
}

}
