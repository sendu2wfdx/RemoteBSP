#include "remotebsp/mock_mcu/transport_replay.hpp"
#include "remotebsp/transport/link_routes.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <unistd.h>

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
        if (event.sequence != index + 1U || event.repeat_count == 0U ||
            (index != 0U && event.at_ms < previous_time) ||
            event.node_id > 127U) {
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
                event.delay_ms != 0U || event.repeat_count != 1U) {
                throw std::invalid_argument("传输回放帧字段无效");
            }
        } else if (delay) {
            if (!directed || event.delay_ms == 0U ||
                event.delay_ms > kMaximumTransportReplayDelayMs ||
                event.route != 0U || !event.data.empty() ||
                event.node_id == 0U || event.repeat_count != 1U) {
                throw std::invalid_argument("传输延迟事件字段无效");
            }
        } else if (event.action == TransportReplayAction::DropNext ||
                   event.action == TransportReplayAction::DuplicateNext) {
            if (!directed || event.delay_ms != 0U || event.route != 0U ||
                !event.data.empty() || event.node_id == 0U ||
                event.repeat_count != 1U) {
                throw std::invalid_argument("传输故障事件包含多余字段");
            }
        } else if (event.action == TransportReplayAction::NodeReboot) {
            if (event.direction != TransportReplayDirection::None ||
                event.delay_ms != 0U || event.route != 0U ||
                !event.data.empty() || event.node_id == 0U ||
                event.repeat_count != 1U) {
                throw std::invalid_argument("节点重启事件包含多余字段");
            }
        } else if (event.action == TransportReplayAction::SendFailure) {
            if (!directed || event.route == 0U || event.data.empty() ||
                event.data.size() > kMaximumTransportReplayFrameBytes ||
                event.delay_ms != 0U || event.repeat_count != 1U) {
                throw std::invalid_argument("发送失败记录字段无效");
            }
        } else if (event.action == TransportReplayAction::ReceiveEmpty ||
                   event.action == TransportReplayAction::ReceiveFailure) {
            if (!directed || event.route != 0U || !event.data.empty() ||
                event.delay_ms > kMaximumTransportReplayDelayMs) {
                throw std::invalid_argument("接收结果记录字段无效");
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
        digest.add_u64(event.repeat_count);
    }
    digest.add_u64(record.dropped_frames);
    digest.add_u64(record.duplicated_frames);
    digest.add_u64(record.reboot_invalidated_frames);
    digest.add_u64(record.reboot_count);
    digest.add_u64(record.send_failures);
    digest.add_u64(record.receive_empty_results);
    digest.add_u64(record.receive_failures);
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
        left.send_failures != right.send_failures ||
        left.receive_empty_results != right.receive_empty_results ||
        left.receive_failures != right.receive_failures ||
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

std::string checksum_text(const std::string& value) {
    std::uint64_t digest = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0')
           << std::setw(16) << digest;
    return output.str();
}

std::string hex_text(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return "-";
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : data) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

std::vector<std::uint8_t> parse_hex(const std::string& text) {
    if (text == "-") return {};
    if (text.empty() || text.size() % 2U != 0U ||
        text.size() > kMaximumTransportReplayFrameBytes * 2U) {
        throw std::invalid_argument("传输会话十六进制载荷长度无效");
    }
    std::vector<std::uint8_t> result;
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0U; index < text.size(); index += 2U) {
        const auto nibble = [](char value) -> unsigned {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10U;
            throw std::invalid_argument("传输会话载荷必须是小写十六进制");
        };
        result.push_back(static_cast<std::uint8_t>(
            (nibble(text[index]) << 4U) | nibble(text[index + 1U])));
    }
    return result;
}

