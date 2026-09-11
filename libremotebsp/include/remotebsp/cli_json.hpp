#pragma once

#include "client.hpp"

#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace remotebsp::cli_json {

constexpr std::uint32_t kSchemaVersion = 1;

inline void write_string(std::ostream& output, std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output << "\\u00" << digits[byte >> 4U]
                           << digits[byte & 0x0FU];
                } else {
                    output << static_cast<char>(byte);
                }
        }
    }
    output << '"';
}

inline void write_ipc_error(std::ostream& output, std::string_view command,
                            const IpcErrorException& error) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":";
    write_string(output, command);
    output << ",\"error\":{\"ipc_error_version\":" << error.version()
           << ",\"code\":" << error.code()
           << ",\"category\":" << static_cast<unsigned>(error.category())
           << ",\"retryable\":"
           << (error.retryable() ? "true" : "false")
           << ",\"possibly_committed\":"
           << (error.possibly_committed() ? "true" : "false")
           << ",\"message\":";
    write_string(output, error.what());
    output << "}}\n";
}

inline void write_digest_or_null(
    std::ostream& output, const std::array<std::uint8_t, 32>& digest,
    bool available) {
    static constexpr char digits[] = "0123456789abcdef";
    if (!available) {
        output << "null";
        return;
    }
    output << '"';
    for (const auto byte : digest) {
        output << digits[byte >> 4U] << digits[byte & 0x0FU];
    }
    output << '"';
}

inline void write_firmware_identity(
    std::ostream& output,
    const protocol::FirmwareIdentityPayload& identity) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"firmware-identity\""
           << ",\"identity_schema_version\":"
           << identity.schema_version
           << ",\"board_type\":" << identity.board_type
           << ",\"uuid\":\"";
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : identity.node_uuid) {
        output << digits[byte >> 4U] << digits[byte & 0x0FU];
    }
    output << '"'
           << ",\"project_sha256\":";
    write_digest_or_null(
        output, identity.project_sha256,
        identity.available(protocol::FirmwareIdentityField::ProjectSha256));
    output << ",\"config_sha256\":";
    write_digest_or_null(
        output, identity.config_sha256,
        identity.available(protocol::FirmwareIdentityField::ConfigSha256));
    output << ",\"firmware_input_sha256\":";
    write_digest_or_null(
        output, identity.firmware_input_sha256,
        identity.available(
            protocol::FirmwareIdentityField::FirmwareInputSha256));
    output << "}\n";
}

inline const char* runtime_operation_kind(RuntimeOperationKind kind) {
    switch (kind) {
        case RuntimeOperationKind::Unknown: return "unknown";
        case RuntimeOperationKind::GpioWrite: return "gpio_write";
        case RuntimeOperationKind::ControlRelease: return "control_release";
        case RuntimeOperationKind::PwmConfigure: return "pwm_configure";
        case RuntimeOperationKind::PwmStop: return "pwm_stop";
        case RuntimeOperationKind::TimedBitstreamConfigure:
            return "timed_bitstream_configure";
        case RuntimeOperationKind::TimedBitstreamFrame:
            return "timed_bitstream_frame";
        case RuntimeOperationKind::TimedBitstreamStop:
            return "timed_bitstream_stop";
        case RuntimeOperationKind::BusResourceReset:
            return "bus_resource_reset";
        case RuntimeOperationKind::MotionGroupCancel:
            return "motion_group_cancel";
    }
    return "unknown";
}

inline const char* runtime_operation_state(RuntimeOperationState state) {
    switch (state) {
        case RuntimeOperationState::Pending: return "pending";
        case RuntimeOperationState::Committed: return "committed";
        case RuntimeOperationState::Rejected: return "rejected";
        case RuntimeOperationState::Unknown: return "unknown";
        case RuntimeOperationState::ExpiredUnknown:
            return "expired_unknown";
    }
    return "unknown";
}

