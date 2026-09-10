#include "remotebsp/mock_mcu/transport_replay.hpp"
#include "remotebsp/transport/link_routes.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "检查失败：%s（%s:%d）\n", #condition,      \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

using remotebsp::mock_mcu::TransportReplayAction;
using remotebsp::mock_mcu::TransportReplayDirection;
using remotebsp::mock_mcu::TransportReplayEvent;

class ScriptedTransport final : public remotebsp::transport::LinkTransport {
public:
    void send(const remotebsp::transport::LinkFrame& frame) override {
        if (on_send) on_send();
        if (fail_send) throw std::runtime_error("模拟发送失败");
        sent.push_back(frame);
    }

    std::optional<remotebsp::transport::LinkFrame> receive(
        std::chrono::milliseconds) override {
        ++receive_calls;
        if (fail_receive) throw std::runtime_error("模拟接收失败");
        if (received.empty()) return std::nullopt;
        auto frame = std::move(received.front());
        received.pop_front();
        return frame;
    }

    remotebsp::transport::LinkCapabilities capabilities()
        const noexcept override {
        return {remotebsp::transport::LinkKind::MockUsb, 64U, true, true,
                true, 0U};
    }

    bool fail_send{};
    bool fail_receive{};
    std::size_t receive_calls{};
    std::function<void()> on_send;
    std::vector<remotebsp::transport::LinkFrame> sent;
    std::deque<remotebsp::transport::LinkFrame> received;
};

TransportReplayEvent action(std::uint32_t sequence, std::uint64_t at_ms,
                            std::uint32_t node_id,
                            TransportReplayAction kind,
                            std::uint64_t delay_ms = 0U) {
    const auto direction = kind == TransportReplayAction::NodeReboot
                               ? TransportReplayDirection::None
                               : TransportReplayDirection::HostToNode;
    return {sequence, at_ms, node_id, kind, direction, delay_ms, 0U, {}};
}

TransportReplayEvent frame(std::uint32_t sequence, std::uint64_t at_ms,
                           std::uint32_t node_id, std::uint32_t route,
                           std::uint8_t value) {
    return {sequence, at_ms, node_id, TransportReplayAction::Frame,
            TransportReplayDirection::HostToNode, 0U, route, {value}};
}

template <typename Callback>
void expect_invalid(Callback callback) {
    try {
        callback();
        CHECK(false);
    } catch (const std::invalid_argument&) {
    }
}

void check_deterministic_faults_and_node_isolation() {
    const std::vector<TransportReplayEvent> events{
        action(1U, 0U, 1U, TransportReplayAction::DelayNext, 10U),
        action(2U, 0U, 1U, TransportReplayAction::DuplicateNext),
        frame(3U, 0U, 1U, 100U, 0xA1U),
        action(4U, 1U, 2U, TransportReplayAction::DelayNext, 20U),
        frame(5U, 1U, 2U, 200U, 0xB2U),
        action(6U, 2U, 1U, TransportReplayAction::DropNext),
        frame(7U, 2U, 1U, 101U, 0xA3U),
        action(8U, 5U, 1U, TransportReplayAction::NodeReboot),
        frame(9U, 6U, 1U, 102U, 0xA4U),
    };

    const auto first = remotebsp::mock_mcu::run_transport_replay(events);
    const auto second = remotebsp::mock_mcu::run_transport_replay(events);
    CHECK(first.event_count == 9U);
    CHECK(first.dropped_frames == 1U);
    CHECK(first.duplicated_frames == 1U);
    CHECK(first.reboot_invalidated_frames == 2U);
    CHECK(first.reboot_count == 1U);
    CHECK(first.deliveries.size() == 2U);
    CHECK(first.deliveries[0].delivery_sequence == 1U);
    CHECK(first.deliveries[0].source_sequence == 9U);
    CHECK(first.deliveries[0].at_ms == 6U);
    CHECK(first.deliveries[0].node_id == 1U);
    CHECK(first.deliveries[0].session_generation == 2U);
    CHECK(first.deliveries[0].data == std::vector<std::uint8_t>{0xA4U});
    // 节点 1 重启只清除自身延迟帧；节点 2 的待交付帧不受影响。
    CHECK(first.deliveries[1].delivery_sequence == 2U);
    CHECK(first.deliveries[1].source_sequence == 5U);
    CHECK(first.deliveries[1].at_ms == 21U);
    CHECK(first.deliveries[1].node_id == 2U);
    CHECK(first.deliveries[1].session_generation == 1U);
    CHECK(first.final_generations.size() == 2U);
    CHECK(first.final_generations[0].node_id == 1U);
    CHECK(first.final_generations[0].session_generation == 2U);
    CHECK(first.final_generations[1].node_id == 2U);
    CHECK(first.final_generations[1].session_generation == 1U);
    CHECK(first.replay_digest == second.replay_digest);
    remotebsp::mock_mcu::verify_transport_replay(events, first);

    auto tampered = first;
    tampered.deliveries.front().data.front() ^= 0xFFU;
    expect_invalid([&] {
        remotebsp::mock_mcu::verify_transport_replay(events, tampered);
    });
}

