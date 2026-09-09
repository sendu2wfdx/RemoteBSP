#include "remotebsp/mock_mcu/twin_replay.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace {

using namespace remotebsp;

template <typename Callback>
void expect_manifest_error(mock_mcu::ManifestError expected,
                           Callback callback) {
    try {
        callback();
        assert(false);
    } catch (const mock_mcu::ManifestException& error) {
        assert(error.code() == expected);
    }
}

mock_mcu::FaultScenario mixed_scenario() {
    return {mock_mcu::kFaultScenarioSchemaVersion,
            {{10U, mock_mcu::FaultAction::SetBusStatus, 201326593U, false,
              protocol::BusTransactionStatus::Nack},
             {10U, mock_mcu::FaultAction::SetNodeOnline, 0U, false,
              protocol::BusTransactionStatus::Ok},
             {20U, mock_mcu::FaultAction::SetBusStatus, 234881025U, false,
              protocol::BusTransactionStatus::Timeout},
             {20U, mock_mcu::FaultAction::SetNodeOnline, 0U, true,
              protocol::BusTransactionStatus::Ok}}};
}

void check_round_trip_and_file_loop() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    const auto scenario = mixed_scenario();
    const auto record = mock_mcu::record_digital_twin(
        manifest, scenario, 1U, 0x123456789ABCDEF0ULL);
    assert(record.schema_version == 1U);
    assert(record.time_base == "monotonic-relative-ms");
    assert(record.checkpoints.size() == 2U);
    assert(record.checkpoints[0].sequence == 1U);
    assert(record.checkpoints[0].at_ms == 10U);
    assert(record.checkpoints[0].applied_events == 2U);
    assert(record.checkpoints[1].sequence == 2U);
    assert(record.checkpoints[1].applied_events == 2U);
    assert(record.summary.event_count == 4U);
    assert(record.summary.checkpoint_count == 2U);
    assert(record.summary.final_elapsed_ms == 20U);
    assert(record.summary.final_online);

    const auto encoded = mock_mcu::encode_twin_replay_record(record);
    const auto decoded = mock_mcu::parse_twin_replay_record(encoded);
    mock_mcu::verify_digital_twin_replay(
        manifest, scenario, 1U, decoded);
    assert(mock_mcu::encode_twin_replay_record(decoded) == encoded);

    std::remove(TEST_REPLAY_OUTPUT);
    mock_mcu::write_twin_replay_record(record, TEST_REPLAY_OUTPUT);
    const auto loaded =
        mock_mcu::load_twin_replay_record(TEST_REPLAY_OUTPUT);
    mock_mcu::verify_digital_twin_replay(
        manifest, scenario, 1U, loaded);
    assert(std::remove(TEST_REPLAY_OUTPUT) == 0);
}

void check_strict_schema_and_tamper_detection() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    const auto scenario = mixed_scenario();
    const auto record =
        mock_mcu::record_digital_twin(manifest, scenario, 7U, 42U);

    auto tampered = record;
    tampered.checkpoints.front().state_digest =
        "fnv1a64:0000000000000000";
    expect_manifest_error(mock_mcu::ManifestError::Conflict, [&] {
        mock_mcu::verify_digital_twin_replay(
            manifest, scenario, 7U, tampered);
    });

    expect_manifest_error(mock_mcu::ManifestError::Conflict, [&] {
        mock_mcu::verify_digital_twin_replay(
            manifest, scenario, 8U, record);
    });

    auto different_scenario = scenario;
    different_scenario.events[0].bus_status =
        protocol::BusTransactionStatus::Fault;
    expect_manifest_error(mock_mcu::ManifestError::Conflict, [&] {
        mock_mcu::verify_digital_twin_replay(
            manifest, different_scenario, 7U, record);
    });

    auto different_manifest = manifest;
    ++different_manifest.node_info.board_type;
    expect_manifest_error(mock_mcu::ManifestError::Conflict, [&] {
        mock_mcu::verify_digital_twin_replay(
            different_manifest, scenario, 7U, record);
    });

    const std::string unknown_field = R"json({
      "schema_version": 1,
      "time_base": "monotonic-relative-ms",
      "random_seed": 0,
      "scenario_id": "fnv1a64:0000000000000000",
      "board_name": "x",
      "node_instance": 1,
      "checkpoints": [],
      "summary": {"event_count": 0, "checkpoint_count": 0,
        "final_elapsed_ms": 0, "final_online": true,
        "final_state_digest": "fnv1a64:0000000000000000"},
      "extra": true
    })json";
    expect_manifest_error(mock_mcu::ManifestError::InvalidSchema, [&] {
        static_cast<void>(
            mock_mcu::parse_twin_replay_record(unknown_field));
    });

    expect_manifest_error(mock_mcu::ManifestError::InvalidJson, [&] {
        static_cast<void>(mock_mcu::parse_twin_replay_record(
            R"json({"schema_version":1,"schema_version":1})json"));
    });

    auto bad_sequence = record;
    bad_sequence.checkpoints.front().sequence = 2U;
    expect_manifest_error(mock_mcu::ManifestError::InvalidValue, [&] {
        static_cast<void>(
            mock_mcu::encode_twin_replay_record(bad_sequence));
    });

    auto bad_elapsed = record;
    ++bad_elapsed.summary.final_elapsed_ms;
    expect_manifest_error(mock_mcu::ManifestError::InvalidValue, [&] {
        static_cast<void>(
            mock_mcu::encode_twin_replay_record(bad_elapsed));
    });

    auto bad_time = record;
    bad_time.checkpoints.back().at_ms =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) + 1U;
    bad_time.summary.final_elapsed_ms = bad_time.checkpoints.back().at_ms;
    expect_manifest_error(mock_mcu::ManifestError::InvalidValue, [&] {
        static_cast<void>(mock_mcu::encode_twin_replay_record(bad_time));
    });
}

