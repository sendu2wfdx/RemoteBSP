#include "remotebsp/protocol/resource.hpp"

#include <cstddef>
namespace remotebsp::protocol {
namespace {

constexpr std::size_t kDescriptorSize = 17;

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

bool valid_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(ResourceType::Gpio) &&
           value <= static_cast<std::uint8_t>(ResourceType::Storage);
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

}