inline const char* runtime_operation_recovery(
    RuntimeOperationRecovery recovery) {
    switch (recovery) {
        case RuntimeOperationRecovery::None: return "none";
        case RuntimeOperationRecovery::NotSent: return "not_sent";
        case RuntimeOperationRecovery::SafeClosed: return "safe_closed";
        case RuntimeOperationRecovery::ScopeBlocked: return "scope_blocked";
        case RuntimeOperationRecovery::AwaitingReboot:
            return "awaiting_reboot";
        case RuntimeOperationRecovery::NodeRebootConfirmed:
            return "node_reboot_confirmed";
    }
    return "unknown";
}

inline const char* runtime_operation_error(RuntimeOperationError error) {
    switch (error) {
        case RuntimeOperationError::None: return "none";
        case RuntimeOperationError::Rejected: return "rejected";
        case RuntimeOperationError::Deadline: return "deadline";
        case RuntimeOperationError::Backend: return "backend";
        case RuntimeOperationError::Persistence: return "persistence";
        case RuntimeOperationError::HistoryExpired:
            return "history_expired";
    }
    return "unknown";
}

inline void write_operation_id(
    std::ostream& output,
    const std::array<std::uint8_t, 32>& operation_id) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto value : operation_id) {
        output << digits[value >> 4U] << digits[value & 0x0FU];
    }
}

inline void write_runtime_scope_id(
    std::ostream& output,
    const std::array<std::uint8_t, 16>& identifier) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto value : identifier) {
        output << digits[value >> 4U] << digits[value & 0x0FU];
    }
}

inline void write_runtime_operation_outcome(
    std::ostream& output, std::string_view command,
    const RuntimeOperationOutcome& outcome) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":";
    write_string(output, command);
    output << ",\"data\":{\"operation_id\":\"";
    write_operation_id(output, outcome.operation_id);
    output << "\",\"lease_id\":";
    if (outcome.lease_id.has_value()) {
        output << '"';
        write_runtime_scope_id(output, *outcome.lease_id);
        output << '"';
    } else {
        output << "null";
    }
    output << ",\"expected_node_uuid\":";
    if (outcome.expected_node_uuid.has_value()) {
        output << '"';
        write_runtime_scope_id(output, *outcome.expected_node_uuid);
        output << '"';
    } else {
        output << "null";
    }
    output << ",\"resource_id\":";
    if (outcome.resource_id.has_value()) {
        output << *outcome.resource_id;
    } else {
        output << "null";
    }
    output << ",\"kind\":";
    if (outcome.kind == RuntimeOperationKind::Unknown) {
        output << "null";
    } else {
        output << '"' << runtime_operation_kind(outcome.kind) << '"';
    }
    output << ",\"state\":\"" << runtime_operation_state(outcome.state)
           << "\",\"replayed\":"
           << (outcome.replayed ? "true" : "false")
           << ",\"recovery\":\""
           << runtime_operation_recovery(outcome.recovery)
           << "\",\"object_id\":";
    if (outcome.object_id.has_value()) {
        output << *outcome.object_id;
    } else {
        output << "null";
    }
    output << ",\"value\":";
    if (outcome.value.has_value()) {
        output << (*outcome.value ? "true" : "false");
    } else {
        output << "null";
    }
    if (outcome.kind == RuntimeOperationKind::PwmConfigure ||
        outcome.kind == RuntimeOperationKind::PwmStop) {
        output << ",\"frequency_hz\":";
        if (outcome.frequency_hz.has_value()) output << *outcome.frequency_hz;
        else output << "null";
        output << ",\"duty\":";
        if (outcome.duty.has_value()) output << *outcome.duty;
        else output << "null";
        output << ",\"active_low\":";
        if (outcome.active_low.has_value()) output << (*outcome.active_low ? "true" : "false");
        else output << "null";
    }
    output << ",\"error_code\":";
    if (outcome.error.has_value()) {
        write_string(output, runtime_operation_error(*outcome.error));
    } else {
        output << "null";
    }
    output << "}}\n";
}

