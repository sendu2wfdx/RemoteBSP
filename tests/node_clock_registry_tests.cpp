#include "remotebsp/protocol/discovery.hpp"
#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/toolbusd/node_registry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                       \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

protocol::NodeIdentity make_identity(std::uint8_t seed) {
    protocol::NodeIdentity identity;
    for (std::size_t index = 0; index < identity.uuid.size(); ++index) {
        identity.uuid[index] = static_cast<std::uint8_t>(seed + index);
    }
    identity.firmware_major = 1U;
    identity.board_type = 0x12340000U + seed;
    identity.protocol_version = protocol::kProtocolVersion;
    return identity;
}

protocol::Packet make_discovery_response(
    const protocol::NodeIdentity& identity) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Response;
    packet.header.command = static_cast<std::uint16_t>(
        protocol::Command::DiscoveryResponse);
    packet.payload = protocol::encode_node_identity(identity);
    return packet;
}

protocol::Packet make_heartbeat(const protocol::NodeIdentity& identity,
                                std::uint32_t node_id) {
    protocol::Packet packet;
    packet.header.message_type = protocol::MessageType::Event;
    packet.header.command =
        static_cast<std::uint16_t>(protocol::Command::Heartbeat);
    packet.header.object_id = node_id;
    packet.payload = protocol::encode_heartbeat(
        {identity.uuid, identity.protocol_version});
    return packet;
}

void add_assigned_node(toolbusd::NodeRegistry& registry,
                       const protocol::NodeIdentity& identity,
                       std::uint32_t node_id,
                       toolbusd::NodeRegistry::TimePoint now = {}) {
    CHECK(registry.accept_discovery_response(
              make_discovery_response(identity), now) ==
          toolbusd::NodeUpdate::Added);
    static_cast<void>(
        registry.make_node_assignment(identity.uuid, node_id, node_id));
    CHECK(registry.accept_heartbeat(
        make_heartbeat(identity, node_id),
        now + std::chrono::milliseconds(1)));
}

toolbusd::ClockModelConfig test_config() {
    toolbusd::ClockModelConfig config;
    config.window_size = 8U;
    config.low_rtt_sample_count = 4U;
    config.minimum_samples = 4U;
    config.minimum_fit_span_ns = 5000000ULL;
    config.maximum_round_trip_ns = 1000000ULL;
    config.synchronized_max_age_ns = 50000000ULL;
    config.model_expiry_ns = 200000000ULL;
    config.maximum_error_bound_ns = 300000ULL;
    config.minimum_drift_uncertainty_ppm = 1U;
    return config;
}

toolbusd::FourTimestampSample make_sample(std::size_t index) {
    constexpr std::uint64_t base_host_ns = 1000000000ULL;
    constexpr std::uint64_t spacing_ns = 2000000ULL;
    constexpr std::uint64_t upward_delay_ns = 100000ULL;
    constexpr std::uint64_t turnaround_ticks = 20ULL;
    constexpr std::uint64_t downward_delay_ns = 100000ULL;
    constexpr std::uint64_t node_offset_ticks = 500ULL;
    const auto host_send =
        base_host_ns + static_cast<std::uint64_t>(index) * spacing_ns;
    const auto node_receive =
        (host_send + upward_delay_ns) / 1000ULL + node_offset_ticks;
    return {host_send, node_receive,
            node_receive + turnaround_ticks,
            host_send + upward_delay_ns +
                turnaround_ticks * 1000ULL + downward_delay_ns};
}

void train_model(toolbusd::NodeRegistry& registry,
                 std::uint32_t node_id, std::uint64_t boot_epoch) {
    for (std::size_t index = 0U; index < 6U; ++index) {
        const auto outcome = registry.add_clock_sample(
            node_id, boot_epoch, make_sample(index));
        CHECK(outcome.status == toolbusd::NodeClockAccessStatus::Ready);
        CHECK(outcome.sample_result ==
              toolbusd::ClockSampleResult::Accepted);
    }
}

