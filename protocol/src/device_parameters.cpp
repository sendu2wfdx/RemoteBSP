#include "remotebsp/protocol/device_parameters.hpp"

#include <limits>
#include <stdexcept>

namespace remotebsp::protocol {
namespace {

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t read_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

}

std::vector<std::uint8_t> encode_device_parameter_status(
    const DeviceParameterStatus& status) {
    std::vector<std::uint8_t> output;
    output.reserve(16U);
    append_u16(output, status.version);
    append_u16(output, status.flags);
    append_u32(output, status.generation);
    append_u16(output, status.stored_count);
    append_u16(output, status.definition_count);
    output.push_back(status.last_error);
    output.insert(output.end(), 3U, 0U);
    return output;
}

DeviceParameterStatus decode_device_parameter_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 16U) {
        throw std::invalid_argument("设备参数状态长度无效");
    }
    DeviceParameterStatus status;
    status.version = read_u16(payload.data());
    if (status.version != kDeviceParameterProtocolVersion) {
        throw std::invalid_argument("设备参数协议版本不受支持");
    }
    status.flags = read_u16(payload.data() + 2U);
    status.generation = read_u32(payload.data() + 4U);
    status.stored_count = read_u16(payload.data() + 8U);
    status.definition_count = read_u16(payload.data() + 10U);
    status.last_error = payload[12U];
    return status;
}

std::vector<std::uint8_t> encode_device_parameter_descriptors(
    const std::vector<DeviceParameterDescriptor>& descriptors) {
    if (descriptors.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("设备参数定义数量过多");
    }
    std::vector<std::uint8_t> output;
    output.reserve(4U + descriptors.size() * 8U);
    append_u16(output, kDeviceParameterProtocolVersion);
    append_u16(output, static_cast<std::uint16_t>(descriptors.size()));
    for (const auto& descriptor : descriptors) {
        append_u16(output, descriptor.id);
        output.push_back(descriptor.type);
        output.push_back(descriptor.flags);
        append_u16(output, descriptor.minimum_length);
        append_u16(output, descriptor.maximum_length);
    }
    return output;
}

std::vector<DeviceParameterDescriptor> decode_device_parameter_descriptors(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 4U || read_u16(payload.data()) !=
                                   kDeviceParameterProtocolVersion) {
        throw std::invalid_argument("设备参数定义列表头无效");
    }
    const auto count = read_u16(payload.data() + 2U);
    if (payload.size() != 4U + static_cast<std::size_t>(count) * 8U) {
        throw std::invalid_argument("设备参数定义列表长度无效");
    }
    std::vector<DeviceParameterDescriptor> output;
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto* data = payload.data() + 4U + index * 8U;
        output.push_back({read_u16(data), data[2], data[3],
                          read_u16(data + 4U), read_u16(data + 6U)});
    }
    return output;
}

std::vector<std::uint8_t> encode_device_parameter_read_request(
    std::uint16_t id) {
    return {static_cast<std::uint8_t>(id),
            static_cast<std::uint8_t>(id >> 8U)};
}

std::uint16_t decode_device_parameter_read_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 2U) {
        throw std::invalid_argument("设备参数读取请求长度无效");
    }
    return read_u16(payload.data());
}

std::vector<std::uint8_t> encode_device_parameter_value(
    const DeviceParameterValue& parameter) {
    if (parameter.value.size() > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE) {
        throw std::invalid_argument("设备参数值过长");
    }
    std::vector<std::uint8_t> output;
    output.reserve(12U + parameter.value.size());
    append_u16(output, parameter.id);
    output.push_back(parameter.type);
    output.push_back(parameter.flags);
    append_u32(output, parameter.generation);
    append_u16(output, static_cast<std::uint16_t>(parameter.value.size()));
    append_u16(output, 0U);
    output.insert(output.end(), parameter.value.begin(), parameter.value.end());
    return output;
}

DeviceParameterValue decode_device_parameter_value(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 12U) {
        throw std::invalid_argument("设备参数值长度无效");
    }
    const auto length = read_u16(payload.data() + 8U);
    if (length > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE ||
        payload.size() != 12U + length) {
        throw std::invalid_argument("设备参数值范围无效");
    }
    DeviceParameterValue result;
    result.id = read_u16(payload.data());
    result.type = payload[2U];
    result.flags = payload[3U];
    result.generation = read_u32(payload.data() + 4U);
    result.value.assign(payload.begin() + 12U, payload.end());
    return result;
}

std::vector<std::uint8_t> encode_device_parameter_unlock_request(
    const DeviceParameterUnlockRequest& request) {
    std::vector<std::uint8_t> output;
    output.reserve(8U);
    append_u32(output, request.expected_generation);
    append_u32(output, request.confirmation);
    return output;
}

DeviceParameterUnlockRequest decode_device_parameter_unlock_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8U) {
        throw std::invalid_argument("设备参数维护解锁请求长度无效");
    }
    return {read_u32(payload.data()), read_u32(payload.data() + 4U)};
}

std::vector<std::uint8_t> encode_device_parameter_unlock_response(
    const DeviceParameterUnlockResponse& response) {
    std::vector<std::uint8_t> output;
    output.reserve(8U);
    append_u32(output, response.generation);
    append_u32(output, response.token);
    return output;
}

DeviceParameterUnlockResponse decode_device_parameter_unlock_response(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 8U) {
        throw std::invalid_argument("设备参数维护解锁响应长度无效");
    }
    return {read_u32(payload.data()), read_u32(payload.data() + 4U)};
}

std::vector<std::uint8_t> encode_device_parameter_write_request(
    const DeviceParameterWriteRequest& request) {
    if (request.value.size() > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE) {
        throw std::invalid_argument("设备参数写入值过长");
    }
    std::vector<std::uint8_t> output;
    output.reserve(12U + request.value.size());
    append_u32(output, request.expected_generation);
    append_u32(output, request.token);
    append_u16(output, request.id);
    append_u16(output, static_cast<std::uint16_t>(request.value.size()));
    output.insert(output.end(), request.value.begin(), request.value.end());
    return output;
}

DeviceParameterWriteRequest decode_device_parameter_write_request(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 12U) {
        throw std::invalid_argument("设备参数写入请求长度无效");
    }
    const auto length = read_u16(payload.data() + 10U);
    if (length > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE ||
        payload.size() != 12U + length) {
        throw std::invalid_argument("设备参数写入范围无效");
    }
    DeviceParameterWriteRequest result;
    result.expected_generation = read_u32(payload.data());
    result.token = read_u32(payload.data() + 4U);
    result.id = read_u16(payload.data() + 8U);
    result.value.assign(payload.begin() + 12U, payload.end());
    return result;
}

}
