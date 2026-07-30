#pragma once

#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

constexpr std::uint16_t kResponseErrorFlag =
    protocol::kErrorResponseFlag;

enum class Capability : std::uint64_t {
    Gpio = 1ULL << 0U,
    Uart = 1ULL << 1U,
    Spi = 1ULL << 2U,
    I2c = 1ULL << 3U,
    Adc = 1ULL << 4U,
    Pwm = 1ULL << 5U,
    Timer = 1ULL << 6U,
    Storage = 1ULL << 7U,
    Bootloader = 1ULL << 8U,
};

constexpr std::uint64_t capability_mask(Capability capability) noexcept {
    return static_cast<std::uint64_t>(capability);
}

enum class StatusCode : std::uint8_t {
    Ok = 0,
    UnknownCommand = 1,
    InvalidPayload = 2,
    ObjectNotFound = 3,
    AccessDenied = 4,
    ResourceExhausted = 5,
    UnsupportedCapability = 6,
    ResourceFailed = 7,
    ResourceBusy = 8,
};

struct NodeInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::uint16_t firmware_major{};
    std::uint16_t firmware_minor{};
    std::uint16_t firmware_patch{};
    std::uint32_t board_type{};
    std::uint8_t protocol_version{protocol::kProtocolVersion};
};

enum class CoreError {
    NotRequest,
    UnsupportedVersion,
};

class CoreException : public std::runtime_error {
public:
    CoreException(CoreError code, const char* message);
    CoreError code() const noexcept;

private:
    CoreError code_;
};

class RemoteCore {
public:
    RemoteCore(NodeInfo node_info, std::uint64_t capabilities,
               std::shared_ptr<GpioBsp> gpio_bsp = nullptr,
               std::shared_ptr<UartBsp> uart_bsp = nullptr,
               std::vector<protocol::ResourceDescriptor> resources = {});

    protocol::Packet handle(const protocol::Packet& request);
    const NodeInfo& node_info() const noexcept;
    std::uint64_t capabilities() const noexcept;
    bool bootloader_requested() const noexcept;

private:
    protocol::Packet make_response(const protocol::Packet& request,
                                   StatusCode status) const;
    protocol::Packet handle_get_info(const protocol::Packet& request) const;
    protocol::Packet handle_get_capability(
        const protocol::Packet& request) const;
    protocol::Packet handle_ping(const protocol::Packet& request) const;
    protocol::Packet handle_bootloader_enter(
        const protocol::Packet& request);
    protocol::Packet handle_resource_enum(
        const protocol::Packet& request) const;
    protocol::Packet handle_resource_describe(
        const protocol::Packet& request) const;
    protocol::Packet handle_resource_status(
        const protocol::Packet& request) const;
    protocol::Packet handle_resource_reset(
        const protocol::Packet& request);
    protocol::Packet handle_gpio_create(const protocol::Packet& request);
    protocol::Packet handle_gpio_read(const protocol::Packet& request) const;
    protocol::Packet handle_gpio_write(const protocol::Packet& request);
    protocol::Packet handle_uart_create(const protocol::Packet& request);
    protocol::Packet handle_uart_read(const protocol::Packet& request);
    protocol::Packet handle_uart_write(const protocol::Packet& request);
    protocol::Packet make_uart_error_response(
        const protocol::Packet& request,
        const std::exception& error) const;

    struct GpioObject {
        std::uint16_t pin{};
        GpioDirection direction{GpioDirection::Input};
    };

    struct UartObject {
        std::uint8_t port{};
    };

    NodeInfo node_info_;
    std::uint64_t capabilities_;
    std::shared_ptr<GpioBsp> gpio_bsp_;
    std::shared_ptr<UartBsp> uart_bsp_;
    std::vector<protocol::ResourceDescriptor> resources_;
    std::unordered_map<std::uint32_t, GpioObject> gpio_objects_;
    std::unordered_map<std::uint32_t, UartObject> uart_objects_;
    std::uint32_t next_object_id_{1};
    bool bootloader_requested_{};
};

}