void check_stable_same_time_order_and_one_shot_effects() {
    const std::vector<TransportReplayEvent> events{
        action(1U, 4U, 3U, TransportReplayAction::DuplicateNext),
        frame(2U, 4U, 3U, 10U, 1U),
        frame(3U, 4U, 3U, 11U, 2U),
    };
    const auto record = remotebsp::mock_mcu::run_transport_replay(events);
    CHECK(record.deliveries.size() == 3U);
    CHECK(record.deliveries[0].source_sequence == 2U);
    CHECK(record.deliveries[1].source_sequence == 2U);
    CHECK(record.deliveries[2].source_sequence == 3U);
    CHECK(record.deliveries[0].delivery_sequence == 1U);
    CHECK(record.deliveries[2].delivery_sequence == 3U);

    const std::vector<TransportReplayEvent> directional{
        {1U, 0U, 3U, TransportReplayAction::DropNext,
         TransportReplayDirection::NodeToHost, 0U, 0U, {}},
        frame(2U, 0U, 3U, 12U, 3U),
        {3U, 0U, 3U, TransportReplayAction::Frame,
         TransportReplayDirection::NodeToHost, 0U, 13U, {4U}},
    };
    const auto directional_record =
        remotebsp::mock_mcu::run_transport_replay(directional);
    CHECK(directional_record.dropped_frames == 1U);
    CHECK(directional_record.deliveries.size() == 1U);
    CHECK(directional_record.deliveries.front().direction ==
           TransportReplayDirection::HostToNode);
}

void check_invalid_inputs_and_resource_bounds() {
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay(
            {frame(2U, 0U, 1U, 1U, 1U)});
    });
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay({
            frame(1U, 1U, 1U, 1U, 1U),
            frame(2U, 0U, 1U, 1U, 1U),
        });
    });
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay({
            action(1U, 0U, 0U, TransportReplayAction::DropNext)});
    });
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay({action(
            1U, 0U, 1U, static_cast<TransportReplayAction>(255U))});
    });
    expect_invalid([] {
        std::vector<std::uint8_t> data(
            remotebsp::mock_mcu::kMaximumTransportReplayFrameBytes + 1U,
            0U);
        remotebsp::mock_mcu::run_transport_replay({
            {1U, 0U, 1U, TransportReplayAction::Frame,
             TransportReplayDirection::HostToNode, 0U, 1U,
             std::move(data)}});
    });
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay({
            action(1U, 0U, 1U, TransportReplayAction::DelayNext, 40000U),
            action(2U, 0U, 1U, TransportReplayAction::DelayNext, 40000U),
        });
    });
    expect_invalid([] {
        std::vector<TransportReplayEvent> events;
        for (std::uint32_t index = 0U; index < 8U; ++index) {
            events.push_back(action(index + 1U, 0U, 1U,
                                    TransportReplayAction::DuplicateNext));
        }
        remotebsp::mock_mcu::run_transport_replay(events);
    });
    expect_invalid([] {
        remotebsp::mock_mcu::run_transport_replay({
            action(1U, std::numeric_limits<std::uint64_t>::max() - 5U,
                   1U, TransportReplayAction::DelayNext, 10U),
            frame(2U, std::numeric_limits<std::uint64_t>::max() - 5U,
                  1U, 1U, 1U),
        });
    });

    expect_invalid([] {
        std::vector<TransportReplayEvent> events;
        events.reserve(2050U);
        for (std::uint32_t index = 0U; index < 1025U; ++index) {
            const auto node_id = index % 127U + 1U;
            events.push_back(action(index * 2U + 1U, 0U, node_id,
                                    TransportReplayAction::DelayNext,
                                    60000U));
            events.push_back(frame(index * 2U + 2U, 0U, node_id,
                                   index + 1U, 1U));
        }
        remotebsp::mock_mcu::run_transport_replay(events);
    });
}

