#pragma once

#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/toolbusd/clock_model.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace remotebsp::toolbusd {

enum class NodeUpdate {
    Added,
    Updated,
};

enum class NodeClockRegistrationResult : std::uint8_t {
    Registered = 0,
    AlreadyRegistered,
    ConfigurationMismatch,
    ReplacedBootEpoch,
    InvalidBootEpoch,
    UnknownNode,
    NodeUnavailable,
    CapacityReached,
};

enum class NodeClockAccessStatus : std::uint8_t {
    Ready = 0,
    UnknownNode,
    NodeUnavailable,
    NotRegistered,
    BootEpochMismatch,
    SampleRejected,
    TargetInPast,
    Unsynced,
    Degraded,
    ErrorBoundExceeded,
    ConversionFailed,
};

struct NodeClockSampleOutcome {
    NodeClockAccessStatus status{NodeClockAccessStatus::NotRegistered};
    std::optional<ClockSampleResult> sample_result;
};

struct HostToNodeTimeResult {
    NodeClockAccessStatus status{NodeClockAccessStatus::NotRegistered};
    std::optional<std::uint64_t> node_tick;
    std::optional<ClockEstimate> estimate;
    // 每次注册新模型或接纳新样本都会变化。运动事务必须冻结此代数与换算结果，
    // 但后续同步样本可以继续更新注册表中的活动模型。
    std::optional<std::uint64_t> model_generation;
};

struct NodeRecord {
    protocol::NodeIdentity identity;
    std::uint32_t node_id{};
    bool online{};
    bool assigned{};
    std::chrono::steady_clock::time_point last_seen;
};

class NodeRegistry {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit NodeRegistry(std::chrono::milliseconds offline_timeout =
                              std::chrono::milliseconds(2000),
                          std::size_t maximum_clock_models = 128U);

    protocol::Packet make_discovery_request(
        std::uint32_t request_id,
        std::uint8_t minimum_version = protocol::kProtocolVersion,
        std::uint8_t maximum_version = protocol::kProtocolVersion) const;
    NodeUpdate accept_discovery_response(const protocol::Packet& response,
                                         TimePoint now = Clock::now());
    protocol::Packet make_node_assignment(
        const protocol::NodeUuid& uuid, std::uint32_t node_id,
        std::uint32_t request_id);
    bool mark_assignment_unconfirmed(const protocol::NodeUuid& uuid);
    bool accept_heartbeat(const protocol::Packet& heartbeat,
                          TimePoint now = Clock::now());
    std::vector<protocol::NodeUuid> expire(TimePoint now = Clock::now());

    /*
     * boot_epoch 由未来同步协议或可靠的启动身份源提供；当前发现/心跳
     * 协议不会推测该值。同一 node_id 只保留当前启动代次的有界模型。
     */
    NodeClockRegistrationResult register_clock_model(
        std::uint32_t node_id, std::uint64_t boot_epoch,
        ClockModelConfig config = {});
    NodeClockSampleOutcome add_clock_sample(
        std::uint32_t node_id, std::uint64_t boot_epoch,
        const FourTimestampSample& sample);
    std::optional<ClockEstimate> clock_estimate(
        std::uint32_t node_id, std::uint64_t boot_epoch,
        std::uint64_t host_now_ns) const;
    HostToNodeTimeResult host_to_node_time(
        std::uint32_t node_id, std::uint64_t boot_epoch,
        std::uint64_t host_time_ns, std::uint64_t host_now_ns,
        std::uint64_t maximum_error_bound_ns) const;
    bool reset_clock_model(std::uint32_t node_id) noexcept;
    std::optional<std::uint64_t> clock_boot_epoch(
        std::uint32_t node_id) const noexcept;
    std::optional<std::uint64_t> clock_model_generation(
        std::uint32_t node_id) const noexcept;
    std::size_t clock_model_count() const noexcept;

    const NodeRecord* find(const protocol::NodeUuid& uuid) const noexcept;
    const NodeRecord* find_by_node_id(std::uint32_t node_id) const noexcept;
    std::size_t size() const noexcept;
    std::size_t online_count() const noexcept;
    std::vector<NodeRecord> records() const;

private:
    struct UuidHash {
        std::size_t operator()(const protocol::NodeUuid& uuid) const noexcept;
    };

    struct NodeClockEntry {
        std::uint64_t boot_epoch{};
        std::uint64_t generation{};
        ClockModel model;
    };

    const NodeClockEntry* find_clock_entry(
        std::uint32_t node_id, std::uint64_t boot_epoch) const noexcept;
    bool node_is_available(std::uint32_t node_id) const noexcept;
    std::uint64_t allocate_clock_model_generation() noexcept;

    std::chrono::milliseconds offline_timeout_;
    std::size_t maximum_clock_models_;
    std::unordered_map<protocol::NodeUuid, NodeRecord, UuidHash> nodes_;
    std::unordered_map<std::uint32_t, NodeClockEntry> clock_models_;
    std::uint64_t next_clock_model_generation_{1U};
};

}
