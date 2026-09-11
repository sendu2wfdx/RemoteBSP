#pragma once

#include "remotebsp/protocol/device_parameter_schema.h"

#include <cstdint>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kDeviceParameterProtocolVersion = 1;
constexpr std::uint32_t kDeviceParameterUnlockConfirmation = 0x50564252U;

enum class DeviceParameterStatusFlag : std::uint16_t {
    MaintenanceUnlocked = 1U << 0U,
    RestartRequired = 1U << 1U,
};

struct DeviceParameterStatus {
    std::uint16_t version{kDeviceParameterProtocolVersion};
    std::uint16_t flags{};
    std::uint32_t generation{};
    std::uint16_t stored_count{};
    std::uint16_t definition_count{};
    std::uint8_t last_error{};
    bool health_available{};
    std::uint8_t health_version{};
    std::uint8_t bad_page_mask{};
    std::uint32_t commit_budget{};
    std::uint32_t write_attempts{};
    std::uint32_t successful_commits{};
    std::uint32_t io_failures{};
};

struct DeviceParameterDescriptor {
    std::uint16_t id{};
    std::uint8_t type{};
    std::uint8_t flags{};
    std::uint16_t minimum_length{};
    std::uint16_t maximum_length{};
};

struct DeviceParameterValue {
    std::uint16_t id{};
    std::uint8_t type{};
    std::uint8_t flags{};
    std::uint32_t generation{};
    std::vector<std::uint8_t> value;
};

struct DeviceParameterUnlockRequest {
    std::uint32_t expected_generation{};
    std::uint32_t confirmation{kDeviceParameterUnlockConfirmation};
};

struct DeviceParameterUnlockResponse {
    std::uint32_t generation{};
    std::uint32_t token{};
};

struct DeviceParameterWriteRequest {
    std::uint32_t expected_generation{};
    std::uint32_t token{};
    std::uint16_t id{};
    std::vector<std::uint8_t> value;
};

std::vector<std::uint8_t> encode_device_parameter_status(
    const DeviceParameterStatus& status);
DeviceParameterStatus decode_device_parameter_status(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_descriptors(
    const std::vector<DeviceParameterDescriptor>& descriptors);
std::vector<DeviceParameterDescriptor> decode_device_parameter_descriptors(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_read_request(
    std::uint16_t id);
std::uint16_t decode_device_parameter_read_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_value(
    const DeviceParameterValue& parameter);
DeviceParameterValue decode_device_parameter_value(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_unlock_request(
    const DeviceParameterUnlockRequest& request);
DeviceParameterUnlockRequest decode_device_parameter_unlock_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_unlock_response(
    const DeviceParameterUnlockResponse& response);
DeviceParameterUnlockResponse decode_device_parameter_unlock_response(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_device_parameter_write_request(
    const DeviceParameterWriteRequest& request);
DeviceParameterWriteRequest decode_device_parameter_write_request(
    const std::vector<std::uint8_t>& payload);

}