inline const char* resource_type(protocol::ResourceType type) {
    switch (type) {
        case protocol::ResourceType::Gpio: return "gpio";
        case protocol::ResourceType::Uart: return "uart";
        case protocol::ResourceType::Spi: return "spi";
        case protocol::ResourceType::I2c: return "i2c";
        case protocol::ResourceType::Adc: return "adc";
        case protocol::ResourceType::Pwm: return "pwm";
        case protocol::ResourceType::Timer: return "timer";
        case protocol::ResourceType::Storage: return "storage";
        case protocol::ResourceType::StepgenAxis: return "stepgen-axis";
        case protocol::ResourceType::TimedBitstream:
            return "timed-bitstream";
        case protocol::ResourceType::I2cBus: return "i2c-bus";
        case protocol::ResourceType::I2cDevice: return "i2c-device";
        case protocol::ResourceType::SpiBus: return "spi-bus";
        case protocol::ResourceType::SpiDevice: return "spi-device";
        case protocol::ResourceType::Stream: return "stream";
    }
    return "unknown";
}

inline const char* health_name(protocol::ResourceHealth health) {
    switch (health) {
        case protocol::ResourceHealth::Normal: return "normal";
        case protocol::ResourceHealth::Busy: return "busy";
        case protocol::ResourceHealth::Degraded: return "degraded";
        case protocol::ResourceHealth::Failed: return "failed";
        case protocol::ResourceHealth::Disabled: return "disabled";
    }
    return "unknown";
}

inline const char* traffic_mode(LinkTrafficMode mode) {
    switch (mode) {
        case LinkTrafficMode::ClassicalCan: return "classical";
        case LinkTrafficMode::CanFd: return "fd";
        case LinkTrafficMode::Usb: return "usb";
    }
    return "unknown";
}

inline const char* traffic_class(std::size_t index) {
    static constexpr std::array<const char*, 6> names{
        "safety", "motion", "system", "interactive", "streaming", "bulk"};
    return index < names.size() ? names[index] : "unknown";
}

inline const char* runtime_clock_state(RuntimeClockState state) {
    switch (state) {
        case RuntimeClockState::Unsynced: return "unsynced";
        case RuntimeClockState::Synced: return "synced";
        case RuntimeClockState::Degraded: return "degraded";
    }
    return "unknown";
}

inline void write_uuid(std::ostream& output,
                       const std::array<std::uint8_t, 16>& uuid) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto value : uuid) {
        output << digits[value >> 4U] << digits[value & 0x0FU];
    }
}

inline void write_node_list(std::ostream& output,
                            const std::vector<DiscoveredNode>& nodes) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"node-list\",\"data\":{\"nodes\":[";
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (index != 0) output << ',';
        output << "{\"node_id\":" << node.node_id
               << ",\"online\":" << (node.online ? "true" : "false")
               << ",\"ready\":" << (node.ready ? "true" : "false")
               << ",\"board_type\":" << node.identity.board_type
               << ",\"firmware\":{"
               << "\"major\":" << node.identity.firmware_major
               << ",\"minor\":" << node.identity.firmware_minor
               << ",\"patch\":" << node.identity.firmware_patch << '}'
               << ",\"protocol_version\":"
               << static_cast<unsigned>(node.identity.protocol_version)
               << ",\"uuid\":\"";
        write_uuid(output, node.identity.uuid);
        output << "\"}";
    }
    output << "]}}\n";
}

inline void write_daemon_identity(std::ostream& output,
                                  const DaemonIdentity& identity) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"daemon-identity\",\"data\":{"
           << "\"ipc_version\":" << identity.version
           << ",\"instance_id\":\"";
    write_uuid(output, identity.instance_id);
    output << "\"}}\n";
}

