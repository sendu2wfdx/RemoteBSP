#pragma once

#include "remotebsp/mock_mcu/board_manifest.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace remotebsp::mock_mcu {

constexpr std::uint32_t kTwinReplaySchemaVersion = 1U;
constexpr const char* kTwinReplayTimeBase = "monotonic-relative-ms";

struct TwinReplayCheckpoint {
    std::uint32_t sequence{};
    std::uint64_t at_ms{};
    std::uint32_t applied_events{};
    std::string state_digest;
};

struct TwinReplaySummary {
    std::uint32_t event_count{};
    std::uint32_t checkpoint_count{};
    std::uint64_t final_elapsed_ms{};
    bool final_online{};
    std::string final_state_digest;
};

struct TwinReplayRecord {
    std::uint32_t schema_version{kTwinReplaySchemaVersion};
    std::string time_base{kTwinReplayTimeBase};
    std::uint64_t random_seed{};
    std::string scenario_id;
    std::string board_name;
    std::uint32_t node_instance{};
    std::vector<TwinReplayCheckpoint> checkpoints;
    TwinReplaySummary summary;
};

// 记录完整故障脚本。当前模型没有随机动作，但显式保存 seed，避免未来加入
// 概率故障后出现不可重放的隐式随机源。
TwinReplayRecord record_digital_twin(
    const BoardManifest& manifest, const FaultScenario& scenario,
    std::uint32_t node_instance, std::uint64_t random_seed);

// 从相同板卡描述与故障脚本重新执行，并逐检查点核对摘要；任何身份、顺序或
// 状态差异都会抛出 ManifestException。
void verify_digital_twin_replay(
    const BoardManifest& manifest, const FaultScenario& scenario,
    std::uint32_t node_instance, const TwinReplayRecord& record);

std::string encode_twin_replay_record(const TwinReplayRecord& record);
TwinReplayRecord parse_twin_replay_record(std::string_view json_text);
TwinReplayRecord load_twin_replay_record(const std::string& path);
void write_twin_replay_record(const TwinReplayRecord& record,
                              const std::string& path);

}  // namespace remotebsp::mock_mcu
