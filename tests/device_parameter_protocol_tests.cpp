#include "remotebsp/protocol/device_parameters.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace remotebsp::protocol;

int main() {
    const DeviceParameterStatus status{
        kDeviceParameterProtocolVersion,
        static_cast<std::uint16_t>(
            DeviceParameterStatusFlag::MaintenanceUnlocked),
        42U, 3U, 22U, 0U};
    const auto decoded_status = decode_device_parameter_status(
        encode_device_parameter_status(status));
    assert(decoded_status.generation == 42U);
    assert(decoded_status.stored_count == 3U);
    assert(!decoded_status.health_available);
    const std::vector<std::uint8_t> legacy_status{
        1U, 0U, 0U, 0U, 42U, 0U, 0U, 0U,
        3U, 0U, 22U, 0U, 0U, 0U, 0U, 0U};
    assert(!decode_device_parameter_status(legacy_status).health_available);
    DeviceParameterStatus observed = status;
    observed.health_available = true;
    observed.health_version = 1U;
    observed.bad_page_mask = 2U;
    observed.commit_budget = 10000U;
    observed.write_attempts = 7U;
    observed.successful_commits = 6U;
    observed.io_failures = 1U;
    const auto decoded_observed = decode_device_parameter_status(
        encode_device_parameter_status(observed));
    assert(decoded_observed.health_available);
    assert(decoded_observed.commit_budget == 10000U);
    assert(decoded_observed.bad_page_mask == 2U);

    const std::vector<DeviceParameterDescriptor> definitions{{
        RBSP_DEVICE_PARAM_SERIAL_NUMBER, RBSP_DEVICE_PARAM_TYPE_UTF8,
        RBSP_DEVICE_PARAM_FLAG_FACTORY, 1U, 32U}};
    const auto decoded_definitions = decode_device_parameter_descriptors(
        encode_device_parameter_descriptors(definitions));
    assert(decoded_definitions.size() == 1U);
    assert(decoded_definitions.front().id == RBSP_DEVICE_PARAM_SERIAL_NUMBER);

    const DeviceParameterValue value{
        RBSP_DEVICE_PARAM_SERIAL_NUMBER, RBSP_DEVICE_PARAM_TYPE_UTF8,
        RBSP_DEVICE_PARAM_FLAG_FACTORY, 7U,
        {'R', 'B', 'S', 'P', '-', '0', '1'}};
    const auto decoded_value = decode_device_parameter_value(
        encode_device_parameter_value(value));
    assert(decoded_value.value == value.value);

    const DeviceParameterWriteRequest write{
        7U, 0x12345678U, RBSP_DEVICE_PARAM_SERIAL_NUMBER,
        {'S', 'N', '-', '2'}};
    const auto decoded_write = decode_device_parameter_write_request(
        encode_device_parameter_write_request(write));
    assert(decoded_write.token == write.token);
    assert(decoded_write.value == write.value);

    bool rejected = false;
    try {
        static_cast<void>(decode_device_parameter_status({1U, 2U}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    std::cout << "设备参数协议测试通过\n";
    return 0;
}
