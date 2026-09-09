#include "remotebsp/mock_mcu/transport_replay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace remotebsp::mock_mcu {
namespace {

constexpr std::size_t kMaximumReplayPayloadBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kMaximumDuplicatesPerFrame = 7U;

struct DirectionEffect {
    std::uint64_t delay_ms{};
    std::uint32_t duplicate_count{};
    bool drop{};
};

struct NodeState {
    std::array<DirectionEffect, 2U> effects{};
    std::uint32_t generation{1U};
    bool used{};
};

std::size_t direction_index(TransportReplayDirection direction) {
    return direction == TransportReplayDirection::HostToNode ? 0U : 1U;
}

class Digest {
public:
    void add_u64(std::uint64_t value) noexcept {
        for (unsigned index = 0; index < 8U; ++index) {
            add(static_cast<std::uint8_t>(value >> (index * 8U)));
        }
    }
    void add_bytes(const std::vector<std::uint8_t>& bytes) noexcept {
        add_u64(bytes.size());
        for (const auto byte : bytes) add(byte);
    }
    std::string text() const {
        std::ostringstream output;
        output << "fnv1a64:" << std::hex << std::setfill('0')
               << std::setw(16) << value_;
        return output.str();
    }

private:
    void add(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }
    std::uint64_t value_{14695981039346656037ULL};
};

void validate_events(const std::vector<TransportReplayEvent>& events) {
    if (events.size() > kMaximumTransportReplayEvents) {
        throw std::invalid_argument("传输回放事件数量超过上限");
    }
    std::size_t payload_bytes = 0U;
    std::uint64_t previous_time = 0U;
    for (std::size_t index = 0U; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.sequence != index + 1U ||
            (index != 0U && event.at_ms < previous_time) ||
            event.node_id == 0U || event.node_id > 127U) {
            throw std::invalid_argument("传输回放顺序、时间或节点无效");
        }
        previous_time = event.at_ms;
        if (event.data.size() >
            kMaximumReplayPayloadBytes - payload_bytes) {
            throw std::invalid_argument("传输回放载荷总量超过上限");
        }
        payload_bytes += event.data.size();
        const bool frame = event.action == TransportReplayAction::Frame;
        const bool delay = event.action == TransportReplayAction::DelayNext;
        const bool directed =
            event.direction == TransportReplayDirection::HostToNode ||
            event.direction == TransportReplayDirection::NodeToHost;
        if (frame) {
            if (!directed || event.route == 0U || event.data.empty() ||
                event.data.size() > kMaximumTransportReplayFrameBytes ||
                event.delay_ms != 0U) {
                throw std::invalid_argument("传输回放帧字段无效");
            }
        } else if (delay) {
            if (!directed || event.delay_ms == 0U ||
                event.delay_ms > kMaximumTransportReplayDelayMs ||
                event.route != 0U || !event.data.empty()) {
                throw std::invalid_argument("传输延迟事件字段无效");
            }
        } else if (event.action == TransportReplayAction::DropNext ||
                   event.action == TransportReplayAction::DuplicateNext) {
            if (!directed || event.delay_ms != 0U || event.route != 0U ||
                !event.data.empty()) {
                throw std::invalid_argument("传输故障事件包含多余字段");
            }
        } else if (event.action == TransportReplayAction::NodeReboot) {
            if (event.direction != TransportReplayDirection::None ||
                event.delay_ms != 0U || event.route != 0U ||
                !event.data.empty()) {
                throw std::invalid_argument("节点重启事件包含多余字段");
            }
        } else {
            throw std::invalid_argument("传输回放动作未知");
        }
    }
}

std::string calculate_digest(
    const TransportReplayRecord& record,
    const std::vector<TransportReplayEvent>& events) {
    Digest digest;
    digest.add_u64(record.schema_version);
    digest.add_u64(record.event_count);
    for (const auto& event : events) {
        digest.add_u64(event.sequence);
        digest.add_u64(event.at_ms);
        digest.add_u64(event.node_id);
        digest.add_u64(static_cast<std::uint8_t>(event.action));
        digest.add_u64(static_cast<std::uint8_t>(event.direction));
        digest.add_u64(event.delay_ms);
        digest.add_u64(event.route);
        digest.add_bytes(event.data);
    }
    digest.add_u64(record.dropped_frames);
    digest.add_u64(record.duplicated_frames);
    digest.add_u64(record.reboot_invalidated_frames);
    digest.add_u64(record.reboot_count);
    for (const auto& delivery : record.deliveries) {
        digest.add_u64(delivery.delivery_sequence);
        digest.add_u64(delivery.source_sequence);
        digest.add_u64(delivery.at_ms);
        digest.add_u64(delivery.node_id);
        digest.add_u64(delivery.session_generation);
        digest.add_u64(static_cast<std::uint8_t>(delivery.direction));
        digest.add_u64(delivery.route);
        digest.add_bytes(delivery.data);
    }
    for (const auto& generation : record.final_generations) {
        digest.add_u64(generation.node_id);
        digest.add_u64(generation.session_generation);
    }
    return digest.text();
}