std::string session_body(const TransportReplaySession& session) {
    if (session.file_version != kTransportSessionFileVersion ||
        session.evidence_scope != "logical-link-boundary-only" ||
        session.time_base != "monotonic-relative-ms") {
        throw std::invalid_argument("传输会话文件身份或证据范围无效");
    }
    verify_transport_replay(session.events, session.record);
    std::ostringstream output;
    output << "REMOTEBSP_TRANSPORT_SESSION " << session.file_version << '\n'
           << "evidence_scope " << session.evidence_scope << '\n'
           << "time_base " << session.time_base << '\n'
           << "event_count " << session.events.size() << '\n';
    for (const auto& event : session.events) {
        output << "event " << event.sequence << ' ' << event.at_ms << ' '
               << event.node_id << ' '
               << static_cast<unsigned>(event.action) << ' '
               << static_cast<unsigned>(event.direction) << ' '
               << event.repeat_count << ' ' << event.delay_ms << ' '
               << event.route << ' ' << hex_text(event.data) << '\n';
    }
    const auto& record = session.record;
    output << "summary " << record.dropped_frames << ' '
           << record.duplicated_frames << ' '
           << record.reboot_invalidated_frames << ' '
           << record.reboot_count << ' ' << record.send_failures << ' '
           << record.receive_empty_results << ' '
           << record.receive_failures << ' ' << record.replay_digest << '\n';
    return output.str();
}

