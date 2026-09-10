#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/firmware_identity.hpp"
#include "remotebsp/cli_json.hpp"

#include <cassert>
#include <cstdint>
#include <vector>
#include <sstream>

using namespace remotebsp;

namespace {

protocol::Packet request(std::vector<std::uint8_t> payload = {},
                         std::uint32_t object_id = 0U) {
    protocol::Packet value;
    value.header.message_type = protocol::MessageType::Request;
    value.header.command =
        static_cast<std::uint16_t>(protocol::Command::FirmwareIdentity);
    value.header.session_id = 3U;
    value.header.request_id = 4U;
    value.header.object_id = object_id;
    value.payload = std::move(payload);
    return value;
}

void protocol_round_trip_and_validation() {
    protocol::FirmwareIdentityPayload identity;
    identity.board_type = 0x010431U;
    identity.node_uuid.fill(0x42U);
    identity.available_fields =
        static_cast<std::uint16_t>(
            protocol::FirmwareIdentityField::ProjectSha256) |
        static_cast<std::uint16_t>(
            protocol::FirmwareIdentityField::FirmwareInputSha256);
    identity.project_sha256.fill(0x11U);
    identity.firmware_input_sha256.fill(0x33U);
    const auto decoded = protocol::decode_firmware_identity(
        protocol::encode_firmware_identity(identity));
    assert(decoded.board_type == identity.board_type);
    assert(decoded.node_uuid == identity.node_uuid);
    assert(decoded.project_sha256 == identity.project_sha256);
    assert(!decoded.available(
        protocol::FirmwareIdentityField::ConfigSha256));

    auto invalid = identity;
    invalid.config_sha256[0] = 1U;
    try {
        static_cast<void>(protocol::encode_firmware_identity(invalid));
        assert(false);
    } catch (const protocol::FirmwareIdentityException& error) {
        assert(error.code() == protocol::FirmwareIdentityError::InvalidDigest);
    }
}

void mock_reports_exact_availability() {
    mock_mcu::NodeInfo info;
    info.board_type = 0x010431U;
    info.firmware_identity.available_fields =
        static_cast<std::uint16_t>(
            protocol::FirmwareIdentityField::ConfigSha256);
    info.firmware_identity.config_sha256.fill(0xA5U);
    mock_mcu::RemoteCore core(info, 0U);
    const auto response = core.handle(request());
    assert(response.payload[0] == 0U);
    const auto identity = protocol::decode_firmware_identity(
        {response.payload.begin() + 1, response.payload.end()});
    assert(identity.board_type == info.board_type);
    assert(identity.available(
        protocol::FirmwareIdentityField::ConfigSha256));
    assert(!identity.available(
        protocol::FirmwareIdentityField::ProjectSha256));

    assert(core.handle(request({1U})).payload[0] ==
           static_cast<std::uint8_t>(mock_mcu::StatusCode::InvalidPayload));
    assert(core.handle(request({}, 9U)).payload[0] ==
           static_cast<std::uint8_t>(mock_mcu::StatusCode::InvalidPayload));
}

void json_preserves_unavailable_fields() {
    protocol::FirmwareIdentityPayload identity;
    identity.board_type = 17U;
    identity.available_fields = static_cast<std::uint16_t>(
        protocol::FirmwareIdentityField::ProjectSha256);
    identity.project_sha256.fill(0xABU);
    std::ostringstream output;
    cli_json::write_firmware_identity(output, identity);
    assert(output.str().find("\"project_sha256\":\"abab") !=
           std::string::npos);
    assert(output.str().find("\"config_sha256\":null") !=
           std::string::npos);
    assert(output.str().find("\"firmware_input_sha256\":null") !=
           std::string::npos);
}

}

int main() {
    protocol_round_trip_and_validation();
    mock_reports_exact_availability();
    json_preserves_unavailable_fields();
    return 0;
}
