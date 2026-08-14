#pragma once

#include "remotebsp/mock_mcu/gpio_bsp.hpp"
#include "remotebsp/mock_mcu/motion_executor.hpp"
#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/mock_mcu/uart_bsp.hpp"
#include "remotebsp/mock_mcu/waveform_bsp.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace remotebsp::mock_mcu {

constexpr std::uint32_t kBoardManifestSchemaVersion = 1;
constexpr std::uint32_t kFaultScenarioSchemaVersion = 1;

struct ReservedResource {
    protocol::ResourceType type{protocol::ResourceType::Gpio};
    std::uint16_t instance{};
    std::string owner;
};

/* 板卡公开的固定波形硬件组合。Studio只能选择这些已经验证的
 * 引脚、定时器、通道与DMA组合，不能任意拼接硬件资源。 */
struct WaveformEndpointCapability {
    protocol::ResourceType type{protocol::ResourceType::Pwm};
    std::uint16_t instance{};
    std::uint16_t pin{};
    std::uint8_t timer{};
    std::uint8_t channel{};
    std::uint8_t dma_channel{};
    std::uint32_t maximum_frequency_hz{};
    std::uint16_t maximum_bits{};
    std::uint32_t maximum_bit_rate{};
};

struct BoardManifest {
    std::uint32_t schema_version{kBoardManifestSchemaVersion};
    std::string name;
    NodeInfo node_info;
    std::uint64_t capabilities{};
    std::vector<protocol::ResourceDescriptor> resources;
    std::vector<protocol::ResourceContract> contracts;
    std::vector<ReservedResource> reserved_resources;
    std::vector<MotionAxisConfig> motion_axes;
    std::vector<WaveformEndpointCapability> waveform_endpoints;
    std::size_t motion_queue_capacity{32};
    std::uint32_t motion_maximum_total_step_rate_hz{};
};

enum class ManifestError {
    Io,
    InvalidJson,
    InvalidSchema,
    InvalidValue,
    Conflict,
};

class ManifestException : public std::runtime_error {
public:
    ManifestException(ManifestError code, const std::string& message);
    ManifestError code() const noexcept;

private:
    ManifestError code_;
};

BoardManifest parse_board_manifest(std::string_view json_text);
BoardManifest load_board_manifest(const std::string& path);
NodeInfo instantiate_node_info(const BoardManifest& manifest,
                               std::uint32_t instance);

enum class FaultAction {
    SetUartFailed,
    SetGpioInput,
    SetNodeOnline,
    TriggerMotionLimit,
};

struct FaultEvent {
    std::uint64_t at_ms{};
    FaultAction action{FaultAction::SetUartFailed};
    std::uint32_t resource_id{};
    bool value{};
};

struct FaultScenario {
    std::uint32_t schema_version{kFaultScenarioSchemaVersion};
    std::vector<FaultEvent> events;
};

FaultScenario parse_fault_scenario(std::string_view json_text);
FaultScenario load_fault_scenario(const std::string& path);

class DigitalTwin {
public:
    explicit DigitalTwin(BoardManifest manifest,
                         FaultScenario scenario = {});

    const BoardManifest& manifest() const noexcept;
    const std::shared_ptr<MockGpioBsp>& gpio() const noexcept;
    const std::shared_ptr<MockUartBsp>& uart() const noexcept;
    const std::shared_ptr<MotionExecutor>& motion() const noexcept;
    const std::shared_ptr<WaveformBsp>& waveform() const noexcept;
    bool online() const noexcept;
    std::optional<std::uint64_t> next_event_ms() const noexcept;
    std::size_t advance_to(std::uint64_t elapsed_ms);
    std::vector<MotionEdge> take_motion_edges();

private:
    const protocol::ResourceDescriptor& require_resource(
        std::uint32_t resource_id, protocol::ResourceType type) const;
    void apply(const FaultEvent& event);

    BoardManifest manifest_;
    FaultScenario scenario_;
    std::shared_ptr<MockGpioBsp> gpio_;
    std::shared_ptr<MockUartBsp> uart_;
    std::shared_ptr<MotionExecutor> motion_;
    std::shared_ptr<WaveformBsp> waveform_;
    std::vector<MotionEdge> pending_motion_edges_;
    std::size_t next_event_index_{};
    bool online_{true};
};

RemoteCore make_remote_core(const DigitalTwin& twin,
                            std::uint32_t instance);

}
