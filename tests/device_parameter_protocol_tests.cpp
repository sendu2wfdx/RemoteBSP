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