inline void write_health_snapshot(
    std::ostream& output, const ToolbusdHealthSnapshot& snapshot) {
    const auto& health = snapshot.health;
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"health-snapshot\",\"data\":{"
           << "\"ipc_version\":" << snapshot.ipc_version
           << ",\"daemon_instance_id\":\"";
    write_uuid(output, snapshot.daemon_instance_id);
    output << "\",\"ipc\":{"
           << "\"active_clients\":" << snapshot.ipc_active_clients
           << ",\"maximum_clients\":" << snapshot.ipc_maximum_clients
           << ",\"peak_clients\":" << snapshot.ipc_peak_clients
           << ",\"accepted_total\":" << snapshot.ipc_accepted_total
           << ",\"capacity_rejected_total\":" << snapshot.ipc_capacity_rejected_total
           << ",\"oversized_frame_total\":" << snapshot.ipc_oversized_frame_total
           << ",\"timeout_total\":" << snapshot.ipc_timeout_total
           << ",\"thread_creation_failed_total\":" << snapshot.ipc_thread_creation_failed_total
           << "},\"health\":{\"contract_version\":" << health.version
           << ",\"source\":" << static_cast<unsigned>(health.source)
           << ",\"overall\":" << static_cast<unsigned>(health.overall)
           << ",\"sample_sequence\":" << health.sample_sequence
           << ",\"sample_time_ms\":" << health.sample_time_ms
           << ",\"node_id\":" << health.node_id
           << ",\"producer_generation\":" << health.producer_generation
           << ",\"metrics\":[";
    for (std::size_t index = 0U; index < health.metrics.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& metric = health.metrics[index];
        output << "{\"metric_id\":" << metric.metric_id
               << ",\"availability\":"
               << static_cast<unsigned>(metric.availability)
               << ",\"unit\":" << static_cast<unsigned>(metric.unit)
               << ",\"value\":" << metric.value << '}';
    }
    output << "]}}}\n";
}

inline void write_node_health_snapshot(
    std::ostream& output, const protocol::HealthSnapshot& health) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"node-health-snapshot\",\"data\":{"
           << "\"health\":{\"contract_version\":" << health.version
           << ",\"source\":" << static_cast<unsigned>(health.source)
           << ",\"overall\":" << static_cast<unsigned>(health.overall)
           << ",\"sample_sequence\":" << health.sample_sequence
           << ",\"sample_time_ms\":" << health.sample_time_ms
           << ",\"node_id\":" << health.node_id
           << ",\"producer_generation\":"
           << health.producer_generation << ",\"metrics\":[";
    for (std::size_t index = 0U; index < health.metrics.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& metric = health.metrics[index];
        output << "{\"metric_id\":" << metric.metric_id
               << ",\"availability\":"
               << static_cast<unsigned>(metric.availability)
               << ",\"unit\":" << static_cast<unsigned>(metric.unit)
               << ",\"value\":" << metric.value << '}';
    }
    output << "]}}}\n";
}

inline void write_traffic_status(std::ostream& output,
                                 const CanTrafficStatus& status) {
    const auto available_permille =
        status.global_capacity_ns == 0
            ? 0
            : status.global_available_ns * 1000U /
                  status.global_capacity_ns;
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"traffic-status\",\"data\":{\"traffic\":{"
           << "\"mode\":\"" << traffic_mode(status.mode) << '"'
           << ",\"arbitration_bitrate\":"
           << status.arbitration_bits_per_second
           << ",\"data_bitrate\":" << status.data_bits_per_second
           << ",\"max_utilization_permille\":"
           << status.maximum_utilization_permille
           << ",\"burst_window_ms\":" << status.burst_window_ms
           << ",\"available_permille\":" << available_permille
           << ",\"admitted_packets\":" << status.admitted_packets
           << ",\"rejected_packets\":" << status.rejected_packets
           << ",\"guaranteed_overruns\":" << status.guaranteed_overruns
           << ",\"admitted_frames\":" << status.admitted_frames
           << ",\"estimated_wire_time_ns\":"
           << status.estimated_wire_time_ns << ",\"classes\":[";
    for (std::size_t index = 0; index < status.classes.size(); ++index) {
        const auto& counters = status.classes[index];
        if (index != 0) output << ',';
        output << "{\"class\":\"" << traffic_class(index) << '"'
               << ",\"admitted_packets\":" << counters.admitted_packets
               << ",\"rejected_packets\":" << counters.rejected_packets
               << ",\"admitted_frames\":" << counters.admitted_frames
               << ",\"estimated_wire_time_ns\":"
               << counters.estimated_wire_time_ns << '}';
    }
    output << "]}}}\n";
}

inline void write_resource_list(
    std::ostream& output, std::uint32_t node_id,
    const std::vector<protocol::ResourceDescriptor>& resources) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"resource-list\",\"data\":{\"node_id\":"
           << node_id << ",\"resources\":[";
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        if (index != 0) output << ',';
        output << "{\"resource_id\":" << resource.resource_id
               << ",\"type\":\"" << resource_type(resource.type) << '"'
               << ",\"instance\":" << resource.instance
               << ",\"source\":\""
               << ((resource.flags & protocol::kResourceFlagExpanded) != 0
                       ? "expanded" : "native") << '"'
               << ",\"rx_capacity\":" << resource.rx_capacity
               << ",\"tx_capacity\":" << resource.tx_capacity << '}';
    }
    output << "]}}\n";
}