void check_mock_node_h2n_and_n2h_adapters() {
    constexpr std::size_t mtu = 64U;
    remotebsp::mock_mcu::NodeInfo info;
    info.uuid[0] = 0x42U;
    remotebsp::mock_mcu::MockNode node(
        remotebsp::mock_mcu::RemoteCore(
            info, remotebsp::mock_mcu::capability_mask(
                      remotebsp::mock_mcu::Capability::Gpio)),
        mtu, 7U);

    remotebsp::protocol::Packet request;
    request.header.message_type =
        remotebsp::protocol::MessageType::Request;
    request.header.command = static_cast<std::uint16_t>(
        remotebsp::protocol::Command::Ping);
    request.header.session_id = 11U;
    request.header.request_id = 22U;
    request.payload = {1U, 2U, 3U};
    const auto request_frames = remotebsp::protocol::Fragmenter(mtu).split(
        remotebsp::protocol::encode(request), 99U);
    CHECK(request_frames.size() == 1U);

    const std::vector<TransportReplayEvent> inbound{
        {1U, 5U, 7U, TransportReplayAction::Frame,
         TransportReplayDirection::HostToNode, 0U,
         remotebsp::transport::kNodeRequestBaseRoute + 7U,
         request_frames.front()},
    };
    const auto inbound_record =
        remotebsp::mock_mcu::run_transport_replay(inbound);
    CHECK(inbound_record.deliveries.size() == 1U);
    const auto reply =
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, inbound_record.deliveries.front(),
            remotebsp::protocol::Reassembler::Clock::now());
    CHECK(reply.has_value());
    CHECK(node.executed_request_count() == 1U);

    const auto outbound =
        remotebsp::mock_mcu::make_mock_node_reply_replay_events(
            1U, 6U, 7U, 88U, *reply);
    const auto outbound_record =
        remotebsp::mock_mcu::run_transport_replay(outbound);
    CHECK(outbound_record.deliveries.size() == reply->frames.size());
    CHECK(outbound_record.deliveries.front().direction ==
           TransportReplayDirection::NodeToHost);

    auto wrong_direction = outbound_record.deliveries.front();
    expect_invalid([&] {
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, wrong_direction,
            remotebsp::protocol::Reassembler::Clock::now());
    });

    auto unknown_route = inbound_record.deliveries.front();
    unknown_route.node_id = 0U;
    unknown_route.route = 0x123U;
    expect_invalid([&] {
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, unknown_route,
            remotebsp::protocol::Reassembler::Clock::now());
    });

    auto other_node_route = inbound_record.deliveries.front();
    other_node_route.node_id = 0U;
    other_node_route.route =
        remotebsp::transport::kNodeRequestBaseRoute + 8U;
    expect_invalid([&] {
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, other_node_route,
            remotebsp::protocol::Reassembler::Clock::now());
    });
}

