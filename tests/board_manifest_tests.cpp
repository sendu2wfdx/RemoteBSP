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
    assert(manifest.resources.size() == 32);
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
    assert(manifest.resources[24].type == ResourceType::Adc);
    assert(manifest.resources[25].instance == 1);
    assert(manifest.resources[26].type == ResourceType::StepgenAxis);
    assert(manifest.resources[28].instance == 2);
    assert(manifest.resources[29].type == ResourceType::Pwm);
    assert(manifest.resources[30].instance == 1);
    assert(manifest.resources[31].type == ResourceType::TimedBitstream);
    assert(manifest.waveform_endpoints.size() == 3);
    assert(manifest.waveform_endpoints[0].type == ResourceType::Pwm);
    assert(manifest.waveform_endpoints[0].pin == 9);
    assert(manifest.waveform_endpoints[2].type ==
           ResourceType::TimedBitstream);
    assert(manifest.waveform_endpoints[2].maximum_bits == 192);
    assert(manifest.motion_axes.size() == 3);
    assert(manifest.motion_queue_capacity == 32);
    assert(manifest.motion_maximum_total_step_rate_hz == 200000U);
    assert(manifest.motion_axes[0].maximum_step_rate_hz == 100000);
    assert((manifest.contracts[26].access_flags &
            remotebsp::protocol::kResourceAccessLeaseRequired) != 0);
    DigitalTwin adc_twin(manifest);
    assert(adc_twin.adc());
    assert((manifest.capabilities & remotebsp::mock_mcu::capability_mask(
               remotebsp::mock_mcu::Capability::Adc)) != 0U);

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
    assert(manifest.motion_maximum_total_step_rate_hz == 30000U);
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

void test_versioned_firmware_identity_manifest() {
    const auto full = remotebsp::mock_mcu::load_board_manifest(
        TEST_IDENTITY_V3_MANIFEST);
    assert(full.schema_version == 3U);
    assert(full.node_info.firmware_identity.available(
        remotebsp::protocol::FirmwareIdentityField::ProjectSha256));
    assert(full.node_info.firmware_identity.available(
        remotebsp::protocol::FirmwareIdentityField::ConfigSha256));
    assert(full.node_info.firmware_identity.available(
        remotebsp::protocol::FirmwareIdentityField::FirmwareInputSha256));
    assert(full.node_info.firmware_identity.project_sha256[0] == 0x11U);
    assert(full.node_info.firmware_identity.config_sha256[0] == 0x22U);
    assert(full.node_info.firmware_identity.firmware_input_sha256[0] == 0x33U);

    const auto unavailable = remotebsp::mock_mcu::load_board_manifest(
        TEST_IDENTITY_UNAVAILABLE_V3_MANIFEST);
    assert(unavailable.schema_version == 3U);
    assert(unavailable.node_info.firmware_identity.available_fields == 0U);

    const auto legacy = remotebsp::mock_mcu::load_board_manifest(
        TEST_IDENTITY_V1_MANIFEST);
    assert(legacy.schema_version == 1U);
    assert(legacy.node_info.firmware_identity.available_fields == 0U);

    const std::string invalid = R"json({
      "schema_version": 3,
      "name": "bad-identity",
      "board_type": 1,
      "uuid": "00112233445566778899aabbccddee00",
      "firmware_version": [0, 2, 0],
      "firmware_identity": {
        "project_sha256": "xyz",
        "config_sha256": null,
        "firmware_input_sha256": null
      },
      "capabilities": [],
      "resource_groups": [],
      "bus_resources": [],
      "reserved_resources": []
    })json";
    try {
        static_cast<void>(remotebsp::mock_mcu::parse_board_manifest(invalid));
        assert(false);
    } catch (const ManifestException& error) {
        assert(error.code() == ManifestError::InvalidSchema);
    }
}

void test_versioned_stream_manifest_and_mock_source() {
    const std::string json = R"json({
      "schema_version": 4,
      "name": "mock-stream-v4",
      "board_type": 1,
      "uuid": "00112233445566778899aabbccddee00",
      "firmware_version": [0, 2, 0],
      "firmware_identity": {
        "project_sha256": null,
        "config_sha256": null,
        "firmware_input_sha256": null
      },
      "capabilities": ["stream"],
      "resource_groups": [{
        "type": "stream", "first_instance": 1, "count": 1,
        "id_base": 251658241, "source": "native",
        "rx_capacity": 64, "tx_capacity": 64,
        "contract": {
          "access": ["read", "shared_read", "lease_supported", "lease_required"],
          "timing_resolution_ns": 1000, "worst_case_latency_us": 1000,
          "maximum_operations_per_second": 1000, "queue_capacity": 64,
          "maximum_rx_bits_per_second": 0,
          "maximum_tx_bits_per_second": 1000000
        }
      }],
      "bus_resources": [],
      "stream_resources": [{
        "resource_id": 251658241, "direction": "node_to_host",
        "transports": ["usb"], "flags": ["lossless", "credit_required"],
        "maximum_chunk_bytes": 16, "buffer_capacity_bytes": 64,
        "sustained_bits_per_second": 100000, "peak_bits_per_second": 1000000,
        "maximum_latency_us": 1000, "maximum_jitter_us": 100
      }],
      "reserved_resources": []
    })json";
    auto manifest = remotebsp::mock_mcu::parse_board_manifest(json);
    assert(manifest.schema_version == 4U);
    assert(manifest.stream_resources.size() == 1U);
    DigitalTwin twin(std::move(manifest));
    assert(twin.stream());
    assert(twin.stream()->produce(251658241U, {1U, 2U}) ==
           remotebsp::mock_mcu::StreamPushStatus::Accepted);
    assert(twin.stream()->buffered_bytes(251658241U) == 2U);
}

}

int main() {
    test_default_board_manifest();
    test_fly_d5_board_manifest();
    test_digital_twin_fault_isolation();
    test_fault_file_and_schema_rejection();
    test_versioned_firmware_identity_manifest();
    test_versioned_stream_manifest_and_mock_source();
}