void require_line_end(std::istringstream& input, const char* label) {
    std::string extra;
    if (input >> extra) {
        throw std::invalid_argument(std::string(label) + "包含多余字段");
    }
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
    std::uint32_t send_failures = 0U;
    std::uint32_t receive_empty_results = 0U;
    std::uint32_t receive_failures = 0U;
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
        node.used = event.node_id != 0U || node.used;
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
        } else if (event.action == TransportReplayAction::Frame) {
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
        } else if (event.action == TransportReplayAction::SendFailure) {
            if (event.repeat_count >
                std::numeric_limits<std::uint32_t>::max() - send_failures) {
                throw std::invalid_argument("发送失败计数溢出");
            }
            send_failures += event.repeat_count;
        } else if (event.action == TransportReplayAction::ReceiveEmpty) {
            if (event.repeat_count > std::numeric_limits<std::uint32_t>::max() -
                                         receive_empty_results) {
                throw std::invalid_argument("接收空结果计数溢出");
            }
            receive_empty_results += event.repeat_count;
        } else {
            if (event.repeat_count >
                std::numeric_limits<std::uint32_t>::max() - receive_failures) {
                throw std::invalid_argument("接收失败计数溢出");
            }
            receive_failures += event.repeat_count;
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
    record.send_failures = send_failures;
    record.receive_empty_results = receive_empty_results;
    record.receive_failures = receive_failures;
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

TransportReplaySession make_transport_replay_session(
    const std::vector<TransportReplayEvent>& events) {
    TransportReplaySession session;
    session.events = events;
    session.record = run_transport_replay(events);
    const auto body = session_body(session);
    session.file_checksum = checksum_text(body);
    return session;
}

std::string encode_transport_replay_session(
    const TransportReplaySession& session) {
    const auto body = session_body(session);
    const auto checksum = checksum_text(body);
    if (!session.file_checksum.empty() && session.file_checksum != checksum) {
        throw std::invalid_argument("传输会话文件校验和不一致");
    }
    const auto encoded = body + "checksum " + checksum + '\n';
    if (encoded.size() > kMaximumTransportSessionFileBytes) {
        throw std::invalid_argument("传输会话文件超过16 MiB上限");
    }
    return encoded;
}

TransportReplaySession parse_transport_replay_session(
    const std::string& text) {
    if (text.size() > kMaximumTransportSessionFileBytes || text.empty()) {
        throw std::invalid_argument("传输会话文件为空或超过16 MiB上限");
    }
    std::istringstream lines(text);
    std::string line;
    unsigned long long version = 0U;
    if (!std::getline(lines, line)) {
        throw std::invalid_argument("传输会话文件头缺失");
    }
    {
        std::istringstream input(line);
        std::string magic;
        if (!(input >> magic >> version) ||
            magic != "REMOTEBSP_TRANSPORT_SESSION" ||
            version != kTransportSessionFileVersion) {
            throw std::invalid_argument("传输会话文件版本无效");
        }
        require_line_end(input, "传输会话文件头");
    }
    const auto read_value = [&](const char* expected) {
        if (!std::getline(lines, line)) {
            throw std::invalid_argument("传输会话元数据缺失");
        }
        std::istringstream input(line);
        std::string name;
        std::string value;
        if (!(input >> name >> value) || name != expected) {
            throw std::invalid_argument("传输会话元数据字段无效");
        }
        require_line_end(input, "传输会话元数据");
        return value;
    };
    const auto scope = read_value("evidence_scope");
    const auto time_base = read_value("time_base");
    const auto count_text = read_value("event_count");
    std::size_t parsed_count = 0U;
    try {
        std::size_t consumed = 0U;
        const auto count = std::stoull(count_text, &consumed, 10);
        if (consumed != count_text.size() ||
            count > kMaximumTransportReplayEvents) {
            throw std::invalid_argument("count");
        }
        parsed_count = static_cast<std::size_t>(count);
    } catch (const std::exception&) {
        throw std::invalid_argument("传输会话事件数量无效");
    }
    std::vector<TransportReplayEvent> events;
    events.reserve(parsed_count);
    for (std::size_t index = 0U; index < parsed_count; ++index) {
        if (!std::getline(lines, line)) {
            throw std::invalid_argument("传输会话事件缺失");
        }
        std::istringstream input(line);
        std::string name;
        unsigned long long sequence, at_ms, node_id, action_value,
            direction_value, repeat_count, delay_ms, route;
        std::string payload;
        if (!(input >> name >> sequence >> at_ms >> node_id >> action_value >>
              direction_value >> repeat_count >> delay_ms >> route >>
              payload) || name != "event" ||
            sequence > std::numeric_limits<std::uint32_t>::max() ||
            node_id > std::numeric_limits<std::uint32_t>::max() ||
            action_value > static_cast<unsigned>(
                               TransportReplayAction::ReceiveFailure) ||
            direction_value > static_cast<unsigned>(
                                  TransportReplayDirection::NodeToHost) ||
            repeat_count > std::numeric_limits<std::uint32_t>::max() ||
            route > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("传输会话事件字段无效");
        }
        require_line_end(input, "传输会话事件");
        events.push_back({
            static_cast<std::uint32_t>(sequence), at_ms,
            static_cast<std::uint32_t>(node_id),
            static_cast<TransportReplayAction>(action_value),
            static_cast<TransportReplayDirection>(direction_value),
            delay_ms, static_cast<std::uint32_t>(route), parse_hex(payload),
            static_cast<std::uint32_t>(repeat_count)});
    }
    if (!std::getline(lines, line)) {
        throw std::invalid_argument("传输会话摘要缺失");
    }
    unsigned long long dropped, duplicated, invalidated, reboots,
        send_failures, receive_empty, receive_failures;
    std::string summary_name;
    std::string replay_digest;
    {
        std::istringstream input(line);
        if (!(input >> summary_name >> dropped >> duplicated >> invalidated >>
              reboots >> send_failures >> receive_empty >> receive_failures >>
              replay_digest) || summary_name != "summary") {
            throw std::invalid_argument("传输会话摘要字段无效");
        }
        require_line_end(input, "传输会话摘要");
    }
    const auto checksum = read_value("checksum");
    if (std::getline(lines, line)) {
        throw std::invalid_argument("传输会话文件包含尾随内容");
    }
    auto session = make_transport_replay_session(events);
    if (scope != session.evidence_scope || time_base != session.time_base ||
        dropped != session.record.dropped_frames ||
        duplicated != session.record.duplicated_frames ||
        invalidated != session.record.reboot_invalidated_frames ||
        reboots != session.record.reboot_count ||
        send_failures != session.record.send_failures ||
        receive_empty != session.record.receive_empty_results ||
        receive_failures != session.record.receive_failures ||
        replay_digest != session.record.replay_digest ||
        checksum != session.file_checksum ||
        encode_transport_replay_session(session) != text) {
        throw std::invalid_argument("传输会话摘要、校验和或规范编码不一致");
    }
    return session;
}

void write_transport_replay_session(
    const TransportReplaySession& session, const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("传输会话文件路径不能为空");
    }
    const auto encoded = encode_transport_replay_session(session);
    auto temporary_text = path + ".tmp.XXXXXX";
    std::vector<char> temporary_template(
        temporary_text.begin(), temporary_text.end());
    temporary_template.push_back('\0');
    const int descriptor = ::mkstemp(temporary_template.data());
    if (descriptor < 0) {
        throw std::runtime_error("无法创建唯一传输会话临时文件");
    }
    const std::string temporary(temporary_template.data());
    std::FILE* output = ::fdopen(descriptor, "wb");
    if (output == nullptr) {
        ::close(descriptor);
        ::unlink(temporary.c_str());
        throw std::runtime_error("无法打开传输会话临时文件");
    }
    const auto written = std::fwrite(
        encoded.data(), 1U, encoded.size(), output);
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (written != encoded.size() || !flushed || !closed) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("写入传输会话文件失败");
    }
    if (::link(temporary.c_str(), path.c_str()) != 0) {
        const bool exists = errno == EEXIST;
        const bool cleanup_failed = ::unlink(temporary.c_str()) != 0;
        if (cleanup_failed) {
            throw std::runtime_error(
                exists
                    ? "传输会话目标文件已存在，且临时文件清理失败"
                    : "原子创建传输会话目标文件失败，且临时文件清理失败");
        }
        throw std::runtime_error(
            exists ? "传输会话目标文件已存在（no-clobber）"
                   : "原子创建传输会话目标文件失败");
    }
    if (::unlink(temporary.c_str()) != 0) {
        throw std::runtime_error(
            "传输会话目标文件已创建，但临时硬链接清理失败");
    }
}