void check_recording_transport_semantic_boundaries() {
    std::uint64_t clock_ms = 0U;
    auto recorder = std::make_shared<
        remotebsp::mock_mcu::TransportSessionRecorder>(
        [&clock_ms] { return clock_ms; });
    auto scripted = std::make_unique<ScriptedTransport>();
    auto* inner = scripted.get();
    remotebsp::mock_mcu::RecordingLinkTransport transport(
        std::move(scripted),
        remotebsp::mock_mcu::RecordingTransportRole::Node, recorder);

    const remotebsp::transport::LinkFrame reply{0x587U, {0x10U, 0x20U}};
    transport.send(reply);
    CHECK(inner->sent.size() == 1U);

    clock_ms = 1U;
    inner->fail_send = true;
    try {
        transport.send({0x507U, {0x30U}});
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    inner->fail_send = false;

    clock_ms = 2U;
    inner->received.push_back({0x609U, {0x40U}});
    const auto received = transport.receive(std::chrono::milliseconds(8));
    CHECK(received.has_value());

    clock_ms = 3U;
    inner->received.push_back({remotebsp::transport::kDiscoveryRoute,
                               {0x41U}});
    CHECK(transport.receive(std::chrono::milliseconds(8)).has_value());
    CHECK(!transport.receive(std::chrono::milliseconds(8)).has_value());

    // Node 侧收到的响应路由属于错方向帧，不能据此归属稳定节点。
    inner->received.push_back({0x589U, {0x42U}});
    CHECK(transport.receive(std::chrono::milliseconds(8)).has_value());
    CHECK(!transport.receive(std::chrono::milliseconds(8)).has_value());

    clock_ms = 4U;
    inner->fail_receive = true;
    try {
        static_cast<void>(transport.receive(std::chrono::milliseconds(9)));
        CHECK(false);
    } catch (const std::runtime_error&) {
    }

    CHECK(transport.capabilities().kind ==
           remotebsp::transport::LinkKind::MockUsb);
    const auto session = recorder->snapshot();
    CHECK(session.events.size() == 8U);
    CHECK(session.events[0].action == TransportReplayAction::Frame);
    CHECK(session.events[0].direction ==
           TransportReplayDirection::NodeToHost);
    CHECK(session.events[1].action == TransportReplayAction::SendFailure);
    CHECK(session.events[2].direction ==
           TransportReplayDirection::HostToNode);
    CHECK(session.events[2].node_id == 9U);
    CHECK(session.events[3].node_id == 0U);
    CHECK(session.events[4].action == TransportReplayAction::ReceiveEmpty);
    CHECK(session.events[4].delay_ms == 8U);
    CHECK(session.events[5].action == TransportReplayAction::Frame);
    CHECK(session.events[5].node_id == 0U);
    CHECK(session.events[6].action == TransportReplayAction::ReceiveEmpty);
    CHECK(session.events[7].action == TransportReplayAction::ReceiveFailure);
    CHECK(session.record.send_failures == 1U);
    CHECK(session.record.receive_empty_results == 2U);
    CHECK(session.record.receive_failures == 1U);
    CHECK(session.record.deliveries.size() == 4U);

    auto host_recorder = std::make_shared<
        remotebsp::mock_mcu::TransportSessionRecorder>();
    auto host_scripted = std::make_unique<ScriptedTransport>();
    auto* host_inner = host_scripted.get();
    host_inner->received.push_back({0x609U, {0x43U}});
    remotebsp::mock_mcu::RecordingLinkTransport host_transport(
        std::move(host_scripted),
        remotebsp::mock_mcu::RecordingTransportRole::Host, host_recorder);
    CHECK(host_transport.receive(std::chrono::milliseconds(1)).has_value());
    CHECK(host_recorder->snapshot().events.front().node_id == 0U);

    const auto receive_calls = inner->receive_calls;
    try {
        static_cast<void>(
            transport.receive(std::chrono::milliseconds(-1)));
        CHECK(false);
    } catch (const std::invalid_argument&) {
    }
    CHECK(inner->receive_calls == receive_calls);

    clock_ms = 3U;
    try {
        recorder->record_receive_empty(
            TransportReplayDirection::HostToNode, 7U,
            std::chrono::milliseconds(1));
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    try {
        static_cast<void>(recorder->snapshot());
        CHECK(false);
    } catch (const std::runtime_error&) {
    }

}

void check_post_io_commit_and_capture_stop_do_not_change_data_plane() {
    std::uint64_t clock_ms = 10U;
    auto recorder = std::make_shared<
        remotebsp::mock_mcu::TransportSessionRecorder>(
        [&clock_ms] { return clock_ms; });
    auto scripted = std::make_unique<ScriptedTransport>();
    auto* inner = scripted.get();
    inner->on_send = [&clock_ms] { clock_ms = 9U; };
    remotebsp::mock_mcu::RecordingLinkTransport transport(
        std::move(scripted),
        remotebsp::mock_mcu::RecordingTransportRole::Node, recorder);
    transport.send({0x587U, {1U}});
    CHECK(recorder->snapshot().record.deliveries.size() == 1U);
    // 第二次预留观察到时钟倒退，停止诊断采集，但真实发送仍成功。
    transport.send({0x587U, {2U}});
    CHECK(inner->sent.size() == 2U);
    try {
        static_cast<void>(recorder->snapshot());
        CHECK(false);
    } catch (const std::runtime_error&) {
    }

    std::uint64_t capacity_clock = 0U;
    auto bounded = std::make_shared<
        remotebsp::mock_mcu::TransportSessionRecorder>(
        [&capacity_clock] { return capacity_clock++; });
    auto capacity_transport = std::make_unique<ScriptedTransport>();
    auto* capacity_inner = capacity_transport.get();
    remotebsp::mock_mcu::RecordingLinkTransport wrapped_capacity(
        std::move(capacity_transport),
        remotebsp::mock_mcu::RecordingTransportRole::Node, bounded);
    for (std::size_t index = 0U;
         index <= remotebsp::mock_mcu::kMaximumTransportReplayEvents;
         ++index) {
        wrapped_capacity.send({0x587U, {1U}});
    }
    CHECK(capacity_inner->sent.size() ==
           remotebsp::mock_mcu::kMaximumTransportReplayEvents + 1U);
    try {
        static_cast<void>(bounded->snapshot());
        CHECK(false);
    } catch (const std::runtime_error&) {
    }

    auto oversized_recorder = std::make_shared<
        remotebsp::mock_mcu::TransportSessionRecorder>();
    auto oversized_transport = std::make_unique<ScriptedTransport>();
    auto* oversized_inner = oversized_transport.get();
    remotebsp::mock_mcu::RecordingLinkTransport wrapped_oversized(
        std::move(oversized_transport),
        remotebsp::mock_mcu::RecordingTransportRole::Node,
        oversized_recorder);
    wrapped_oversized.send({
        0x587U,
        std::vector<std::uint8_t>(
            remotebsp::mock_mcu::kMaximumTransportReplayFrameBytes + 1U,
            0x5AU)});
    CHECK(oversized_inner->sent.size() == 1U);
    try {
        static_cast<void>(oversized_recorder->snapshot());
        CHECK(false);
    } catch (const std::runtime_error&) {
    }

    auto invalid_inner = std::make_unique<ScriptedTransport>();
    try {
        remotebsp::mock_mcu::RecordingLinkTransport invalid(
            std::move(invalid_inner),
            static_cast<remotebsp::mock_mcu::RecordingTransportRole>(0U),
            std::make_shared<
                remotebsp::mock_mcu::TransportSessionRecorder>());
        CHECK(false);
    } catch (const std::invalid_argument&) {
    }
}

void check_session_file_roundtrip_and_strictness() {
    const std::vector<TransportReplayEvent> events{
        action(1U, 0U, 2U, TransportReplayAction::DelayNext, 5U),
        frame(2U, 1U, 2U, 0x602U, 0xA5U),
        {3U, 2U, 2U, TransportReplayAction::ReceiveEmpty,
         TransportReplayDirection::NodeToHost, 10U, 0U, {}, 3U},
        action(4U, 3U, 2U, TransportReplayAction::NodeReboot),
    };
    const auto session =
        remotebsp::mock_mcu::make_transport_replay_session(events);
    const auto encoded =
        remotebsp::mock_mcu::encode_transport_replay_session(session);
    const auto parsed =
        remotebsp::mock_mcu::parse_transport_replay_session(encoded);
    CHECK(parsed.file_version == 1U);
    CHECK(parsed.evidence_scope == "logical-link-boundary-only");
    CHECK(parsed.time_base == "monotonic-relative-ms");
    CHECK(parsed.record.replay_digest == session.record.replay_digest);
    CHECK(parsed.record.receive_empty_results == 3U);

    std::remove(TEST_TRANSPORT_REPLAY_OUTPUT);
    remotebsp::mock_mcu::write_transport_replay_session(
        session, TEST_TRANSPORT_REPLAY_OUTPUT);
    const auto loaded = remotebsp::mock_mcu::load_transport_replay_session(
        TEST_TRANSPORT_REPLAY_OUTPUT);
    CHECK(loaded.file_checksum == session.file_checksum);
    try {
        remotebsp::mock_mcu::write_transport_replay_session(
            session, TEST_TRANSPORT_REPLAY_OUTPUT);
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    std::remove(TEST_TRANSPORT_REPLAY_OUTPUT);

    auto tampered = encoded;
    const auto payload = tampered.find("a5");
    CHECK(payload != std::string::npos);
    tampered[payload] = 'b';
    expect_invalid([&] {
        remotebsp::mock_mcu::parse_transport_replay_session(tampered);
    });
    expect_invalid([&] {
        remotebsp::mock_mcu::parse_transport_replay_session(
            encoded + "trailing\n");
    });
    expect_invalid([] {
        remotebsp::mock_mcu::parse_transport_replay_session(std::string(
            remotebsp::mock_mcu::kMaximumTransportSessionFileBytes + 1U,
            'x'));
    });
}

}  // namespace

int main() {
    check_deterministic_faults_and_node_isolation();
    check_stable_same_time_order_and_one_shot_effects();
    check_invalid_inputs_and_resource_bounds();
    check_mock_node_h2n_and_n2h_adapters();
    check_recording_transport_semantic_boundaries();
    check_post_io_commit_and_capture_stop_do_not_change_data_plane();
    check_session_file_roundtrip_and_strictness();
    return 0;
}