bool records_equal(const TransportReplayRecord& left,
                   const TransportReplayRecord& right) {
    if (left.schema_version != right.schema_version ||
        left.event_count != right.event_count ||
        left.dropped_frames != right.dropped_frames ||
        left.duplicated_frames != right.duplicated_frames ||
        left.reboot_invalidated_frames !=
            right.reboot_invalidated_frames ||
        left.reboot_count != right.reboot_count ||
        left.replay_digest != right.replay_digest ||
        left.deliveries.size() != right.deliveries.size() ||
        left.final_generations.size() != right.final_generations.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.deliveries.size(); ++index) {
        const auto& a = left.deliveries[index];
        const auto& b = right.deliveries[index];
        if (a.delivery_sequence != b.delivery_sequence ||
            a.source_sequence != b.source_sequence || a.at_ms != b.at_ms ||
            a.node_id != b.node_id ||
            a.session_generation != b.session_generation ||
            a.direction != b.direction ||
            a.route != b.route || a.data != b.data) {
            return false;
        }
    }
    for (std::size_t index = 0U;
         index < left.final_generations.size(); ++index) {
        if (left.final_generations[index].node_id !=
                right.final_generations[index].node_id ||
            left.final_generations[index].session_generation !=
                right.final_generations[index].session_generation) {
            return false;
        }
    }
    return true;
}

}  // namespace

TransportReplayRecord run_transport_replay(
    const std::vector<TransportReplayEvent>& events) {
    validate_events(events);
    std::array<NodeState, 128U> nodes{};
    std::vector<TransportReplayDelivery> pending;
    std::vector<TransportReplayDelivery> delivered;
    pending.reserve(kMaximumTransportReplayPendingFrames);
    delivered.reserve(kMaximumTransportReplayPendingFrames);
    std::uint32_t dropped = 0U;
    std::uint32_t duplicated = 0U;
    std::uint32_t reboot_invalidated = 0U;
    std::uint32_t reboots = 0U;
    std::uint32_t next_delivery_sequence = 1U;

    const auto flush = [&](std::uint64_t at_ms, bool all) {
        std::stable_sort(pending.begin(), pending.end(),
                         [](const auto& left, const auto& right) {
                             if (left.at_ms != right.at_ms) {
                                 return left.at_ms < right.at_ms;
                             }
                             return left.delivery_sequence <
                                    right.delivery_sequence;
                         });
        auto split = all ? pending.end() : std::upper_bound(
            pending.begin(), pending.end(), at_ms,
            [](std::uint64_t time, const auto& item) {
                return time < item.at_ms;
            });
        delivered.insert(delivered.end(),
                         std::make_move_iterator(pending.begin()),
                         std::make_move_iterator(split));
        pending.erase(pending.begin(), split);
    };

    for (const auto& event : events) {
        flush(event.at_ms, false);
        auto& node = nodes[event.node_id];
        node.used = true;
        if (event.action == TransportReplayAction::DelayNext) {
            auto& effect = node.effects[direction_index(event.direction)];
            if (effect.delay_ms > kMaximumTransportReplayDelayMs -
                                      event.delay_ms) {
                throw std::invalid_argument("同节点累计延迟超过上限");
            }
            effect.delay_ms += event.delay_ms;
        } else if (event.action == TransportReplayAction::DropNext) {
            node.effects[direction_index(event.direction)].drop = true;
        } else if (event.action == TransportReplayAction::DuplicateNext) {
            auto& effect = node.effects[direction_index(event.direction)];
            if (effect.duplicate_count == kMaximumDuplicatesPerFrame) {
                throw std::invalid_argument("同一帧重复次数超过上限");
            }
            ++effect.duplicate_count;
        } else if (event.action == TransportReplayAction::NodeReboot) {
            const auto old_size = pending.size();
            pending.erase(std::remove_if(
                pending.begin(), pending.end(), [&](const auto& item) {
                    return item.node_id == event.node_id;
                }), pending.end());
            reboot_invalidated +=
                static_cast<std::uint32_t>(old_size - pending.size());
            node.effects = {};
            if (node.generation == std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("节点会话代次已经耗尽");
            }
            ++node.generation;
            ++reboots;
        } else {
            auto& effect = node.effects[direction_index(event.direction)];
            const auto copies =
                effect.drop ? 0U : effect.duplicate_count + 1U;
            if (pending.size() + copies >
                kMaximumTransportReplayPendingFrames) {
                throw std::invalid_argument("传输回放待交付队列达到上限");
            }
            if (delivered.size() + pending.size() + copies >
                kMaximumTransportReplayDeliveries) {
                throw std::invalid_argument("传输回放交付总量达到上限");
            }
            if (event.at_ms >
                std::numeric_limits<std::uint64_t>::max() - effect.delay_ms) {
                throw std::invalid_argument("传输回放交付时间溢出");
            }
            if (effect.drop) {
                ++dropped;
            } else {
                duplicated += effect.duplicate_count;
                for (std::uint32_t copy = 0U; copy < copies; ++copy) {
                    pending.push_back({
                        next_delivery_sequence++, event.sequence,
                        event.at_ms + effect.delay_ms, event.node_id,
                        node.generation, event.direction, event.route,
                        event.data});
                }
            }
            effect = {};
        }
    }
    flush(0U, true);
    for (std::size_t index = 0U; index < delivered.size(); ++index) {
        delivered[index].delivery_sequence =
            static_cast<std::uint32_t>(index + 1U);
    }

    TransportReplayRecord record;
    record.event_count = static_cast<std::uint32_t>(events.size());
    record.dropped_frames = dropped;
    record.duplicated_frames = duplicated;
    record.reboot_invalidated_frames = reboot_invalidated;
    record.reboot_count = reboots;
    record.deliveries = std::move(delivered);
    for (std::uint32_t node_id = 1U; node_id <= 127U; ++node_id) {
        if (nodes[node_id].used) {
            record.final_generations.push_back(
                {node_id, nodes[node_id].generation});
        }
    }
    record.replay_digest = calculate_digest(record, events);
    return record;
}