inline void write_resource_status(
    std::ostream& output, std::uint32_t node_id,
    const protocol::ResourceStatusPayload& status) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"resource-status\",\"data\":{\"node_id\":"
           << node_id << ",\"resource\":{\"resource_id\":"
           << status.resource_id
           << ",\"health\":" << static_cast<unsigned>(status.health)
           << ",\"health_name\":\"" << health_name(status.health) << '"'
           << ",\"error_flags\":" << status.error_flags
           << ",\"rx_buffered\":" << status.rx_buffered
           << ",\"tx_buffered\":" << status.tx_buffered
           << ",\"rx_overruns\":" << status.rx_overruns
           << ",\"tx_overruns\":" << status.tx_overruns << "}}}\n";
}

inline void write_runtime_snapshot(std::ostream& output,
                                   const RuntimeSnapshot& snapshot) {
    output << "{\"schema_version\":" << kSchemaVersion
           << ",\"command\":\"runtime-snapshot\",\"data\":{"
           << "\"snapshot_version\":" << snapshot.version
           << ",\"snapshot_sequence\":" << snapshot.sequence
           << ",\"traffic\":{\"mode\":\""
           << traffic_mode(snapshot.traffic.mode) << '"'
           << ",\"arbitration_bitrate\":"
           << snapshot.traffic.arbitration_bits_per_second
           << ",\"data_bitrate\":"
           << snapshot.traffic.data_bits_per_second
           << ",\"max_utilization_permille\":"
           << snapshot.traffic.maximum_utilization_permille
           << ",\"burst_window_ms\":" << snapshot.traffic.burst_window_ms
           << ",\"available_permille\":"
           << (snapshot.traffic.global_capacity_ns == 0U
                   ? 0U
                   : snapshot.traffic.global_available_ns * 1000U /
                         snapshot.traffic.global_capacity_ns)
           << ",\"admitted_packets\":" << snapshot.traffic.admitted_packets
           << ",\"rejected_packets\":" << snapshot.traffic.rejected_packets
           << ",\"guaranteed_overruns\":"
           << snapshot.traffic.guaranteed_overruns
           << ",\"admitted_frames\":" << snapshot.traffic.admitted_frames
           << ",\"estimated_wire_time_ns\":"
           << snapshot.traffic.estimated_wire_time_ns
           << ",\"classes\":[";
    for (std::size_t index = 0U; index < snapshot.traffic.classes.size();
         ++index) {
        if (index != 0U) output << ',';
        const auto& counters = snapshot.traffic.classes[index];
        output << "{\"class\":\"" << traffic_class(index) << '"'
               << ",\"admitted_packets\":" << counters.admitted_packets
               << ",\"rejected_packets\":" << counters.rejected_packets
               << ",\"admitted_frames\":" << counters.admitted_frames
               << ",\"estimated_wire_time_ns\":"
               << counters.estimated_wire_time_ns << '}';
    }
    output << "]},\"nodes\":[";
    for (std::size_t index = 0U; index < snapshot.nodes.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& node = snapshot.nodes[index];
        output << "{\"node_id\":" << node.node_id
               << ",\"online\":" << (node.online ? "true" : "false")
               << ",\"ready\":" << (node.ready ? "true" : "false")
               << ",\"board_type\":" << node.identity.board_type
               << ",\"firmware\":{\"major\":"
               << node.identity.firmware_major
               << ",\"minor\":" << node.identity.firmware_minor
               << ",\"patch\":" << node.identity.firmware_patch << '}'
               << ",\"protocol_version\":"
               << static_cast<unsigned>(node.identity.protocol_version)
               << ",\"uuid\":\"";
        write_uuid(output, node.identity.uuid);
        output << "\"}";
    }
    output << "],\"bus_health\":[";
    for (std::size_t index = 0U; index < snapshot.bus_health.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& health = snapshot.bus_health[index];
        output << "{\"node_id\":" << health.node_id
               << ",\"resource_id\":" << health.resource_id
               << ",\"last_status_valid\":"
               << (health.last_status_valid ? "true" : "false")
               << ",\"last_status\":"
               << static_cast<unsigned>(health.last_status)
               << ",\"consecutive_failures\":"
               << health.consecutive_failures
               << ",\"peak_consecutive_failures\":"
               << health.peak_consecutive_failures
               << ",\"last_result_time_us\":"
               << health.last_result_time_us << '}';
    }
    output << "],\"resources\":[";
    for (std::size_t index = 0U; index < snapshot.resources.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& resource = snapshot.resources[index];
        output << "{\"node_id\":" << resource.node_id
               << ",\"status_valid\":"
               << (resource.status_valid ? "true" : "false")
               << ",\"descriptor\":{\"resource_id\":"
               << resource.descriptor.resource_id
               << ",\"type\":\"" << resource_type(resource.descriptor.type)
               << '"' << ",\"instance\":" << resource.descriptor.instance
               << ",\"source\":\""
               << ((resource.descriptor.flags &
                    protocol::kResourceFlagExpanded) != 0U
                       ? "expanded" : "native") << '"'
               << ",\"rx_capacity\":" << resource.descriptor.rx_capacity
               << ",\"tx_capacity\":" << resource.descriptor.tx_capacity
               << "},\"status\":{\"resource_id\":"
               << resource.status.resource_id
               << ",\"health\":"
               << static_cast<unsigned>(resource.status.health)
               << ",\"health_name\":\""
               << health_name(resource.status.health) << '"'
               << ",\"error_flags\":" << resource.status.error_flags
               << ",\"rx_buffered\":" << resource.status.rx_buffered
               << ",\"tx_buffered\":" << resource.status.tx_buffered
               << ",\"rx_overruns\":" << resource.status.rx_overruns
               << ",\"tx_overruns\":" << resource.status.tx_overruns
               << "}}";
    }
    output << "],\"node_issues\":[";
    for (std::size_t index = 0U; index < snapshot.node_issues.size();
         ++index) {
        if (index != 0U) output << ',';
        output << "{\"node_id\":" << snapshot.node_issues[index].node_id
               << ",\"code\":"
               << static_cast<unsigned>(snapshot.node_issues[index].code)
               << '}';
    }
    output << "],\"clocks\":[";
    for (std::size_t index = 0U; index < snapshot.clocks.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& clock = snapshot.clocks[index];
        output << "{\"node_id\":" << clock.node_id
               << ",\"registered\":"
               << (clock.registered ? "true" : "false")
               << ",\"estimate_valid\":"
               << (clock.estimate_valid ? "true" : "false")
               << ",\"state\":\""
               << (clock.registered
                       ? runtime_clock_state(clock.state)
                       : "unregistered") << '"'
               << ",\"boot_epoch\":";
        if (clock.registered) output << clock.boot_epoch;
        else output << "null";
        output << ",\"model_generation\":";
        if (clock.registered) output << clock.model_generation;
        else output << "null";
        output << ",\"sample_count\":" << clock.sample_count
               << ",\"selected_sample_count\":"
               << clock.selected_sample_count
               << ",\"rate_deviation_ppb\":";
        if (clock.estimate_valid) output << clock.rate_deviation_ppb;
        else output << "null";
        output << ",\"drift_uncertainty_ppm\":";
        if (clock.estimate_valid) output << clock.drift_uncertainty_ppm;
        else output << "null";
        output << ",\"minimum_network_rtt_ns\":";
        if (clock.estimate_valid) output << clock.minimum_network_rtt_ns;
        else output << "null";
        output << ",\"error_bound_ns\":";
        if (clock.estimate_valid) output << clock.error_bound_ns;
        else output << "null";
        output << ",\"sample_age_ns\":";
        if (clock.estimate_valid) output << clock.sample_age_ns;
        else output << "null";
        output << ",\"last_sample_host_time_ns\":";
        if (clock.estimate_valid) output << clock.last_sample_host_time_ns;
        else output << "null";
        output << '}';
    }
    output << "]}}\n";
}

}  // namespace remotebsp::cli_json
