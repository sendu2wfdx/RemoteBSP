#pragma once

#include "client.hpp"

#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

namespace remotebsp::cli_json {

constexpr std::uint32_t kSchemaVersion = 1;

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
    output << "]}}\n";
}

}  // namespace remotebsp::cli_json