TransportReplaySession load_transport_replay_session(
    const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("无法读取传输会话文件");
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) >
                        kMaximumTransportSessionFileBytes) {
        throw std::invalid_argument("传输会话文件超过16 MiB上限");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    if (!text.empty() &&
        !input.read(text.data(), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("读取传输会话文件失败");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("读取期间传输会话文件长度发生变化");
    }
    return parse_transport_replay_session(text);
}

TransportSessionRecorder::TransportSessionRecorder(TransportReplayClock clock)
    : clock_(std::move(clock)), origin_(std::chrono::steady_clock::now()) {
    events_.reserve(kMaximumTransportReplayEvents);
}

std::uint64_t TransportSessionRecorder::now_ms() const {
    if (clock_) return clock_();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - origin_).count());
}

void TransportSessionRecorder::append(TransportReplayEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        if (capture_failure_ != CaptureFailure::None) {
            throw std::runtime_error("传输会话采集已经停止");
        }
        const auto at_ms = now_ms();
        if (at_ms < last_at_ms_) {
            throw std::runtime_error("传输会话时钟发生倒退");
        }
        event.sequence = static_cast<std::uint32_t>(events_.size() + 1U);
        event.at_ms = at_ms;
        auto isolated = event;
        isolated.sequence = 1U;
        validate_events({isolated});
        if (event.action == TransportReplayAction::ReceiveEmpty &&
            !events_.empty()) {
            auto& previous = events_.back();
            if (previous.action == event.action &&
                previous.direction == event.direction &&
                previous.node_id == event.node_id &&
                previous.delay_ms == event.delay_ms &&
                previous.at_ms == at_ms &&
                previous.repeat_count !=
                    std::numeric_limits<std::uint32_t>::max()) {
                ++previous.repeat_count;
                last_at_ms_ = at_ms;
                return;
            }
        }
        if (events_.size() == kMaximumTransportReplayEvents ||
            event.data.size() > kMaximumReplayPayloadBytes - payload_bytes_ -
                                    reserved_payload_bytes_) {
            throw std::runtime_error("传输会话记录容量耗尽");
        }
        payload_bytes_ += event.data.size();
        last_at_ms_ = at_ms;
        events_.push_back(std::move(event));
    } catch (const std::exception& error) {
        static_cast<void>(error);
        if (capture_failure_ == CaptureFailure::None) {
            capture_failure_ = CaptureFailure::InvalidObservation;
        }
        throw;
    } catch (...) {
        if (capture_failure_ == CaptureFailure::None) {
            capture_failure_ = CaptureFailure::Internal;
        }
        throw;
    }
}