void verify_transport_replay(
    const std::vector<TransportReplayEvent>& events,
    const TransportReplayRecord& expected) {
    validate_events(events);
    if (expected.schema_version != kTransportReplaySchemaVersion) {
        throw std::invalid_argument("传输回放记录版本不受支持");
    }
    if (expected.event_count > kMaximumTransportReplayEvents ||
        expected.deliveries.size() > kMaximumTransportReplayDeliveries ||
        expected.final_generations.size() > 127U ||
        calculate_digest(expected, events) != expected.replay_digest) {
        throw std::invalid_argument("传输回放记录结构或摘要无效");
    }
    const auto actual = run_transport_replay(events);
    if (!records_equal(actual, expected)) {
        throw std::invalid_argument("传输回放摘要或计数不一致");
    }
}

std::optional<NodeReply> deliver_replayed_frame_to_mock_node(
    MockNode& node, const TransportReplayDelivery& delivery,
    protocol::Reassembler::TimePoint origin) {
    if (delivery.direction != TransportReplayDirection::HostToNode ||
        delivery.node_id != node.node_id() || delivery.route == 0U ||
        delivery.data.empty()) {
        throw std::invalid_argument("逻辑交付不适用于目标 MockNode H2N 入口");
    }
    using Duration = protocol::Reassembler::Clock::duration;
    const auto maximum_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(Duration::max()).count();
    if (delivery.at_ms > static_cast<std::uint64_t>(maximum_ms)) {
        throw std::invalid_argument("逻辑交付时间超出 Mock 时钟范围");
    }
    return node.handle_frame(
        delivery.route, delivery.data,
        origin + std::chrono::milliseconds(delivery.at_ms));
}

std::vector<TransportReplayEvent> make_mock_node_reply_replay_events(
    std::uint32_t first_sequence, std::uint64_t at_ms,
    std::uint32_t node_id, std::uint32_t route, const NodeReply& reply) {
    if (first_sequence == 0U || node_id == 0U || node_id > 127U ||
        route == 0U || reply.frames.empty() ||
        reply.frames.size() > kMaximumTransportReplayEvents ||
        reply.frames.size() - 1U >
            std::numeric_limits<std::uint32_t>::max() - first_sequence) {
        throw std::invalid_argument("MockNode N2H 回放事件参数无效");
    }
    std::vector<TransportReplayEvent> events;
    events.reserve(reply.frames.size());
    for (std::size_t index = 0U; index < reply.frames.size(); ++index) {
        const auto& data = reply.frames[index];
        if (data.empty() || data.size() > kMaximumTransportReplayFrameBytes) {
            throw std::invalid_argument("MockNode N2H 帧大小无效");
        }
        events.push_back({
            static_cast<std::uint32_t>(first_sequence + index), at_ms,
            node_id, TransportReplayAction::Frame,
            TransportReplayDirection::NodeToHost, 0U, route, data});
    }
    return events;
}

}  // namespace remotebsp::mock_mcu
