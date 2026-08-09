#include "remotebsp/mock_mcu/board_manifest.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using remotebsp::mock_mcu::DigitalTwin;
using remotebsp::mock_mcu::FaultAction;
using remotebsp::mock_mcu::FaultEvent;
using remotebsp::mock_mcu::FaultScenario;
using remotebsp::mock_mcu::ManifestError;
using remotebsp::mock_mcu::ManifestException;
using remotebsp::protocol::ResourceType;

void test_default_board_manifest() {
    const auto manifest =
        remotebsp::mock_mcu::load_board_manifest(TEST_BOARD_MANIFEST);
    assert(manifest.schema_version == 1);
    assert(manifest.name == "mock-generic-v1");
    assert(manifest.resources.size() == 30);
    assert(manifest.contracts.size() == manifest.resources.size());
    assert(manifest.reserved_resources.size() == 1);
    assert(manifest.reserved_resources[0].type == ResourceType::Spi);
    assert(manifest.reserved_resources[0].instance == 1);

    assert(manifest.resources[0].type == ResourceType::Gpio);
    assert(manifest.resources[15].instance == 15);
    assert(manifest.resources[16].type == ResourceType::Uart);
    assert(manifest.resources[20].flags ==
           remotebsp::protocol::kResourceFlagExpanded);
    assert(manifest.contracts[23].maximum_tx_bits_per_second ==
           3000000);
    assert(manifest.resources[24].type == ResourceType::StepgenAxis);
    assert(manifest.resources[26].instance == 2);
    assert(manifest.resources[27].type == ResourceType::Pwm);
    assert(manifest.resources[28].instance == 1);
    assert(manifest.resources[29].type == ResourceType::TimedBitstream);
    assert(manifest.motion_axes.size() == 3);
    assert(manifest.motion_queue_capacity == 32);
    assert(manifest.motion_axes[0].maximum_step_rate_hz == 100000);
    assert((manifest.contracts[24].access_flags &
            remotebsp::protocol::kResourceAccessLeaseRequired) != 0);

    const auto first =
        remotebsp::mock_mcu::instantiate_node_info(manifest, 1);
    const auto second =
        remotebsp::mock_mcu::instantiate_node_info(manifest, 2);
    assert(first.board_type == second.board_type);
    assert(first.uuid != second.uuid);
    assert(first.uuid[15] == 1);
    assert(second.uuid[15] == 2);
}

void test_fly_d5_board_manifest() {
    const auto manifest =
        remotebsp::mock_mcu::load_board_manifest(
            TEST_FLY_D5_MANIFEST);
    assert(manifest.name == "mellow-fly-d5-v1");
    assert(manifest.node_info.board_type == 0x00F072D5U);
    assert(manifest.resources.size() == 21);
    assert(manifest.resources[15].type == ResourceType::Gpio);
    assert(manifest.resources[16].type ==
           ResourceType::StepgenAxis);
    assert(manifest.resources[20].instance == 4);
    assert(manifest.motion_axes.size() == 5);
    assert(manifest.motion_queue_capacity == 8);
}

void test_digital_twin_fault_isolation() {
    auto manifest =
        remotebsp::mock_mcu::load_board_manifest(TEST_BOARD_MANIFEST);
    const FaultScenario scenario{
        1,
        {
            FaultEvent{100, FaultAction::SetUartFailed,
                       0x02000001U, true},
            FaultEvent{150, FaultAction::SetGpioInput,
                       0x01000003U, true},
            FaultEvent{200, FaultAction::SetNodeOnline, 0, false},
            FaultEvent{300, FaultAction::SetUartFailed,
                       0x02000001U, false},
            FaultEvent{400, FaultAction::SetNodeOnline, 0, true},
        }};
    DigitalTwin twin(std::move(manifest), scenario);
    twin.uart()->configure(
        0, {115200, 8, 1,
            remotebsp::mock_mcu::UartParity::None});
    twin.uart()->configure(
        1, {115200, 8, 1,
            remotebsp::mock_mcu::UartParity::None});
    assert(twin.next_event_ms() == 100);
    assert(twin.advance_to(99) == 0);
    assert(twin.advance_to(100) == 1);
    assert(twin.uart()->status(1).failed);
    assert(!twin.uart()->status(0).failed);

    twin.uart()->write(0, {0x11, 0x22});
    assert(twin.uart()->take_tx(0) ==
           std::vector<std::uint8_t>({0x11, 0x22}));
    assert(twin.advance_to(200) == 2);
    twin.gpio()->configure(
        3, remotebsp::mock_mcu::GpioDirection::Input, false);
    assert(twin.gpio()->read(3));
    assert(!twin.online());
    assert(twin.advance_to(400) == 2);
    assert(!twin.uart()->status(1).failed);
    assert(twin.online());
    assert(!twin.next_event_ms().has_value());
}

void test_fault_file_and_schema_rejection() {
    const auto scenario =
        remotebsp::mock_mcu::load_fault_scenario(TEST_FAULT_SCENARIO);
    assert(scenario.events.size() == 4);
    assert(scenario.events[0].action == FaultAction::SetUartFailed);
    assert(scenario.events[2].action == FaultAction::SetNodeOnline);

    const std::string invalid = R"json(
        {
          "schema_version": 1,
          "name": "bad",
          "board_type": 1,
          "uuid": "00000000000000000000000000000000",
          "firmware_version": [0, 1, 0],
          "capabilities": ["gpio"],
          "resource_groups": [],
          "reserved_resources": [],
          "typo": true
        }
    )json";
    try {
        static_cast<void>(
            remotebsp::mock_mcu::parse_board_manifest(invalid));
        assert(false);
    } catch (const ManifestException& error) {
        assert(error.code() == ManifestError::InvalidSchema);
    }

    const std::string out_of_order = R"json(
        {
          "schema_version": 1,
          "events": [
            {"at_ms": 2, "action": "node_online", "value": false},
            {"at_ms": 1, "action": "node_online", "value": true}
          ]
        }
    )json";
    try {
        static_cast<void>(
            remotebsp::mock_mcu::parse_fault_scenario(out_of_order));
        assert(false);
    } catch (const ManifestException& error) {
        assert(error.code() == ManifestError::InvalidValue);
    }
}

}

int main() {
    test_default_board_manifest();
    test_fly_d5_board_manifest();
    test_digital_twin_fault_isolation();
    test_fault_file_and_schema_rejection();
}
