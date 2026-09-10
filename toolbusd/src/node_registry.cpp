#include "remotebsp/toolbusd/node_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace remotebsp::toolbusd {
namespace {

bool same_clock_config(const ClockModelConfig& left,
                       const ClockModelConfig& right) noexcept {
    return left.nominal_tick_rate_hz == right.nominal_tick_rate_hz &&
           left.node_counter_bits == right.node_counter_bits &&
           left.window_size == right.window_size &&
           left.low_rtt_sample_count == right.low_rtt_sample_count &&
           left.minimum_samples == right.minimum_samples &&
           left.minimum_fit_span_ns == right.minimum_fit_span_ns &&
           left.maximum_round_trip_ns == right.maximum_round_trip_ns &&
           left.synchronized_max_age_ns == right.synchronized_max_age_ns &&
           left.model_expiry_ns == right.model_expiry_ns &&
           left.maximum_error_bound_ns == right.maximum_error_bound_ns &&
           left.minimum_drift_uncertainty_ppm ==
               right.minimum_drift_uncertainty_ppm &&
           left.maximum_rate_deviation_ppm ==
               right.maximum_rate_deviation_ppm;
}

}

NodeRegistry::NodeRegistry(std::chrono::milliseconds offline_timeout,
                           std::size_t maximum_clock_models)
    : offline_timeout_(offline_timeout),
      maximum_clock_models_(maximum_clock_models) {
    if (offline_timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("节点离线超时必须大于零");
    }
    if (maximum_clock_models_ == 0U) {
        throw std::invalid_argument("时钟模型容量必须大于零");
    }
}

protocol::Packet NodeRegistry::make_discovery_request(
    std::uint32_t request_id, std::uint8_t minimum_version,
    std::uint8_t maximum_version) const {
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryRequest);
    request.header.request_id = request_id;
    request.payload = protocol::encode_discovery_request(
        {minimum_version, maximum_version});
    return request;
}

NodeUpdate NodeRegistry::accept_discovery_response(
    const protocol::Packet& response, TimePoint now) {
    if (response.header.message_type != protocol::MessageType::Response ||
        response.header.command != static_cast<std::uint16_t>(
                                       protocol::Command::DiscoveryResponse)) {
        throw std::invalid_argument("消息不是发现响应");
    }
    const auto identity = protocol::decode_node_identity(response.payload);
    const auto found = nodes_.find(identity.uuid);
    if (found == nodes_.end()) {
        nodes_.emplace(identity.uuid,
                       NodeRecord{identity, 0, true, false, now});
        return NodeUpdate::Added;
    }
    found->second.identity = identity;
    found->second.online = true;
    found->second.last_seen = now;
    return NodeUpdate::Updated;
}

protocol::Packet NodeRegistry::make_node_assignment(
    const protocol::NodeUuid& uuid, std::uint32_t node_id,
    std::uint32_t request_id) {
    const auto found = nodes_.find(uuid);
    if (found == nodes_.end()) {
        throw std::invalid_argument("不能为未知节点分配 ID");
    }
    if (node_id == 0U) {
        throw std::invalid_argument("节点 ID 不能为零");
    }
    for (const auto& entry : nodes_) {
        if (entry.first != uuid && entry.second.node_id == node_id) {
            throw std::invalid_argument("节点 ID 已被其他节点占用");
        }
    }
    protocol::Packet request;
    request.header.message_type = protocol::MessageType::Request;
    request.header.command =
        static_cast<std::uint16_t>(protocol::Command::NodeAssign);
    request.header.request_id = request_id;
    request.payload =
        protocol::encode_node_assignment({uuid, node_id});
    reset_clock_model(found->second.node_id);
    reset_clock_model(node_id);
    found->second.node_id = node_id;
    found->second.assigned = false;
    return request;
}

bool NodeRegistry::mark_assignment_unconfirmed(
    const protocol::NodeUuid& uuid) {
    const auto found = nodes_.find(uuid);
    if (found == nodes_.end()) {
        return false;
    }
    reset_clock_model(found->second.node_id);
    found->second.assigned = false;
    return true;
}

bool NodeRegistry::accept_heartbeat(const protocol::Packet& heartbeat,
                                    TimePoint now) {
    if (heartbeat.header.message_type != protocol::MessageType::Event ||
        heartbeat.header.command !=
            static_cast<std::uint16_t>(protocol::Command::Heartbeat)) {
        return false;
    }
    const auto decoded = protocol::decode_heartbeat(heartbeat.payload);
    const auto found = nodes_.find(decoded.uuid);
    if (found == nodes_.end() ||
        found->second.identity.protocol_version !=
            decoded.protocol_version) {
        return false;
    }
    if (heartbeat.header.object_id != 0) {
        for (const auto& entry : nodes_) {
            if (entry.first != decoded.uuid &&
                entry.second.node_id == heartbeat.header.object_id) {
                return false;
            }
        }
        if (found->second.node_id != heartbeat.header.object_id) {
            reset_clock_model(found->second.node_id);
            reset_clock_model(heartbeat.header.object_id);
        }
        found->second.node_id = heartbeat.header.object_id;
        found->second.assigned = true;
    }
    found->second.online = true;
    found->second.last_seen = now;
    return true;
}

