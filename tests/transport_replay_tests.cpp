#include "remotebsp/mock_mcu/transport_replay.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using remotebsp::mock_mcu::TransportReplayAction;
using remotebsp::mock_mcu::TransportReplayDirection;
using remotebsp::mock_mcu::TransportReplayEvent;

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
        assert(false);
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
    assert(first.event_count == 9U);
    assert(first.dropped_frames == 1U);
    assert(first.duplicated_frames == 1U);
    assert(first.reboot_invalidated_frames == 2U);
    assert(first.reboot_count == 1U);
    assert(first.deliveries.size() == 2U);
    assert(first.deliveries[0].delivery_sequence == 1U);
    assert(first.deliveries[0].source_sequence == 9U);
    assert(first.deliveries[0].at_ms == 6U);
    assert(first.deliveries[0].node_id == 1U);
    assert(first.deliveries[0].session_generation == 2U);
    assert(first.deliveries[0].data == std::vector<std::uint8_t>{0xA4U});
    // 节点 1 重启只清除自身延迟帧；节点 2 的待交付帧不受影响。
    assert(first.deliveries[1].delivery_sequence == 2U);
    assert(first.deliveries[1].source_sequence == 5U);
    assert(first.deliveries[1].at_ms == 21U);
    assert(first.deliveries[1].node_id == 2U);
    assert(first.deliveries[1].session_generation == 1U);
    assert(first.final_generations.size() == 2U);
    assert(first.final_generations[0].node_id == 1U);
    assert(first.final_generations[0].session_generation == 2U);
    assert(first.final_generations[1].node_id == 2U);
    assert(first.final_generations[1].session_generation == 1U);
    assert(first.replay_digest == second.replay_digest);
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
    assert(record.deliveries.size() == 3U);
    assert(record.deliveries[0].source_sequence == 2U);
    assert(record.deliveries[1].source_sequence == 2U);
    assert(record.deliveries[2].source_sequence == 3U);
    assert(record.deliveries[0].delivery_sequence == 1U);
    assert(record.deliveries[2].delivery_sequence == 3U);

    const std::vector<TransportReplayEvent> directional{
        {1U, 0U, 3U, TransportReplayAction::DropNext,
         TransportReplayDirection::NodeToHost, 0U, 0U, {}},
        frame(2U, 0U, 3U, 12U, 3U),
        {3U, 0U, 3U, TransportReplayAction::Frame,
         TransportReplayDirection::NodeToHost, 0U, 13U, {4U}},
    };
    const auto directional_record =
        remotebsp::mock_mcu::run_transport_replay(directional);
    assert(directional_record.dropped_frames == 1U);
    assert(directional_record.deliveries.size() == 1U);
    assert(directional_record.deliveries.front().direction ==
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
    assert(request_frames.size() == 1U);

    const std::vector<TransportReplayEvent> inbound{
        {1U, 5U, 7U, TransportReplayAction::Frame,
         TransportReplayDirection::HostToNode, 0U, 77U,
         request_frames.front()},
    };
    const auto inbound_record =
        remotebsp::mock_mcu::run_transport_replay(inbound);
    assert(inbound_record.deliveries.size() == 1U);
    const auto reply =
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, inbound_record.deliveries.front(),
            remotebsp::protocol::Reassembler::Clock::now());
    assert(reply.has_value());
    assert(node.executed_request_count() == 1U);

    const auto outbound =
        remotebsp::mock_mcu::make_mock_node_reply_replay_events(
            1U, 6U, 7U, 88U, *reply);
    const auto outbound_record =
        remotebsp::mock_mcu::run_transport_replay(outbound);
    assert(outbound_record.deliveries.size() == reply->frames.size());
    assert(outbound_record.deliveries.front().direction ==
           TransportReplayDirection::NodeToHost);

    auto wrong_direction = outbound_record.deliveries.front();
    expect_invalid([&] {
        remotebsp::mock_mcu::deliver_replayed_frame_to_mock_node(
            node, wrong_direction,
            remotebsp::protocol::Reassembler::Clock::now());
    });
}

}  // namespace

int main() {
    check_deterministic_faults_and_node_isolation();
    check_stable_same_time_order_and_one_shot_effects();
    check_invalid_inputs_and_resource_bounds();
    check_mock_node_h2n_and_n2h_adapters();
    return 0;
}
