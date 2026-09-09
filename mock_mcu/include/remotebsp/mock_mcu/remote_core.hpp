#pragma once

#include "remotebsp/mock_mcu/device_parameter_store.hpp"
#include "remotebsp/mock_mcu/bus_bsp.hpp"
#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/motion_executor.hpp"
#include "remotebsp/mock_mcu/motion_group_participant.hpp"
#include "remotebsp/mock_mcu/time_sync_bsp.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"
#include "remotebsp/mock_mcu/waveform_bsp.hpp"
#include "remotebsp/protocol/device_parameters.hpp"
#include "remotebsp/protocol/bus_stream.hpp"
#include "remotebsp/protocol/motion.hpp"
#include "remotebsp/protocol/motion_group.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/protocol/time_sync.hpp"

#include <array>
#include <chrono>
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
    Motion = 1ULL << 9U,
    TimedBitstream = 1ULL << 10U,
    DeviceParameters = 1ULL << 12U,
    Stream = 1ULL << 13U,
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
    InvalidResourceCatalog,
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
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    RemoteCore(NodeInfo node_info, std::uint64_t capabilities,
               std::shared_ptr<GpioBsp> gpio_bsp = nullptr,
               std::shared_ptr<UartBsp> uart_bsp = nullptr,
               std::vector<protocol::ResourceDescriptor> resources = {},
               std::vector<protocol::ResourceContract> contracts = {},
               std::shared_ptr<MotionExecutor> motion = nullptr,
               std::shared_ptr<WaveformBsp> waveform = nullptr,
               std::shared_ptr<DeviceParameterStore>
                   device_parameters = nullptr,
               std::shared_ptr<BusBsp> bus_bsp = nullptr,
               std::shared_ptr<TimeSyncBsp> time_sync_bsp = nullptr);

    protocol::Packet handle(
        const protocol::Packet& request,
        TimePoint now = Clock::now());
    const NodeInfo& node_info() const noexcept;
    std::uint64_t capabilities() const noexcept;
    bool bootloader_requested() const noexcept;
    std::size_t expire_leases(TimePoint now = Clock::now());
    std::size_t release_session(std::uint32_t session_id);
    std::vector<protocol::Packet> poll_uart_events(
        std::size_t maximum_payload = 64);