bool NodeRegistry::observe_response(std::uint32_t node_id,
                                    TimePoint now) noexcept {
    for (auto& entry : nodes_) {
        auto& node = entry.second;
        if (node.node_id == node_id && node.assigned && node.online) {
            node.last_seen = now;
            return true;
        }
    }
    return false;
}

std::vector<protocol::NodeUuid> NodeRegistry::expire(TimePoint now) {
    std::vector<protocol::NodeUuid> offline;
    for (auto& entry : nodes_) {
        NodeRecord& node = entry.second;
        if (node.online && now - node.last_seen >= offline_timeout_) {
            node.online = false;
            reset_clock_model(node.node_id);
            offline.push_back(entry.first);
        }
    }
    return offline;
}

NodeClockRegistrationResult NodeRegistry::register_clock_model(
    std::uint32_t node_id, std::uint64_t boot_epoch,
    ClockModelConfig config) {
    if (boot_epoch == 0U) {
        return NodeClockRegistrationResult::InvalidBootEpoch;
    }
    const auto* node = find_by_node_id(node_id);
    if (node == nullptr) {
        return NodeClockRegistrationResult::UnknownNode;
    }
    if (!node->online || !node->assigned) {
        return NodeClockRegistrationResult::NodeUnavailable;
    }

    const auto found = clock_models_.find(node_id);
    if (found != clock_models_.end() &&
        found->second.boot_epoch == boot_epoch) {
        return same_clock_config(found->second.model.config(), config)
                   ? NodeClockRegistrationResult::AlreadyRegistered
                   : NodeClockRegistrationResult::ConfigurationMismatch;
    }
    if (found == clock_models_.end() &&
        clock_models_.size() >= maximum_clock_models_) {
        return NodeClockRegistrationResult::CapacityReached;
    }

    ClockModel replacement(config);
    const bool replaced = found != clock_models_.end();
    if (replaced) {
        found->second = NodeClockEntry{
            boot_epoch, allocate_clock_model_generation(),
            std::move(replacement)};
    } else {
        clock_models_.emplace(
            node_id,
            NodeClockEntry{boot_epoch, allocate_clock_model_generation(),
                           std::move(replacement)});
    }
    return replaced
               ? NodeClockRegistrationResult::ReplacedBootEpoch
               : NodeClockRegistrationResult::Registered;
}

NodeClockSampleOutcome NodeRegistry::add_clock_sample(
    std::uint32_t node_id, std::uint64_t boot_epoch,
    const FourTimestampSample& sample) {
    const auto* node = find_by_node_id(node_id);
    if (node == nullptr) {
        return {NodeClockAccessStatus::UnknownNode, std::nullopt};
    }
    if (!node->online || !node->assigned) {
        return {NodeClockAccessStatus::NodeUnavailable, std::nullopt};
    }
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end()) {
        return {NodeClockAccessStatus::NotRegistered, std::nullopt};
    }
    if (found->second.boot_epoch != boot_epoch) {
        return {NodeClockAccessStatus::BootEpochMismatch, std::nullopt};
    }
    const auto sample_result = found->second.model.add_sample(sample);
    if (sample_result == ClockSampleResult::Accepted) {
        found->second.generation = allocate_clock_model_generation();
    }
    return {sample_result == ClockSampleResult::Accepted
                ? NodeClockAccessStatus::Ready
                : NodeClockAccessStatus::SampleRejected,
            sample_result};
}

std::optional<ClockEstimate> NodeRegistry::clock_estimate(
    std::uint32_t node_id, std::uint64_t boot_epoch,
    std::uint64_t host_now_ns) const {
    if (!node_is_available(node_id)) {
        return std::nullopt;
    }
    const auto* entry = find_clock_entry(node_id, boot_epoch);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->model.estimate(host_now_ns);
}