void TransportSessionRecorder::record_frame(
    TransportReplayDirection direction, std::uint32_t node_id,
    const transport::LinkFrame& frame) {
    append({0U, 0U, node_id, TransportReplayAction::Frame,
            direction, 0U, frame.route, frame.data});
}

void TransportSessionRecorder::record_send_failure(
    TransportReplayDirection direction, std::uint32_t node_id,
    const transport::LinkFrame& frame) {
    append({0U, 0U, node_id, TransportReplayAction::SendFailure,
            direction, 0U, frame.route, frame.data});
}

void TransportSessionRecorder::record_receive_empty(
    TransportReplayDirection direction, std::uint32_t node_id,
    std::chrono::milliseconds timeout) {
    append({0U, 0U, node_id, TransportReplayAction::ReceiveEmpty,
            direction, static_cast<std::uint64_t>(timeout.count()), 0U, {}});
}

void TransportSessionRecorder::record_receive_failure(
    TransportReplayDirection direction, std::uint32_t node_id,
    std::chrono::milliseconds timeout) {
    append({0U, 0U, node_id, TransportReplayAction::ReceiveFailure,
            direction, static_cast<std::uint64_t>(timeout.count()), 0U, {}});
}

void TransportSessionRecorder::record_fault(
    std::uint32_t node_id, TransportReplayAction action,
    TransportReplayDirection direction, std::uint64_t delay_ms) {
    append({0U, 0U, node_id, action, direction, delay_ms, 0U, {}});
}

void TransportSessionRecorder::record_reboot(std::uint32_t node_id) {
    append({0U, 0U, node_id, TransportReplayAction::NodeReboot,
            TransportReplayDirection::None, 0U, 0U, {}});
}

TransportSessionRecorder::ObservationReservation
TransportSessionRecorder::reserve_send(
    TransportReplayDirection direction, std::uint32_t node_id,
    const transport::LinkFrame& frame) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_failure_ != CaptureFailure::None) return {};
        if ((direction != TransportReplayDirection::HostToNode &&
             direction != TransportReplayDirection::NodeToHost) ||
            node_id > transport::kMaximumNodeId || frame.route == 0U ||
            frame.data.empty() ||
            frame.data.size() > kMaximumTransportReplayFrameBytes) {
            capture_failure_ = CaptureFailure::InvalidObservation;
            return {};
        }
        const auto at_ms = now_ms();
        if (at_ms < last_at_ms_) {
            capture_failure_ = CaptureFailure::ClockRollback;
            return {};
        }
        if (events_.size() == kMaximumTransportReplayEvents ||
            frame.data.size() > kMaximumReplayPayloadBytes -
                                    payload_bytes_ -
                                    reserved_payload_bytes_) {
            capture_failure_ = CaptureFailure::Capacity;
            return {};
        }
        TransportReplayEvent event{
            static_cast<std::uint32_t>(events_.size() + 1U), at_ms,
            node_id, TransportReplayAction::Frame, direction, 0U,
            frame.route, frame.data};
        const auto index = events_.size();
        events_.push_back(std::move(event));
        payload_bytes_ += frame.data.size();
        ++pending_observations_;
        last_at_ms_ = at_ms;
        return {index, 0U};
    } catch (const std::invalid_argument&) {
        capture_failure_ = CaptureFailure::InvalidObservation;
    } catch (const std::bad_alloc&) {
        capture_failure_ = CaptureFailure::Capacity;
    } catch (...) {
        capture_failure_ = CaptureFailure::Internal;
    }
    return {};
}

