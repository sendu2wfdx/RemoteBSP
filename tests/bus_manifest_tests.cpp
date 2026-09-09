#include "remotebsp/mock_mcu/board_manifest.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace remotebsp;

std::string read_text(const char* path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string replace_once(std::string text, const std::string& before,
                         const std::string& after) {
    const auto position = text.find(before);
    assert(position != std::string::npos);
    text.replace(position, before.size(), after);
    return text;
}

void expect_manifest_error(const std::string& text,
                           mock_mcu::ManifestError expected) {
    try {
        static_cast<void>(mock_mcu::parse_board_manifest(text));
        assert(false);
    } catch (const mock_mcu::ManifestException& error) {
        assert(error.code() == expected);
    }
}

protocol::Packet request(protocol::Command command,
                         std::vector<std::uint8_t> payload) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(command);
    packet.header.session_id = 1;
    packet.header.request_id = 1;
    packet.payload = std::move(payload);
    return packet;
}

std::vector<std::uint8_t> body(const protocol::Packet& response) {
    assert(!response.payload.empty());
    assert(response.payload[0] ==
           static_cast<std::uint8_t>(mock_mcu::StatusCode::Ok));
    return {response.payload.begin() + 1, response.payload.end()};
}

void test_v2_manifest_instantiates_bus_backend() {
    auto manifest = mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    assert(manifest.schema_version == 2);
    assert(manifest.bus_resources.size() == 4);
    assert(manifest.bus_resources[1].i2c_address == 72);
    assert(manifest.bus_resources[3].spi_mode == 3);
    assert(manifest.bus_resources[3].bits_per_word == 8);
    assert(manifest.bus_resources[3].spi_chip_select == 5);

    auto scenario = mock_mcu::load_fault_scenario(TEST_BUS_FAULT_SCENARIO);
    mock_mcu::DigitalTwin twin(std::move(manifest), std::move(scenario));
    assert(twin.bus() != nullptr);
    auto core = mock_mcu::make_remote_core(twin, 1);

    const auto i2c = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::I2cTransfer,
                protocol::encode_i2c_transfer_request(
                    {201326593, 1000, protocol::kI2cTransferRepeatedStart,
                     3, {2}})))));
    assert(i2c.status == protocol::BusTransactionStatus::Ok);
    assert(i2c.data == std::vector<std::uint8_t>({2, 3, 4}));

    assert(twin.advance_to(10) == 1);
    const auto nack = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::I2cTransfer,
                protocol::encode_i2c_transfer_request(
                    {201326593, 1000, 0, 1, {0}})))));
    assert(nack.status == protocol::BusTransactionStatus::Nack);

    // 单个 I2C 设备的故障不影响同节点 SPI 设备的确定性响应。
    const auto spi = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::SpiTransfer,
                protocol::encode_spi_transfer_request(
                    {234881025, 1000, 0, 3, 0xFF, {0x80}})))));
    assert(spi.status == protocol::BusTransactionStatus::Ok);
    assert(spi.data == std::vector<std::uint8_t>({0xAA, 0x55, 0x11}));

    assert(twin.advance_to(20) == 1);
    const auto timeout = protocol::decode_bus_transfer_result(body(core.handle(
        request(protocol::Command::SpiTransfer,
                protocol::encode_spi_transfer_request(
                    {234881025, 1000, 0, 1, 0xFF, {0x80}})))));
    assert(timeout.status == protocol::BusTransactionStatus::Timeout);
}

void test_manifest_rejects_invalid_bus_graphs() {
    const auto valid = read_text(TEST_BUS_MANIFEST);

    const auto internal_spi = replace_once(
        valid, "\"reserved_resources\": []",
        "\"reserved_resources\": [{\"type\":\"spi\",\"instance\":0,"
        "\"owner\":\"internal-adapter\"}]");
    expect_manifest_error(internal_spi, mock_mcu::ManifestError::Conflict);

    const auto missing_parent = replace_once(
        valid, "\"parent_bus_resource_id\": 184549376",
        "\"parent_bus_resource_id\": 999");
    expect_manifest_error(missing_parent, mock_mcu::ManifestError::Conflict);

    const auto wrong_type = replace_once(
        valid, "\"type\": \"spi_device\", \"first_instance\": 1",
        "\"type\": \"i2c_device\", \"first_instance\": 2");
    expect_manifest_error(wrong_type, mock_mcu::ManifestError::Conflict);

    const auto oversized = replace_once(
        valid, "\"deterministic_response\": [170, 85, 17]",
        "\"deterministic_response\": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]");
    expect_manifest_error(oversized, mock_mcu::ManifestError::InvalidValue);

    const auto child_too_fast = replace_once(
        valid,
        "\"parent_bus_resource_id\": 184549376, \"maximum_clock_hz\": 400000",
        "\"parent_bus_resource_id\": 184549376, \"maximum_clock_hz\": 800000");
    expect_manifest_error(child_too_fast,
                          mock_mcu::ManifestError::InvalidValue);
}

}  // namespace

int main() {
    test_v2_manifest_instantiates_bus_backend();
    test_manifest_rejects_invalid_bus_graphs();
}