void test_registration_sampling_and_gate() {
    toolbusd::NodeRegistry registry(std::chrono::milliseconds(2000), 4U);
    const auto identity = make_identity(1U);
    registry.accept_discovery_response(make_discovery_response(identity));
    static_cast<void>(registry.make_node_assignment(identity.uuid, 21U, 1U));
    CHECK(registry.register_clock_model(21U, 9U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::NodeUnavailable);
    CHECK(registry.accept_heartbeat(make_heartbeat(identity, 21U)));
    CHECK(registry.register_clock_model(21U, 0U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::InvalidBootEpoch);
    CHECK(registry.register_clock_model(21U, 9U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::Registered);
    CHECK(registry.register_clock_model(21U, 9U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::AlreadyRegistered);
    CHECK(registry.clock_model_count() == 1U);
    CHECK(registry.clock_boot_epoch(21U) == 9U);
    const auto initial_quality = registry.clock_quality(21U, 1000000000ULL);
    CHECK(initial_quality.has_value());
    CHECK(initial_quality->boot_epoch == 9U);
    CHECK(initial_quality->model_generation != 0U);
    CHECK(!initial_quality->estimate.valid);
    CHECK(initial_quality->estimate.state ==
          toolbusd::ClockSyncState::Unsynced);
    CHECK(initial_quality->estimate.sample_count == 0U);

    const auto before_training = registry.host_to_node_time(
        21U, 9U, 1000000000ULL, 1000000000ULL, 200000ULL);
    CHECK(before_training.status ==
          toolbusd::NodeClockAccessStatus::Unsynced);

    train_model(registry, 21U, 9U);
    const auto duplicate =
        registry.add_clock_sample(21U, 9U, make_sample(5U));
    CHECK(duplicate.status ==
          toolbusd::NodeClockAccessStatus::SampleRejected);
    CHECK(duplicate.sample_result ==
          toolbusd::ClockSampleResult::HostTimeWentBackwards);
    constexpr std::uint64_t last_receive_ns = 1010220000ULL;
    const auto quality = registry.clock_quality(
        21U, last_receive_ns + 1000000ULL);
    CHECK(quality.has_value());
    CHECK(quality->boot_epoch == 9U);
    CHECK(quality->model_generation != initial_quality->model_generation);
    CHECK(quality->estimate.valid);
    CHECK(quality->estimate.sample_count == 6U);
    CHECK(quality->estimate.selected_sample_count == 4U);
    const auto estimate =
        registry.clock_estimate(21U, 9U, last_receive_ns + 1000000ULL);
    CHECK(estimate.has_value());
    CHECK(estimate->state == toolbusd::ClockSyncState::Synced);

    const auto target_ns = last_receive_ns + 5000000ULL;
    const auto converted = registry.host_to_node_time(
        21U, 9U, target_ns, last_receive_ns + 1000000ULL, 200000ULL);
    CHECK(converted.status == toolbusd::NodeClockAccessStatus::Ready);
    CHECK(converted.node_tick.has_value());
    CHECK(converted.estimate.has_value());
    CHECK(converted.estimate->state == toolbusd::ClockSyncState::Synced);
    CHECK(*converted.node_tick == target_ns / 1000ULL + 500ULL);
    const auto wrong_epoch = registry.host_to_node_time(
        21U, 10U, target_ns, last_receive_ns, 200000ULL);
    CHECK(wrong_epoch.status ==
          toolbusd::NodeClockAccessStatus::BootEpochMismatch);

    const auto past = registry.host_to_node_time(
        21U, 9U, last_receive_ns, last_receive_ns + 1U, 200000ULL);
    CHECK(past.status == toolbusd::NodeClockAccessStatus::TargetInPast);
    const auto too_strict = registry.host_to_node_time(
        21U, 9U, target_ns, last_receive_ns, 50000ULL);
    CHECK(too_strict.status ==
          toolbusd::NodeClockAccessStatus::ErrorBoundExceeded);
    const auto stale_target = registry.host_to_node_time(
        21U, 9U, last_receive_ns + 60000000ULL,
        last_receive_ns, 300000ULL);
    CHECK(stale_target.status == toolbusd::NodeClockAccessStatus::Degraded);
}

void test_epoch_isolation_and_lifecycle_reset() {
    toolbusd::NodeRegistry registry(std::chrono::milliseconds(100));
    const auto identity = make_identity(30U);
    const auto start = toolbusd::NodeRegistry::TimePoint{};
    add_assigned_node(registry, identity, 7U, start);
    CHECK(registry.register_clock_model(7U, 100U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::Registered);
    train_model(registry, 7U, 100U);

    const auto wrong_epoch =
        registry.add_clock_sample(7U, 101U, make_sample(6U));
    CHECK(wrong_epoch.status ==
          toolbusd::NodeClockAccessStatus::BootEpochMismatch);
    CHECK(!wrong_epoch.sample_result.has_value());
    CHECK(registry.register_clock_model(7U, 101U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::ReplacedBootEpoch);
    CHECK(!registry.clock_estimate(7U, 100U, 1011000000ULL).has_value());
    const auto fresh = registry.clock_estimate(7U, 101U, 1011000000ULL);
    CHECK(fresh.has_value());
    CHECK(fresh->state == toolbusd::ClockSyncState::Unsynced);
    CHECK(registry.clock_model_count() == 1U);

    CHECK(registry.mark_assignment_unconfirmed(identity.uuid));
    CHECK(registry.clock_model_count() == 0U);
    CHECK(registry.accept_heartbeat(
        make_heartbeat(identity, 7U), start + std::chrono::milliseconds(2)));
    CHECK(registry.register_clock_model(7U, 102U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::Registered);
    CHECK(registry.expire(start + std::chrono::milliseconds(102)).size() ==
          1U);
    CHECK(registry.clock_model_count() == 0U);
    CHECK(!registry.clock_estimate(7U, 102U, 1011000000ULL).has_value());
    CHECK(!registry.clock_quality(7U, 1011000000ULL).has_value());
}

void test_bounded_capacity_and_node_id_ownership() {
    toolbusd::NodeRegistry registry(std::chrono::milliseconds(2000), 1U);
    const auto first = make_identity(50U);
    const auto second = make_identity(80U);
    add_assigned_node(registry, first, 1U);
    add_assigned_node(registry, second, 2U);
    CHECK(registry.register_clock_model(99U, 1U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::UnknownNode);
    CHECK(registry.register_clock_model(1U, 1U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::Registered);
    CHECK(registry.register_clock_model(2U, 1U, test_config()) ==
          toolbusd::NodeClockRegistrationResult::CapacityReached);

    try {
        static_cast<void>(
            registry.make_node_assignment(second.uuid, 1U, 99U));
        CHECK(false);
    } catch (const std::invalid_argument&) {
        CHECK(true);
    }
    CHECK(registry.clock_model_count() == 1U);
}

}

int main() {
    test_registration_sampling_and_gate();
    test_epoch_isolation_and_lifecycle_reset();
    test_bounded_capacity_and_node_id_ownership();
    if (failures != 0) {
        std::cerr << failures << " 个节点时钟注册测试失败\n";
        return 1;
    }
    std::cout << "所有节点时钟注册测试通过\n";
    return 0;
}