private:
    protocol::Packet make_response(const protocol::Packet& request,
                                   StatusCode status) const;
    protocol::Packet handle_get_info(const protocol::Packet& request) const;
    protocol::Packet handle_get_capability(
        const protocol::Packet& request) const;
    protocol::Packet handle_ping(const protocol::Packet& request) const;
    protocol::Packet handle_time_sync(
        const protocol::Packet& request, TimePoint now) const;
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
    protocol::Packet handle_resource_contract(
        const protocol::Packet& request) const;
    protocol::Packet handle_resource_acquire(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_resource_renew(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_resource_release(
        const protocol::Packet& request);
    protocol::Packet handle_resource_lease_status(
        const protocol::Packet& request, TimePoint now) const;
    protocol::Packet handle_device_parameter_status(
        const protocol::Packet& request, TimePoint now) const;
    protocol::Packet handle_device_parameter_list(
        const protocol::Packet& request) const;
    protocol::Packet handle_device_parameter_read(
        const protocol::Packet& request) const;
    protocol::Packet handle_device_parameter_unlock(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_device_parameter_write(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_device_parameter_lock(
        const protocol::Packet& request);
    protocol::Packet handle_gpio_create(const protocol::Packet& request);
    protocol::Packet handle_gpio_read(const protocol::Packet& request) const;
    protocol::Packet handle_gpio_write(const protocol::Packet& request);
    protocol::Packet handle_uart_create(const protocol::Packet& request);
    protocol::Packet handle_uart_read(const protocol::Packet& request);
    protocol::Packet handle_uart_write(const protocol::Packet& request);
    protocol::Packet handle_i2c_contract(
        const protocol::Packet& request) const;
    protocol::Packet handle_i2c_transfer(
        const protocol::Packet& request);
    protocol::Packet handle_spi_contract(
        const protocol::Packet& request) const;
    protocol::Packet handle_spi_transfer(
        const protocol::Packet& request);
    protocol::Packet handle_pwm_create(const protocol::Packet& request);
    protocol::Packet handle_pwm_write(const protocol::Packet& request);
    protocol::Packet handle_pwm_stop(const protocol::Packet& request);
    protocol::Packet handle_timed_bitstream_create(
        const protocol::Packet& request);
    protocol::Packet handle_timed_bitstream_write(
        const protocol::Packet& request);
    protocol::Packet handle_timed_bitstream_abort(
        const protocol::Packet& request);
    protocol::Packet handle_motion_enqueue(
        const protocol::Packet& request);
    protocol::Packet handle_motion_status(
        const protocol::Packet& request) const;
    protocol::Packet handle_motion_abort(
        const protocol::Packet& request);
    protocol::Packet handle_motion_clear_fault(
        const protocol::Packet& request);
    protocol::Packet handle_motion_contract(
        const protocol::Packet& request) const;
    protocol::Packet handle_motion_group_prepare(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_motion_group_commit(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet handle_motion_group_abort(
        const protocol::Packet& request, TimePoint now);
    protocol::Packet make_uart_error_response(
        const protocol::Packet& request,
        const std::exception& error) const;

    struct GpioObject {
        std::uint16_t pin{};
        GpioDirection direction{GpioDirection::Input};
        std::uint32_t resource_id{};
        std::uint32_t owner_session_id{};
    };

    struct UartObject {
        std::uint8_t port{};
        std::uint32_t resource_id{};
        std::uint32_t owner_session_id{};
        bool streaming{};
        std::uint32_t event_sequence{};
    };

    struct PwmObject {
        std::uint8_t channel{};
        std::uint32_t resource_id{};
        std::uint32_t owner_session_id{};
    };

    struct TimedBitstreamObject {
        std::uint8_t channel{};
        std::uint32_t resource_id{};
        std::uint32_t owner_session_id{};
    };

    struct Lease {
        std::uint64_t lease_id{};
        std::uint32_t owner_session_id{};
        std::uint32_t granted_duration_ms{};
        protocol::ResourceLeaseMode mode{
            protocol::ResourceLeaseMode::None};
        TimePoint expires_at{};
    };

    const protocol::ResourceDescriptor* find_resource(
        std::uint32_t resource_id) const noexcept;
    const protocol::ResourceDescriptor* find_resource(
        protocol::ResourceType type, std::uint16_t instance) const noexcept;
    const protocol::ResourceContract* find_contract(
        std::uint32_t resource_id) const noexcept;
    bool resource_has_objects(std::uint32_t resource_id) const noexcept;
    bool session_has_exclusive_lease(
        std::uint32_t resource_id, std::uint32_t session_id) const noexcept;
    bool resource_access_allowed(
        std::uint32_t resource_id, std::uint32_t session_id) const noexcept;
    void release_resource_objects(std::uint32_t resource_id,
                                  std::uint32_t owner_session_id);
    protocol::ResourceLeaseInfo make_lease_info(
        std::uint32_t resource_id, std::uint32_t requester_session_id,
        TimePoint now) const;
    NodeInfo node_info_;
    std::uint64_t capabilities_;
    std::shared_ptr<GpioBsp> gpio_bsp_;
    std::shared_ptr<UartBsp> uart_bsp_;
    std::shared_ptr<MotionExecutor> motion_;
    std::shared_ptr<MockMotionGroupParticipant> motion_group_;
    std::shared_ptr<WaveformBsp> waveform_;
    std::shared_ptr<DeviceParameterStore> device_parameters_;
    std::shared_ptr<BusBsp> bus_bsp_;
    std::shared_ptr<TimeSyncBsp> time_sync_bsp_;
    std::vector<protocol::ResourceDescriptor> resources_;
    std::vector<protocol::ResourceContract> contracts_;
    std::unordered_map<std::uint32_t, std::vector<Lease>> leases_;
    std::unordered_map<std::uint32_t, GpioObject> gpio_objects_;
    std::unordered_map<std::uint32_t, UartObject> uart_objects_;
    std::unordered_map<std::uint32_t, PwmObject> pwm_objects_;
    std::unordered_map<std::uint32_t, TimedBitstreamObject>
        timed_bitstream_objects_;
    std::uint32_t next_object_id_{1};
    std::uint64_t next_lease_id_{1};
    bool bootloader_requested_{};
    std::uint32_t parameter_unlock_session_{};
    std::uint32_t parameter_unlock_token_{};
    TimePoint parameter_unlock_expires_{};
    bool parameter_restart_required_{};
};

}
