#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/traffic_control.hpp"
#include "remotebsp/cli_json.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

void test_classical_cost() {
    const auto empty = toolbusd::estimate_can_frame_cost(
        toolbusd::TrafficBusMode::Classical, 0, 1000000, 1000000);
    const auto full = toolbusd::estimate_can_frame_cost(
        toolbusd::TrafficBusMode::Classical, 8, 1000000, 1000000);
    CHECK(empty.nominal_phase_bits == 55);
    CHECK(empty.wire_time_ns == 55000);
    CHECK(full.nominal_phase_bits == 135);
    CHECK(full.wire_time_ns == 135000);
    CHECK(full.wire_payload_length == 8);
}

void test_fd_cost_and_dlc_padding() {
    const auto brs = toolbusd::estimate_can_frame_cost(
        toolbusd::TrafficBusMode::CanFd, 9, 500000, 2000000, true);
    const auto no_brs = toolbusd::estimate_can_frame_cost(
        toolbusd::TrafficBusMode::CanFd, 9, 500000, 2000000, false);
    CHECK(brs.wire_payload_length == 12);
    CHECK(brs.nominal_phase_bits == 43);
    CHECK(brs.data_phase_bits > 12 * 8);
    CHECK(brs.wire_time_ns < no_brs.wire_time_ns);
}

void test_usb_cost() {
    const auto cost = toolbusd::estimate_can_frame_cost(
        toolbusd::TrafficBusMode::Usb, 64, 12000000, 12000000);
    CHECK(cost.nominal_phase_bits == 0);
    CHECK(cost.data_phase_bits == 76 * 8);
    CHECK(cost.wire_payload_length == 64);
    CHECK(cost.wire_time_ns == 50667);
}

protocol::Packet packet(protocol::Command command) {
    protocol::Packet value;
    value.header.command = static_cast<std::uint16_t>(command);
    return value;
}

void test_classification() {
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::NodeAssign)) ==
          toolbusd::TrafficClass::System);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::GpioWrite)) ==
          toolbusd::TrafficClass::Interactive);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::UartWrite)) ==
          toolbusd::TrafficClass::Streaming);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::BootloaderEnter)) ==
          toolbusd::TrafficClass::Bulk);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::MotionEnqueue)) ==
          toolbusd::TrafficClass::Motion);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::MotionAbort)) ==
          toolbusd::TrafficClass::Safety);
    CHECK(toolbusd::classify_traffic(
              packet(protocol::Command::MotionClearFault)) ==
          toolbusd::TrafficClass::Interactive);
}

void test_admission_and_refill() {
    toolbusd::TrafficConfig config;
    config.maximum_utilization_permille = 100;
    config.burst_window = std::chrono::milliseconds(100);
    config.class_limit_permille.fill(1000);
    config.class_limit_permille[static_cast<std::size_t>(
        toolbusd::TrafficClass::Streaming)] = 1;
    const auto start = toolbusd::TrafficController::TimePoint{};
    toolbusd::TrafficController controller(config, start);

    const std::vector<std::size_t> burst(74, 8);
    CHECK(controller.admit(toolbusd::TrafficClass::Interactive,
                           burst,
                           toolbusd::AdmissionPolicy::Enforce,
                           start));
    CHECK(!controller.admit(toolbusd::TrafficClass::Interactive,
                            {8},
                            toolbusd::AdmissionPolicy::Enforce,
                            start));
    CHECK(!controller.admit(toolbusd::TrafficClass::Streaming,
                            {8},
                            toolbusd::AdmissionPolicy::Enforce,
                            start + std::chrono::milliseconds(2)));
    CHECK(controller.admit(toolbusd::TrafficClass::Interactive,
                           {8},
                           toolbusd::AdmissionPolicy::Enforce,
                           start + std::chrono::milliseconds(2)));
    CHECK(controller.admit(toolbusd::TrafficClass::Safety,
                           burst,
                           toolbusd::AdmissionPolicy::Guaranteed,
                           start + std::chrono::milliseconds(2)));

    const auto status =
        controller.snapshot(start + std::chrono::milliseconds(2));
    CHECK(status.admitted_packets == 3);
    CHECK(status.rejected_packets == 2);
    CHECK(status.guaranteed_overruns == 1);
    CHECK(status.admitted_frames == 149);
    CHECK(status.classes[static_cast<std::size_t>(
              toolbusd::TrafficClass::Streaming)]
              .rejected_packets == 1);
}

void test_ipc_round_trip() {
    toolbusd::TrafficSnapshot input;
    input.mode = toolbusd::TrafficBusMode::CanFd;
    input.arbitration_bits_per_second = 500000;
    input.data_bits_per_second = 2000000;
    input.maximum_utilization_permille = 700;
    input.burst_window_ms = 250;
    input.global_capacity_ns = 175000000;
    input.global_available_ns = 123;
    input.admitted_packets = 10;
    input.rejected_packets = 2;
    input.guaranteed_overruns = 1;
    input.admitted_frames = 20;
    input.estimated_wire_time_ns = 456;
    input.classes[4] = {3, 4, 5, 6};

    const auto output = toolbusd::decode_ipc_traffic_status(
        toolbusd::encode_ipc_traffic_status(input));
    CHECK(output.mode == toolbusd::TrafficBusMode::CanFd);
    CHECK(output.global_capacity_ns == 175000000);
    CHECK(output.rejected_packets == 2);
    CHECK(output.classes[4].estimated_wire_time_ns == 6);

    input.mode = toolbusd::TrafficBusMode::Usb;
    const auto usb_output = toolbusd::decode_ipc_traffic_status(
        toolbusd::encode_ipc_traffic_status(input));
    CHECK(usb_output.mode == toolbusd::TrafficBusMode::Usb);
}