void check_repeatability_and_cross_node_isolation() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    const auto scenario = mixed_scenario();
    const auto first =
        mock_mcu::record_digital_twin(manifest, scenario, 1U, 99U);
    const auto repeated =
        mock_mcu::record_digital_twin(manifest, scenario, 1U, 99U);
    const auto second_node =
        mock_mcu::record_digital_twin(manifest, scenario, 2U, 99U);
    assert(mock_mcu::encode_twin_replay_record(first) ==
           mock_mcu::encode_twin_replay_record(repeated));
    assert(first.scenario_id == second_node.scenario_id);
    assert(first.summary.final_state_digest !=
           second_node.summary.final_state_digest);

    const auto other_seed =
        mock_mcu::record_digital_twin(manifest, scenario, 1U, 100U);
    assert(first.summary.final_state_digest !=
           other_seed.summary.final_state_digest);
}

void check_existing_fault_effects_change_checkpoints() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_DEFAULT_MANIFEST);
    const mock_mcu::FaultScenario scenario{
        mock_mcu::kFaultScenarioSchemaVersion,
        {{10U, mock_mcu::FaultAction::SetGpioInput, 0x01000003U, true},
         {20U, mock_mcu::FaultAction::SetUartFailed, 0x02000001U, true},
         {30U, mock_mcu::FaultAction::SetNodeOnline, 0U, false},
         {40U, mock_mcu::FaultAction::SetNodeOnline, 0U, true}}};
    const auto record =
        mock_mcu::record_digital_twin(manifest, scenario, 3U, 1U);
    assert(record.checkpoints.size() == 4U);
    for (std::size_t index = 1U; index < record.checkpoints.size(); ++index) {
        assert(record.checkpoints[index - 1U].state_digest !=
               record.checkpoints[index].state_digest);
    }
    mock_mcu::verify_digital_twin_replay(
        manifest, scenario, 3U, record);
}

void check_programmatic_scenario_validation() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    auto invalid = mixed_scenario();
    invalid.events[1].at_ms = 9U;
    expect_manifest_error(mock_mcu::ManifestError::InvalidValue, [&] {
        static_cast<void>(
            mock_mcu::record_digital_twin(manifest, invalid, 1U, 0U));
    });

    invalid = mixed_scenario();
    invalid.events[0].action = static_cast<mock_mcu::FaultAction>(0xFFU);
    expect_manifest_error(mock_mcu::ManifestError::InvalidValue, [&] {
        static_cast<void>(
            mock_mcu::record_digital_twin(manifest, invalid, 1U, 0U));
    });
}

void check_standard_digest_and_atomic_write_boundary() {
    const auto manifest =
        mock_mcu::load_board_manifest(TEST_BUS_MANIFEST);
    const mock_mcu::FaultScenario empty{};
    const auto record =
        mock_mcu::record_digital_twin(manifest, empty, 1U, 0U);
    assert(record.scenario_id == "fnv1a64:392209f14dea4c24");
    assert(record.summary.final_elapsed_ms == 0U);

    const std::string temporary =
        std::string(TEST_REPLAY_OUTPUT) + ".tmp";
    std::remove(TEST_REPLAY_OUTPUT);
    std::remove(temporary.c_str());
    {
        std::ofstream old_file(TEST_REPLAY_OUTPUT,
                               std::ios::binary | std::ios::trunc);
        old_file << "旧文件必须保留";
    }
    {
        std::ofstream collision(temporary,
                                std::ios::binary | std::ios::trunc);
        collision << "预先存在的临时文件";
    }
    expect_manifest_error(mock_mcu::ManifestError::Io, [&] {
        mock_mcu::write_twin_replay_record(record, TEST_REPLAY_OUTPUT);
    });
    {
        std::ifstream old_file(TEST_REPLAY_OUTPUT, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(old_file)),
                                   std::istreambuf_iterator<char>());
        assert(contents == "旧文件必须保留");
    }
    assert(std::remove(temporary.c_str()) == 0);
    assert(std::remove(TEST_REPLAY_OUTPUT) == 0);
}

}  // namespace

int main() {
    check_round_trip_and_file_loop();
    check_strict_schema_and_tamper_detection();
    check_repeatability_and_cross_node_isolation();
    check_existing_fault_effects_change_checkpoints();
    check_programmatic_scenario_validation();
    check_standard_digest_and_atomic_write_boundary();
}