TransportSessionRecorder::ObservationReservation
TransportSessionRecorder::reserve_receive(
    TransportReplayDirection direction, std::uint32_t node_id,
    std::chrono::milliseconds timeout) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_failure_ != CaptureFailure::None) return {};
        const auto at_ms = now_ms();
        if (at_ms < last_at_ms_) {
            capture_failure_ = CaptureFailure::ClockRollback;
            return {};
        }
        TransportReplayEvent event{
            static_cast<std::uint32_t>(events_.size() + 1U), at_ms,
            node_id, TransportReplayAction::ReceiveEmpty, direction,
            static_cast<std::uint64_t>(timeout.count()), 0U, {}};
        auto isolated = event;
        isolated.sequence = 1U;
        validate_events({isolated});
        if (events_.size() == kMaximumTransportReplayEvents ||
            kMaximumTransportReplayFrameBytes >
                kMaximumReplayPayloadBytes - payload_bytes_ -
                    reserved_payload_bytes_) {
            capture_failure_ = CaptureFailure::Capacity;
            return {};
        }
        event.data.reserve(kMaximumTransportReplayFrameBytes);
        const auto index = events_.size();
        reserved_payload_bytes_ += kMaximumTransportReplayFrameBytes;
        ++pending_observations_;
        last_at_ms_ = at_ms;
        events_.push_back(std::move(event));
        return {index, kMaximumTransportReplayFrameBytes};
    } catch (const std::invalid_argument&) {
        capture_failure_ = CaptureFailure::InvalidObservation;
    } catch (const std::bad_alloc&) {
        capture_failure_ = CaptureFailure::Capacity;
    } catch (...) {
        capture_failure_ = CaptureFailure::Internal;
    }
    return {};
}

void TransportSessionRecorder::commit_send(
    ObservationReservation reservation, bool succeeded) noexcept {
    if (!reservation.active()) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reservation.index >= events_.size() ||
            pending_observations_ == 0U) {
            capture_failure_ = CaptureFailure::Internal;
            return;
        }
        events_[reservation.index].action =
            succeeded ? TransportReplayAction::Frame
                      : TransportReplayAction::SendFailure;
        --pending_observations_;
    } catch (...) {
        capture_failure_ = CaptureFailure::Internal;
    }
}

void TransportSessionRecorder::commit_receive(
    ObservationReservation reservation, const transport::LinkFrame* frame,
    std::uint32_t node_id, bool failed) noexcept {
    if (!reservation.active()) return;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reservation.index >= events_.size() ||
            pending_observations_ == 0U ||
            reservation.reserved_payload_bytes > reserved_payload_bytes_) {
            capture_failure_ = CaptureFailure::Internal;
            return;
        }
        reserved_payload_bytes_ -= reservation.reserved_payload_bytes;
        --pending_observations_;
        auto& event = events_[reservation.index];
        if (failed) {
            event.action = TransportReplayAction::ReceiveFailure;
            return;
        }
        if (frame == nullptr) {
            event.action = TransportReplayAction::ReceiveEmpty;
            return;
        }
        if (node_id > 127U || frame->route == 0U || frame->data.empty() ||
            frame->data.size() > kMaximumTransportReplayFrameBytes) {
            capture_failure_ = CaptureFailure::InvalidObservation;
            return;
        }
        event.action = TransportReplayAction::Frame;
        event.node_id = node_id;
        event.delay_ms = 0U;
        event.route = frame->route;
        event.data.assign(frame->data.begin(), frame->data.end());
        payload_bytes_ += frame->data.size();
    } catch (...) {
        capture_failure_ = CaptureFailure::Internal;
    }
}

TransportReplaySession TransportSessionRecorder::snapshot() const {
    std::vector<TransportReplayEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_observations_ != 0U) {
            throw std::runtime_error("传输会话仍有未完成的 I/O 观察");
        }
        if (capture_failure_ != CaptureFailure::None) {
            const char* reason = "内部错误";
            if (capture_failure_ == CaptureFailure::ClockRollback) {
                reason = "单调时钟倒退";
            } else if (capture_failure_ == CaptureFailure::Capacity) {
                reason = "记录容量耗尽";
            } else if (capture_failure_ ==
                       CaptureFailure::InvalidObservation) {
                reason = "观察字段无效";
            }
            throw std::runtime_error(
                std::string("传输会话采集不完整: ") + reason);
        }
        events = events_;
    }
    return make_transport_replay_session(events);
}