HostToNodeTimeResult NodeRegistry::host_to_node_time(
    std::uint32_t node_id, std::uint64_t boot_epoch,
    std::uint64_t host_time_ns, std::uint64_t host_now_ns,
    std::uint64_t maximum_error_bound_ns) const {
    HostToNodeTimeResult result;
    const auto* node = find_by_node_id(node_id);
    if (node == nullptr) {
        result.status = NodeClockAccessStatus::UnknownNode;
        return result;
    }
    if (!node->online || !node->assigned) {
        result.status = NodeClockAccessStatus::NodeUnavailable;
        return result;
    }
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end()) {
        result.status = NodeClockAccessStatus::NotRegistered;
        return result;
    }
    if (found->second.boot_epoch != boot_epoch) {
        result.status = NodeClockAccessStatus::BootEpochMismatch;
        return result;
    }
    if (host_time_ns < host_now_ns) {
        result.status = NodeClockAccessStatus::TargetInPast;
        return result;
    }

    /* 在目标执行时刻评估老化，未来跨度也必须计入误差包络。 */
    result.estimate = found->second.model.estimate(host_time_ns);
    if (!result.estimate->valid ||
        result.estimate->state == ClockSyncState::Unsynced) {
        result.status = NodeClockAccessStatus::Unsynced;
        return result;
    }
    if (result.estimate->state == ClockSyncState::Degraded) {
        result.status = NodeClockAccessStatus::Degraded;
        return result;
    }
    if (maximum_error_bound_ns == 0U ||
        result.estimate->error_bound_ns > maximum_error_bound_ns) {
        result.status = NodeClockAccessStatus::ErrorBoundExceeded;
        return result;
    }
    result.node_tick =
        found->second.model.host_to_node_ticks(host_time_ns);
    if (!result.node_tick.has_value()) {
        result.status = NodeClockAccessStatus::ConversionFailed;
        return result;
    }
    result.model_generation = found->second.generation;
    result.status = NodeClockAccessStatus::Ready;
    return result;
}

bool NodeRegistry::reset_clock_model(std::uint32_t node_id) noexcept {
    return clock_models_.erase(node_id) != 0U;
}

std::optional<std::uint64_t> NodeRegistry::clock_boot_epoch(
    std::uint32_t node_id) const noexcept {
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end()) {
        return std::nullopt;
    }
    return found->second.boot_epoch;
}

std::optional<std::uint64_t> NodeRegistry::clock_model_generation(
    std::uint32_t node_id) const noexcept {
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end()) {
        return std::nullopt;
    }
    return found->second.generation;
}

std::optional<NodeClockQuality> NodeRegistry::clock_quality(
    std::uint32_t node_id, std::uint64_t host_now_ns) const {
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end()) {
        return std::nullopt;
    }
    return NodeClockQuality{
        found->second.boot_epoch, found->second.generation,
        found->second.model.estimate(host_now_ns)};
}

std::size_t NodeRegistry::clock_model_count() const noexcept {
    return clock_models_.size();
}

std::uint64_t NodeRegistry::allocate_clock_model_generation() noexcept {
    if (next_clock_model_generation_ == 0U) {
        next_clock_model_generation_ = 1U;
    }
    const auto generation = next_clock_model_generation_++;
    if (next_clock_model_generation_ == 0U) {
        next_clock_model_generation_ = 1U;
    }
    return generation;
}

const NodeRecord* NodeRegistry::find(
    const protocol::NodeUuid& uuid) const noexcept {
    const auto found = nodes_.find(uuid);
    return found == nodes_.end() ? nullptr : &found->second;
}

const NodeRecord* NodeRegistry::find_by_node_id(
    std::uint32_t node_id) const noexcept {
    for (const auto& entry : nodes_) {
        if (entry.second.node_id == node_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::size_t NodeRegistry::size() const noexcept { return nodes_.size(); }

std::size_t NodeRegistry::online_count() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : nodes_) {
        if (entry.second.online) {
            ++count;
        }
    }
    return count;
}

std::vector<NodeRecord> NodeRegistry::records() const {
    std::vector<NodeRecord> result;
    result.reserve(nodes_.size());
    for (const auto& entry : nodes_) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const NodeRecord& left, const NodeRecord& right) {
                  return left.node_id < right.node_id;
              });
    return result;
}

const NodeRegistry::NodeClockEntry* NodeRegistry::find_clock_entry(
    std::uint32_t node_id, std::uint64_t boot_epoch) const noexcept {
    const auto found = clock_models_.find(node_id);
    if (found == clock_models_.end() ||
        found->second.boot_epoch != boot_epoch) {
        return nullptr;
    }
    return &found->second;
}

bool NodeRegistry::node_is_available(std::uint32_t node_id) const noexcept {
    const auto* node = find_by_node_id(node_id);
    return node != nullptr && node->online && node->assigned;
}

std::size_t NodeRegistry::UuidHash::operator()(
    const protocol::NodeUuid& uuid) const noexcept {
    std::size_t hash = 1469598103934665603ULL;
    for (const auto byte : uuid) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}