void test_versioned_cli_json() {
    DiscoveredNode node;
    node.node_id = 7;
    node.online = true;
    node.ready = false;
    node.identity.uuid[0] = 0xab;
    node.identity.board_type = 0x431;
    node.identity.firmware_major = 1;
    node.identity.firmware_minor = 2;
    node.identity.firmware_patch = 3;
    node.identity.protocol_version = 4;
    std::ostringstream nodes;
    cli_json::write_node_list(nodes, {node});
    CHECK(nodes.str().find(
        "{\"schema_version\":1,\"command\":\"node-list\"") == 0);
    CHECK(nodes.str().find("\"node_id\":7") != std::string::npos);
    CHECK(nodes.str().find("\"online\":true") != std::string::npos);
    CHECK(nodes.str().find("\"ready\":false") != std::string::npos);
    CHECK(nodes.str().find("\"protocol_version\":4") != std::string::npos);
    CHECK(nodes.str().find("\"uuid\":\"ab000000000000000000000000000000\"") !=
          std::string::npos);

    CanTrafficStatus traffic;
    traffic.mode = LinkTrafficMode::CanFd;
    traffic.arbitration_bits_per_second = 1000000;
    traffic.data_bits_per_second = 5000000;
    traffic.global_capacity_ns = 100;
    traffic.global_available_ns = 75;
    traffic.admitted_packets = std::numeric_limits<std::uint64_t>::max();
    traffic.classes[0].admitted_packets = 9;
    std::ostringstream traffic_output;
    cli_json::write_traffic_status(traffic_output, traffic);
    CHECK(traffic_output.str().find("\"command\":\"traffic-status\"") !=
          std::string::npos);
    CHECK(traffic_output.str().find("\"mode\":\"fd\"") !=
          std::string::npos);
    CHECK(traffic_output.str().find("\"available_permille\":750") !=
          std::string::npos);
    CHECK(traffic_output.str().find(
        "\"admitted_packets\":18446744073709551615") !=
          std::string::npos);
    CHECK(traffic_output.str().find("\"class\":\"safety\"") !=
          std::string::npos);

    protocol::ResourceDescriptor descriptor;
    descriptor.resource_id = 0x01000001;
    descriptor.type = protocol::ResourceType::Gpio;
    descriptor.instance = 2;
    descriptor.flags = protocol::kResourceFlagExpanded;
    descriptor.rx_capacity = 3;
    descriptor.tx_capacity = 4;
    std::ostringstream resources;
    cli_json::write_resource_list(resources, 7, {descriptor});
    CHECK(resources.str().find("\"command\":\"resource-list\"") !=
          std::string::npos);
    CHECK(resources.str().find("\"resource_id\":16777217") !=
          std::string::npos);
    CHECK(resources.str().find("\"source\":\"expanded\"") !=
          std::string::npos);

    protocol::ResourceStatusPayload status;
    status.resource_id = descriptor.resource_id;
    status.health = protocol::ResourceHealth::Degraded;
    status.error_flags = protocol::kResourceErrorRxOverflow;
    status.rx_overruns = 5;
    std::ostringstream resource_status;
    cli_json::write_resource_status(resource_status, 7, status);
    CHECK(resource_status.str().find(
        "\"command\":\"resource-status\"") != std::string::npos);
    CHECK(resource_status.str().find("\"health_name\":\"degraded\"") !=
          std::string::npos);
    CHECK(resource_status.str().find("\"rx_overruns\":5") !=
          std::string::npos);

    const IpcErrorException ipc_error(
        toolbusd::kIpcErrorEnvelopeVersion,
        static_cast<std::uint16_t>(toolbusd::IpcErrorCode::LeaseNotFound),
        static_cast<std::uint8_t>(toolbusd::IpcErrorCategory::Conflict),
        false, false, "租约 \"lease-a\" 不存在");
    std::ostringstream error_output;
    cli_json::write_ipc_error(
        error_output, "runtime-control-release", ipc_error);
    CHECK(error_output.str().find(
        "{\"schema_version\":1,\"command\":\"runtime-control-release\"") ==
          0U);
    CHECK(error_output.str().find("\"ipc_error_version\":1") !=
          std::string::npos);
    CHECK(error_output.str().find("\"code\":103") != std::string::npos);
    CHECK(error_output.str().find("\"category\":4") != std::string::npos);
    CHECK(error_output.str().find("\"retryable\":false") !=
          std::string::npos);
    CHECK(error_output.str().find("\"possibly_committed\":false") !=
          std::string::npos);
    CHECK(error_output.str().find("租约 \\\"lease-a\\\" 不存在") !=
          std::string::npos);
}

}

int main() {
    test_classical_cost();
    test_fd_cost_and_dlc_padding();
    test_usb_cost();
    test_classification();
    test_admission_and_refill();
    test_ipc_round_trip();
    test_versioned_cli_json();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "CAN 业务优先级与带宽准入测试通过\n";
    return 0;
}
