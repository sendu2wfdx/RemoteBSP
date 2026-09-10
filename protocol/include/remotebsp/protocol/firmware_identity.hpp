#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kFirmwareIdentitySchemaVersion = 1U;

enum class FirmwareIdentityField : std::uint16_t {
    ProjectSha256 = 1U << 0U,
    ConfigSha256 = 1U << 1U,
    FirmwareInputSha256 = 1U << 2U,
};

struct FirmwareIdentityPayload {
    std::uint16_t schema_version{kFirmwareIdentitySchemaVersion};
    std::uint16_t available_fields{};
    std::uint32_t board_type{};
    std::array<std::uint8_t, 16> node_uuid{};
    std::array<std::uint8_t, 32> project_sha256{};
    std::array<std::uint8_t, 32> config_sha256{};
    std::array<std::uint8_t, 32> firmware_input_sha256{};

    bool available(FirmwareIdentityField field) const noexcept {
        return (available_fields & static_cast<std::uint16_t>(field)) != 0U;
    }
};

enum class FirmwareIdentityError {
    InvalidLength,
    UnsupportedSchema,
    InvalidAvailability,
    InvalidDigest,
};

class FirmwareIdentityException : public std::runtime_error {
public:
    FirmwareIdentityException(FirmwareIdentityError code, const char* message);
    FirmwareIdentityError code() const noexcept;

private:
    FirmwareIdentityError code_;
};

std::vector<std::uint8_t> encode_firmware_identity(
    const FirmwareIdentityPayload& identity);
FirmwareIdentityPayload decode_firmware_identity(
    const std::vector<std::uint8_t>& payload);

}