RecordingLinkTransport::RecordingLinkTransport(
    std::unique_ptr<transport::LinkTransport> inner,
    RecordingTransportRole role,
    std::shared_ptr<TransportSessionRecorder> recorder)
    : inner_(std::move(inner)), role_(role), recorder_(std::move(recorder)) {
    if (!inner_ || !recorder_ ||
        (role_ != RecordingTransportRole::Host &&
         role_ != RecordingTransportRole::Node)) {
        throw std::invalid_argument("传输会话录制适配器参数无效");
    }
}

std::uint32_t RecordingLinkTransport::resolve_node(
    TransportReplayDirection direction,
    const transport::LinkFrame* frame) const noexcept {
    if (frame == nullptr) return 0U;
    const auto route = frame->route;
    if (direction == TransportReplayDirection::HostToNode &&
        route > transport::kNodeRequestBaseRoute &&
        route <= transport::kNodeRequestBaseRoute +
                     transport::kMaximumNodeId) {
        return route - transport::kNodeRequestBaseRoute;
    }
    if (direction == TransportReplayDirection::NodeToHost) {
        const std::array<std::uint32_t, 2U> bases{
            transport::kNodeResponseBaseRoute,
            transport::kNodeEventBaseRoute};
        for (const auto base : bases) {
            if (route > base && route <= base + transport::kMaximumNodeId) {
                return route - base;
            }
        }
    }
    return 0U;
}

void RecordingLinkTransport::send(const transport::LinkFrame& frame) {
    const auto direction = role_ == RecordingTransportRole::Host
                               ? TransportReplayDirection::HostToNode
                               : TransportReplayDirection::NodeToHost;
    const auto node_id = resolve_node(direction, &frame);
    const auto reservation =
        recorder_->reserve_send(direction, node_id, frame);
    try {
        inner_->send(frame);
    } catch (...) {
        const auto failure = std::current_exception();
        recorder_->commit_send(reservation, false);
        std::rethrow_exception(failure);
    }
    recorder_->commit_send(reservation, true);
}

std::optional<transport::LinkFrame> RecordingLinkTransport::receive(
    std::chrono::milliseconds timeout) {
    const auto direction = role_ == RecordingTransportRole::Host
                               ? TransportReplayDirection::NodeToHost
                               : TransportReplayDirection::HostToNode;
    if (timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("传输会话接收超时不能为负数");
    }
    const auto reservation = recorder_->reserve_receive(
        direction, 0U, timeout);
    std::optional<transport::LinkFrame> frame;
    try {
        frame = inner_->receive(timeout);
    } catch (...) {
        const auto failure = std::current_exception();
        recorder_->commit_receive(reservation, nullptr, 0U, true);
        std::rethrow_exception(failure);
    }
    recorder_->commit_receive(
        reservation, frame.has_value() ? &*frame : nullptr,
        frame.has_value() ? resolve_node(direction, &*frame) : 0U, false);
    return frame;
}

transport::LinkCapabilities RecordingLinkTransport::capabilities()
    const noexcept {
    return inner_->capabilities();
}

std::optional<NodeReply> deliver_replayed_frame_to_mock_node(
    MockNode& node, const TransportReplayDelivery& delivery,
    protocol::Reassembler::TimePoint origin) {
    const bool broadcast =
        delivery.node_id == 0U &&
        delivery.route == transport::kDiscoveryRoute;
    const bool addressed =
        delivery.node_id == node.node_id() && node.node_id() != 0U &&
        delivery.route ==
            transport::kNodeRequestBaseRoute + node.node_id();
    if (delivery.direction != TransportReplayDirection::HostToNode ||
        (!broadcast && !addressed) ||
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
