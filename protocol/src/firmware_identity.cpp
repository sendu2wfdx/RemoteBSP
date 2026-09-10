#include "remotebsp/protocol/firmware_identity.hpp"

#include <algorithm>

namespace remotebsp::protocol {
namespace {

constexpr std::uint16_t kKnownFields =
    static_cast<std::uint16_t>(FirmwareIdentityField::ProjectSha256) |
    static_cast<std::uint16_t>(FirmwareIdentityField::ConfigSha256) |
    static_cast<std::uint16_t>(FirmwareIdentityField::FirmwareInputSha256);

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::uint16_t get_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(input[1] << 8U);
}

std::uint32_t get_u32(const std::uint8_t* input) {
    std::uint32_t value = 0U;
    for (unsigned index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

bool all_zero(const std::array<std::uint8_t, 32>& digest) {
    return std::all_of(digest.begin(), digest.end(),
                       [](std::uint8_t value) { return value == 0U; });
}

void validate(const FirmwareIdentityPayload& identity) {
    if (identity.schema_version != kFirmwareIdentitySchemaVersion) {
        throw FirmwareIdentityException(FirmwareIdentityError::UnsupportedSchema,
                                        "不支持的固件身份载荷版本");
    }
    if ((identity.available_fields & ~kKnownFields) != 0U) {
        throw FirmwareIdentityException(FirmwareIdentityError::InvalidAvailability,
                                        "固件身份包含未知可用性位");
    }
    const std::array<std::pair<FirmwareIdentityField,
                               const std::array<std::uint8_t, 32>*>, 3> fields{{
        {FirmwareIdentityField::ProjectSha256, &identity.project_sha256},
        {FirmwareIdentityField::ConfigSha256, &identity.config_sha256},
        {FirmwareIdentityField::FirmwareInputSha256,
         &identity.firmware_input_sha256},
    }};
    for (const auto& field : fields) {
        if (!identity.available(field.first) && !all_zero(*field.second)) {
            throw FirmwareIdentityException(FirmwareIdentityError::InvalidDigest,
                                            "固件身份可用性与摘要值不一致");
        }
    }
}

}

FirmwareIdentityException::FirmwareIdentityException(
    FirmwareIdentityError code, const char* message)
    : std::runtime_error(message), code_(code) {}

FirmwareIdentityError FirmwareIdentityException::code() const noexcept {
    return code_;
}

std::vector<std::uint8_t> encode_firmware_identity(
    const FirmwareIdentityPayload& identity) {
    validate(identity);
    std::vector<std::uint8_t> output;
    output.reserve(120U);
    append_u16(output, identity.schema_version);
    append_u16(output, identity.available_fields);
    append_u32(output, identity.board_type);
    output.insert(output.end(), identity.node_uuid.begin(),
                  identity.node_uuid.end());
    output.insert(output.end(), identity.project_sha256.begin(),
                  identity.project_sha256.end());
    output.insert(output.end(), identity.config_sha256.begin(),
                  identity.config_sha256.end());
    output.insert(output.end(), identity.firmware_input_sha256.begin(),
                  identity.firmware_input_sha256.end());
    return output;
}

FirmwareIdentityPayload decode_firmware_identity(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() != 120U) {
        throw FirmwareIdentityException(FirmwareIdentityError::InvalidLength,
                                        "固件身份载荷长度无效");
    }
    FirmwareIdentityPayload result;
    result.schema_version = get_u16(payload.data());
    result.available_fields = get_u16(payload.data() + 2U);
    result.board_type = get_u32(payload.data() + 4U);
    std::copy_n(payload.begin() + 8, 16, result.node_uuid.begin());
    std::copy_n(payload.begin() + 24, 32, result.project_sha256.begin());
    std::copy_n(payload.begin() + 56, 32, result.config_sha256.begin());
    std::copy_n(payload.begin() + 88, 32, result.firmware_input_sha256.begin());
    validate(result);
    return result;
}

}
