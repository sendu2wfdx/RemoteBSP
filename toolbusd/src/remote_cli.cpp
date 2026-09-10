#include "remotebsp/client.hpp"
#include "remotebsp/cli_json.hpp"
#include "remotebsp/tmc2209.hpp"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint32_t parse_u32(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const unsigned long value = std::stoul(text, &consumed, 0);
    if (consumed != text.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t parse_u64(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const unsigned long long value =
        std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::uint64_t>(value);
}

std::int32_t parse_i32(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed, 0);
    if (consumed != text.size() ||
        value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " 无效");
    }
    return static_cast<std::int32_t>(value);
}

remotebsp::protocol::MotionAxisMovePayload parse_motion_move(
    const std::string& text) {
    const auto delimiter = text.find(':');
    if (delimiter == std::string::npos || delimiter == 0 ||
        delimiter + 1U >= text.size() ||
        text.find(':', delimiter + 1U) != std::string::npos) {
        throw std::invalid_argument(
            "运动轴参数必须使用 <资源ID>:<有符号步数>");
    }
    return {
        parse_u32(text.substr(0, delimiter), "运动轴资源 ID"),
        parse_i32(text.substr(delimiter + 1U), "运动轴步数")};
}

remotebsp::MotionGroupMemberPlan parse_motion_group_member(
    const std::string& text, std::uint64_t start_time_ns) {
    std::array<std::size_t, 4U> delimiters{};
    std::size_t search_from = 0U;
    for (auto& delimiter : delimiters) {
        delimiter = text.find('/', search_from);
        if (delimiter == std::string::npos || delimiter == search_from) {
            throw std::invalid_argument(
                "运动组成员必须使用 节点/序号/持续ns/final|more/轴:步数,...");
        }
        search_from = delimiter + 1U;
    }
    if (search_from >= text.size() ||
        text.find('/', search_from) != std::string::npos) {
        throw std::invalid_argument("运动组成员字段数量无效");
    }
    remotebsp::MotionGroupMemberPlan member;
    member.node_id = parse_u32(text.substr(0U, delimiters[0U]),
                               "运动组节点 ID");
    member.segment.sequence = parse_u32(
        text.substr(delimiters[0U] + 1U,
                    delimiters[1U] - delimiters[0U] - 1U),
        "运动组运动段序号");
    member.segment.start_time_ns = start_time_ns;
    member.segment.duration_ns = parse_u64(
        text.substr(delimiters[1U] + 1U,
                    delimiters[2U] - delimiters[1U] - 1U),
        "运动组运动段持续时间");
    const auto final = text.substr(
        delimiters[2U] + 1U,
        delimiters[3U] - delimiters[2U] - 1U);
    if (final == "final") {
        member.segment.final_segment = true;
    } else if (final == "more") {
        member.segment.final_segment = false;
    } else {
        throw std::invalid_argument(
            "运动组运动段结束标志必须是 final 或 more");
    }
    const auto axes = text.substr(delimiters[3U] + 1U);
    std::size_t offset = 0U;
    while (offset < axes.size()) {
        const auto comma = axes.find(',', offset);
        const auto end = comma == std::string::npos ? axes.size() : comma;
        if (end == offset) {
            throw std::invalid_argument("运动组轴列表包含空项");
        }
        member.segment.axes.push_back(
            parse_motion_move(axes.substr(offset, end - offset)));
        if (comma == std::string::npos) {
            break;
        }
        offset = comma + 1U;
    }
    return member;
}

std::uint8_t parse_hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("十六进制数据包含非法字符");
}

std::vector<std::uint8_t> parse_hex(const std::string& text) {
    if (text.empty() || (text.size() % 2U) != 0U) {
        throw std::invalid_argument(
            "十六进制数据必须非空，且每个字节使用两个字符");
    }
    std::vector<std::uint8_t> result;
    result.reserve(text.size() / 2U);
    for (std::size_t index = 0; index < text.size(); index += 2U) {
        result.push_back(static_cast<std::uint8_t>(
            (parse_hex_digit(text[index]) << 4U) |
            parse_hex_digit(text[index + 1U])));
    }
    return result;
}

std::array<std::uint8_t, 16> parse_hex_id(const std::string& text,
                                          const char* name) {
    const auto bytes = parse_hex(text);
    if (bytes.size() != 16U) {
        throw std::invalid_argument(std::string(name) +
                                    " 必须是32位十六进制");
    }
    std::array<std::uint8_t, 16> result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}

std::uint16_t parse_device_parameter_id(const std::string& text) {
    if (text == "serial-number" || text == "sn") {
        return RBSP_DEVICE_PARAM_SERIAL_NUMBER;
    }
    if (text == "device-uuid" || text == "uuid") {
        return RBSP_DEVICE_PARAM_DEVICE_UUID;
    }
    if (text == "hardware-revision" || text == "hw-rev") {
        return RBSP_DEVICE_PARAM_HARDWARE_REVISION;
    }
    if (text == "manufacturing-batch" || text == "batch") {
        return RBSP_DEVICE_PARAM_MANUFACTURING_BATCH;
    }
    if (text == "manufacturing-date" || text == "date") {
        return RBSP_DEVICE_PARAM_MANUFACTURING_DATE;
    }
    if (text == "device-name" || text == "name") {
        return RBSP_DEVICE_PARAM_DEVICE_NAME;
    }
    if (text.rfind("adc", 0U) == 0U && text.size() > 3U) {
        const auto channel = parse_u32(text.substr(3U), "ADC通道");
        if (channel >= RBSP_DEVICE_PARAM_ADC_CHANNEL_COUNT) {
            throw std::invalid_argument("ADC校准通道超出0～15");
        }
        return static_cast<std::uint16_t>(
            RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + channel);
    }
    const auto value = parse_u32(text, "设备参数ID");
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("设备参数ID超过16位范围");
    }
    return static_cast<std::uint16_t>(value);
}

const char* device_parameter_name(std::uint16_t id) {
    switch (id) {
        case RBSP_DEVICE_PARAM_SERIAL_NUMBER: return "serial-number";
        case RBSP_DEVICE_PARAM_DEVICE_UUID: return "device-uuid";
        case RBSP_DEVICE_PARAM_HARDWARE_REVISION: return "hardware-revision";
        case RBSP_DEVICE_PARAM_MANUFACTURING_BATCH:
            return "manufacturing-batch";
        case RBSP_DEVICE_PARAM_MANUFACTURING_DATE:
            return "manufacturing-date";
        case RBSP_DEVICE_PARAM_DEVICE_NAME: return "device-name";
        default: return "adc-calibration-or-unknown";
    }
}

void print_device_parameter_status(
    const remotebsp::protocol::DeviceParameterStatus& status) {
    const auto unlocked = static_cast<std::uint16_t>(
        remotebsp::protocol::DeviceParameterStatusFlag::MaintenanceUnlocked);
    const auto restart = static_cast<std::uint16_t>(
        remotebsp::protocol::DeviceParameterStatusFlag::RestartRequired);
    std::cout << "version=" << status.version
              << " generation=" << status.generation
              << " stored=" << status.stored_count
              << " definitions=" << status.definition_count
              << " maintenance_unlocked="
              << ((status.flags & unlocked) != 0U ? "yes" : "no")
              << " restart_required="
              << ((status.flags & restart) != 0U ? "yes" : "no")
              << " store_error=" << static_cast<unsigned>(status.last_error)
              << '\n';
}

const char* resource_type(remotebsp::protocol::ResourceType type) {
    using remotebsp::protocol::ResourceType;
    switch (type) {
        case ResourceType::Gpio: return "gpio";
        case ResourceType::Uart: return "uart";
        case ResourceType::Spi: return "spi";
        case ResourceType::I2c: return "i2c";
        case ResourceType::Adc: return "adc";
        case ResourceType::Pwm: return "pwm";
        case ResourceType::Timer: return "timer";
        case ResourceType::Storage: return "storage";
        case ResourceType::StepgenAxis: return "stepgen-axis";
        case ResourceType::TimedBitstream: return "timed-bitstream";
        case ResourceType::I2cBus: return "i2c-bus";
        case ResourceType::I2cDevice: return "i2c-device";
        case ResourceType::SpiBus: return "spi-bus";
        case ResourceType::SpiDevice: return "spi-device";
        case ResourceType::Stream: return "stream";
    }
    return "unknown";
}

const char* resource_source(std::uint16_t flags) {
    return (flags & remotebsp::protocol::kResourceFlagExpanded) != 0
               ? "expanded"
               : "native";
}

const char* health_name(remotebsp::protocol::ResourceHealth health) {
    using remotebsp::protocol::ResourceHealth;
    switch (health) {
        case ResourceHealth::Normal: return "normal";
        case ResourceHealth::Busy: return "busy";
        case ResourceHealth::Degraded: return "degraded";
        case ResourceHealth::Failed: return "failed";
        case ResourceHealth::Disabled: return "disabled";
    }
    return "unknown";
}

void print_resource(const remotebsp::protocol::ResourceDescriptor& resource,
                    bool include_type) {
    std::cout << "resource_id=0x" << std::hex << resource.resource_id
              << std::dec;
    if (include_type) {
        std::cout << " type=" << resource_type(resource.type);
    }
    std::cout << " instance=" << resource.instance
              << " source=" << resource_source(resource.flags)
              << " rx_capacity=" << resource.rx_capacity
              << " tx_capacity=" << resource.tx_capacity << '\n';
}

void print_hex(const std::vector<std::uint8_t>& data) {
    static constexpr char digits[] = "0123456789abcdef";
    for (const auto value : data) {
        std::cout << digits[value >> 4U] << digits[value & 0x0FU];
    }
}

const char* lease_mode_name(
    remotebsp::protocol::ResourceLeaseMode mode) {
    using remotebsp::protocol::ResourceLeaseMode;
    switch (mode) {
        case ResourceLeaseMode::None: return "none";
        case ResourceLeaseMode::SharedRead: return "shared-read";
        case ResourceLeaseMode::Exclusive: return "exclusive";
    }
    return "unknown";
}

const char* traffic_class_name(std::size_t index) {
    static constexpr const char* names[] = {
        "safety", "motion", "system",
        "interactive", "streaming", "bulk"};
    return index < std::size(names) ? names[index] : "unknown";
}

const char* motion_state_name(
    remotebsp::protocol::MotionStatePayload state) {
    using remotebsp::protocol::MotionStatePayload;
    switch (state) {
        case MotionStatePayload::Idle: return "idle";
        case MotionStatePayload::Armed: return "armed";
        case MotionStatePayload::Running: return "running";
        case MotionStatePayload::Faulted: return "faulted";
    }
    return "unknown";
}

const char* motion_fault_name(
    remotebsp::protocol::MotionFaultPayload fault) {
    using remotebsp::protocol::MotionFaultPayload;
    switch (fault) {
        case MotionFaultPayload::None: return "none";
        case MotionFaultPayload::Aborted: return "aborted";
        case MotionFaultPayload::LimitTriggered: return "limit";
        case MotionFaultPayload::QueueUnderrun: return "queue-underrun";
        case MotionFaultPayload::TimingDeadlineMissed:
            return "timing-deadline-missed";
    }
    return "unknown";
}

const char* motion_group_state_name(
    remotebsp::MotionGroupTransactionState state) {
    using State = remotebsp::MotionGroupTransactionState;
    switch (state) {
        case State::Idle: return "idle";
        case State::Preparing: return "preparing";
        case State::Ready: return "ready";
        case State::Committing: return "committing";
        case State::Committed: return "committed";
        case State::Aborting: return "aborting";
        case State::Aborted: return "aborted";
    }
    return "unknown";
}

void print_motion_group_status(
    const remotebsp::MotionGroupTransactionStatus& status) {
    std::cout << "transaction_id=" << status.transaction_id
              << " group_id=" << status.group_id
              << " plan_generation=" << status.plan_generation
              << " state=" << motion_group_state_name(status.state)
              << " abort_reason="
              << (status.abort_reason.has_value()
                      ? static_cast<unsigned>(*status.abort_reason)
                      : 0U)
              << " members=" << status.member_count
              << " ready=" << status.ready_count
              << " committed=" << status.committed_count
              << " pending_requests=" << status.pending_request_count
              << " commit_dispatched="
              << (status.commit_dispatched ? 1 : 0)
              << " abort_scope="
              << (status.abort_is_best_effort
                      ? "best-effort-after-commit"
                      : "pre-commit")
              << '\n';
}

void print_lease(
    const remotebsp::protocol::ResourceLeaseInfo& lease) {
    std::cout << "resource_id=0x" << std::hex << lease.resource_id
              << " lease_id=0x" << lease.lease_id << std::dec
              << " owner_session_id=" << lease.owner_session_id
              << " mode=" << lease_mode_name(lease.mode)
              << " granted_ms=" << lease.granted_duration_ms
              << " remaining_ms=" << lease.remaining_ms
              << " active_count=" << lease.active_lease_count << '\n';
}

void print_usage() {
    std::cerr
        << "用法: remote-cli [--json] [--socket 路径] [--node 节点ID] <命令> [参数]\n"
        << "命令:\n"
        << "  ping <文本>\n"
        << "  bootloader-enter\n"
        << "  bootloader-enter-usb\n"
        << "  node-list\n"
        << "  traffic-status\n"
        << "  daemon-identity\n"
        << "  runtime-control-acquire <daemon实例ID> <控制租约ID> "
           "<预期节点UUID> <调用者ID> <GPIO资源ID> <租约ms>\n"
        << "  runtime-gpio-write <daemon实例ID> <控制租约ID> "
           "<预期节点UUID> <调用者ID> <GPIO资源ID> <幂等键> <0|1>\n"
        << "  runtime-control-release <daemon实例ID> <控制租约ID> "
           "<调用者ID>\n"
        << "  runtime-snapshot [最大资源数] [总超时毫秒]\n"
        << "  event-wait\n"
        << "  get-info | get-capability\n"
        << "  resource-list\n"
        << "  resource-describe <资源ID>\n"
        << "  resource-status <资源ID>\n"
        << "  resource-reset <资源ID>\n"
        << "  resource-contract <资源ID>\n"
        << "  resource-acquire <资源ID> <毫秒> [exclusive|shared-read]\n"
        << "  resource-renew <资源ID> <租约ID> <毫秒>\n"
        << "  resource-release <资源ID> <租约ID>\n"
        << "  resource-lease-status <资源ID>\n"
        << "  param-status\n"
        << "  param-list\n"
        << "  param-get <名称|参数ID>\n"
        << "  param-set <名称|参数ID> <文本|hex:十六进制>\n"
        << "  gpio-create <引脚> <input|output> [初始电平]\n"
        << "  gpio-read <对象ID>\n"
        << "  gpio-write <对象ID> <0|1>\n"
        << "  pwm-create <通道> <频率Hz> <占空比0..10000> [active-high|active-low]\n"
        << "  pwm-write <对象ID> <占空比0..10000>\n"
        << "  pwm-stop <对象ID>\n"
        << "  timed-bitstream-create <通道> <位周期ns> <0高电平ns> <1高电平ns> <复位us>\n"
        << "  timed-bitstream-write-hex <对象ID> <位数> <十六进制字节串>\n"
        << "  timed-bitstream-abort <对象ID>\n"
        << "  ws2812-create <通道>\n"
        << "  ws2812-write <对象ID> <RRGGBB...>\n"
        << "  uart-create <端口> <波特率> [数据位] [none|odd|even] [停止位] [poll|stream]\n"
        << "  uart-read <对象ID> <最大长度>\n"
        << "  uart-stream-read <对象ID> <最大长度> [超时毫秒]\n"
        << "  uart-write <对象ID> <文本>\n"
        << "  uart-write-hex <对象ID> <十六进制字节串>\n"
        << "  tmc2209-read <UART对象ID> <寄存器> [节点地址]\n"
        << "  tmc2209-write <UART对象ID> <寄存器> <32位值> [节点地址]\n"
        << "  uart-write-all <对象ID> <文本> [超时毫秒]\n"
        << "  uart-write-all-hex <对象ID> <十六进制字节串> [超时毫秒]\n"
        << "  motion-enqueue <序号> <开始ns|auto> <持续ns> "
           "<final|more> <资源ID:步数>...\n"
        << "  motion-status\n"
        << "  motion-contract\n"
        << "  motion-abort\n"
        << "  motion-clear-fault\n"
        << "  motion-group-submit <事务ID> <组ID> <计划代数> "
           "<开始ns|auto> <64位十六进制摘要> "
           "<节点/序号/持续ns/final|more/资源ID:步数,...>...\n"
        << "  motion-group-status <事务ID> <组ID> <计划代数>\n"
        << "  motion-group-cancel <事务ID> <组ID> <计划代数>\n";
}

int run(const std::vector<std::string>& arguments,
        const std::string& socket_path, std::uint32_t node_id,
        bool json_output) {
    if (arguments.empty()) {
        print_usage();
        return 2;
    }
    const auto& name = arguments[0];
    remotebsp::Client client(socket_path, node_id);

    if (name == "daemon-identity" && arguments.size() == 1) {
        const auto identity = client.daemon_identity();
        if (json_output) {
            remotebsp::cli_json::write_daemon_identity(
                std::cout, identity);
        } else {
            std::cout << "ipc_version=" << identity.version
                      << " instance_id=";
            const std::vector<std::uint8_t> bytes(
                identity.instance_id.begin(), identity.instance_id.end());
            print_hex(bytes);
            std::cout << '\n';
        }
        return 0;
    }

    if (name == "runtime-control-acquire" && arguments.size() == 7) {
        client.runtime_control_acquire(
            parse_hex_id(arguments[1], "daemon实例ID"),
            parse_hex_id(arguments[2], "控制租约ID"),
            parse_hex_id(arguments[3], "预期节点UUID"), arguments[4],
            parse_u32(arguments[5], "GPIO资源ID"),
            parse_u32(arguments[6], "租约毫秒"));
        if (json_output) {
            std::cout << "{\"schema_version\":1,\"command\":"
                         "\"runtime-control-acquire\",\"data\":{}}\n";
        } else {
            std::cout << "ok\n";
        }
        return 0;
    }

    if (name == "runtime-gpio-write" && arguments.size() == 8) {
        const auto value = parse_u32(arguments[7], "GPIO 电平");
        if (value > 1U) {
            throw std::invalid_argument("GPIO 电平必须是0或1");
        }
        const auto result = client.runtime_gpio_write(
            parse_hex_id(arguments[1], "daemon实例ID"),
            parse_hex_id(arguments[2], "控制租约ID"),
            parse_hex_id(arguments[3], "预期节点UUID"), arguments[4],
            parse_u32(arguments[5], "GPIO资源ID"),
            arguments[6], value != 0U);
        if (json_output) {
            std::cout << "{\"schema_version\":1,\"command\":"
                         "\"runtime-gpio-write\",\"data\":{"
                         "\"object_id\":"
                      << result.object_id << ",\"value\":"
                      << (result.value ? "true" : "false")
                      << ",\"replayed\":"
                      << (result.replayed ? "true" : "false")
                      << "}}\n";
        } else {
            std::cout << "object_id=" << result.object_id
                      << " value=" << (result.value ? 1 : 0)
                      << " replayed=" << (result.replayed ? "yes" : "no")
                      << '\n';
        }
        return 0;
    }

    if (name == "runtime-control-release" && arguments.size() == 4) {
        client.runtime_control_release(
            parse_hex_id(arguments[1], "daemon实例ID"),
            parse_hex_id(arguments[2], "控制租约ID"), arguments[3]);
        if (json_output) {
            std::cout << "{\"schema_version\":1,\"command\":"
                         "\"runtime-control-release\",\"data\":{}}\n";
        } else {
            std::cout << "ok\n";
        }
        return 0;
    }

    if (name == "traffic-status" && arguments.size() == 1) {
        const auto status = client.traffic_status();
        if (json_output) {
            remotebsp::cli_json::write_traffic_status(std::cout, status);
            return 0;
        }
        const auto available_permille =
            status.global_capacity_ns == 0
                ? 0
                : static_cast<std::uint64_t>(
                      status.global_available_ns * 1000U /
                      status.global_capacity_ns);
        const char* mode_name = "classical";
        if (status.mode == remotebsp::LinkTrafficMode::CanFd) {
            mode_name = "fd";
        } else if (status.mode == remotebsp::LinkTrafficMode::Usb) {
            mode_name = "usb";
        }
        std::cout
            << "mode=" << mode_name
            << " arbitration_bitrate="
            << status.arbitration_bits_per_second
            << " data_bitrate=" << status.data_bits_per_second
            << " max_utilization_permille="
            << status.maximum_utilization_permille
            << " burst_window_ms=" << status.burst_window_ms
            << " available_permille=" << available_permille
            << " admitted_packets=" << status.admitted_packets
            << " rejected_packets=" << status.rejected_packets
            << " guaranteed_overruns=" << status.guaranteed_overruns
            << " admitted_frames=" << status.admitted_frames
            << " estimated_wire_time_ns="
            << status.estimated_wire_time_ns << '\n';
        for (std::size_t index = 0; index < status.classes.size();
             ++index) {
            const auto& counters = status.classes[index];
            std::cout << "class=" << traffic_class_name(index)
                      << " admitted_packets="
                      << counters.admitted_packets
                      << " rejected_packets="
                      << counters.rejected_packets
                      << " admitted_frames="
                      << counters.admitted_frames
                      << " estimated_wire_time_ns="
                      << counters.estimated_wire_time_ns << '\n';
        }
        return 0;
    }
    if (name == "node-list" && arguments.size() == 1) {
        const auto nodes = client.list_nodes();
        if (json_output) {
            remotebsp::cli_json::write_node_list(std::cout, nodes);
            return 0;
        }
        for (const auto& node : nodes) {
            std::cout << "node_id=" << node.node_id
                      << " online=" << (node.online ? 1 : 0)
                      << " ready=" << (node.ready ? 1 : 0)
                      << " board_type=0x" << std::hex
                      << node.identity.board_type << std::dec
                      << " firmware=" << node.identity.firmware_major << '.'
                      << node.identity.firmware_minor << '.'
                      << node.identity.firmware_patch
                      << " protocol_version="
                      << static_cast<unsigned>(
                             node.identity.protocol_version)
                      << " uuid=";
            const std::vector<std::uint8_t> uuid(
                node.identity.uuid.begin(), node.identity.uuid.end());
            print_hex(uuid);
            std::cout << '\n';
        }
        return 0;
    }
    if (name == "runtime-snapshot" &&
        (arguments.size() == 1 || arguments.size() == 3)) {
        const auto maximum_resources = arguments.size() == 3
            ? parse_u32(arguments[1], "最大资源数") : 128U;
        const auto timeout_ms = arguments.size() == 3
            ? parse_u32(arguments[2], "快照总超时") : 5000U;
        if (maximum_resources == 0U || maximum_resources > 128U ||
            timeout_ms == 0U || timeout_ms > 5000U) {
            throw std::invalid_argument(
                "Runtime 快照最大资源数或总超时超过上限");
        }
        const auto snapshot = client.runtime_snapshot(
            static_cast<std::uint16_t>(maximum_resources), timeout_ms);
        if (json_output) {
            remotebsp::cli_json::write_runtime_snapshot(
                std::cout, snapshot);
        } else {
            std::cout << "snapshot_version=" << snapshot.version
                      << " snapshot_sequence=" << snapshot.sequence
                      << " nodes=" << snapshot.nodes.size()
                      << " resources=" << snapshot.resources.size()
                      << '\n';
        }
        return 0;
    }
    if (name == "event-wait" && arguments.size() == 1) {
        const auto event = client.next_event();
        if (!event.has_value()) {
            std::cout << "timeout\n";
            return 3;
        }
        std::cout << "command=0x" << std::hex
                  << event->header.command
                  << " resource_id=0x" << event->header.object_id
                  << std::dec << " data_hex=";
        print_hex(event->payload);
        std::cout << '\n';
        return 0;
    }
    if (name == "ping" && arguments.size() == 2) {
        const std::vector<std::uint8_t> input(arguments[1].begin(),
                                               arguments[1].end());
        const auto output = client.ping(input);
        std::cout << "pong="
                  << std::string(output.begin(), output.end()) << '\n';
        return 0;
    }
    if (name == "bootloader-enter" && arguments.size() == 1) {
        client.enter_bootloader();
        std::cout << "ok\n";
        return 0;
    }
    if (name == "bootloader-enter-usb" && arguments.size() == 1) {
        client.enter_usb_bootloader();
        std::cout << "ok\n";
        return 0;
    }
    if (name == "get-info" && arguments.size() == 1) {
        const auto info = client.get_info();
        std::cout << "uuid=";
        const std::vector<std::uint8_t> uuid(info.uuid.begin(),
                                              info.uuid.end());
        print_hex(uuid);
        std::cout << '\n'
                  << "firmware=" << info.firmware_major << '.'
                  << info.firmware_minor << '.' << info.firmware_patch << '\n'
                  << "board_type=0x" << std::hex << info.board_type
                  << std::dec << '\n'
                  << "protocol_version="
                  << static_cast<unsigned>(info.protocol_version) << '\n';
        return 0;
    }
    if (name == "get-capability" && arguments.size() == 1) {
        // 请求失败时不要提前输出半截结果，避免错误信息看起来像协议损坏。
        const auto capabilities = client.get_capabilities();
        std::cout << "capabilities=0x" << std::hex << capabilities
                  << std::dec << '\n';
        return 0;
    }
    if (name == "param-status" && arguments.size() == 1) {
        print_device_parameter_status(client.device_parameter_status());
        return 0;
    }
    if (name == "param-list" && arguments.size() == 1) {
        for (const auto& parameter : client.list_device_parameters()) {
            std::cout << "id=0x" << std::hex << parameter.id << std::dec
                      << " name=" << device_parameter_name(parameter.id)
                      << " type=" << static_cast<unsigned>(parameter.type)
                      << " flags=0x" << std::hex
                      << static_cast<unsigned>(parameter.flags) << std::dec
                      << " length=" << parameter.minimum_length << ".."
                      << parameter.maximum_length << '\n';
        }
        return 0;
    }
    if (name == "param-get" && arguments.size() == 2) {
        const auto parameter = client.read_device_parameter(
            parse_device_parameter_id(arguments[1]));
        std::cout << "id=0x" << std::hex << parameter.id << std::dec
                  << " name=" << device_parameter_name(parameter.id)
                  << " generation=" << parameter.generation
                  << " type=" << static_cast<unsigned>(parameter.type)
                  << " value=";
        if (parameter.type == RBSP_DEVICE_PARAM_TYPE_UTF8) {
            std::cout.write(
                reinterpret_cast<const char*>(parameter.value.data()),
                static_cast<std::streamsize>(parameter.value.size()));
        } else {
            print_hex(parameter.value);
        }
        std::cout << '\n';
        return 0;
    }
    if (name == "param-set" && arguments.size() == 3) {
        const auto id = parse_device_parameter_id(arguments[1]);
        std::vector<std::uint8_t> value;
        if (arguments[2].rfind("hex:", 0U) == 0U) {
            value = parse_hex(arguments[2].substr(4U));
        } else {
            value.assign(arguments[2].begin(), arguments[2].end());
        }
        const auto status = client.write_device_parameter(id, value);
        print_device_parameter_status(status);
        return 0;
    }
    if (name == "resource-list" && arguments.size() == 1) {
        const auto resources = client.list_resources();
        if (json_output) {
            remotebsp::cli_json::write_resource_list(
                std::cout, node_id, resources);
            return 0;
        }
        for (const auto& resource : resources) {
            print_resource(resource, true);
        }
        return 0;
    }
    if (name == "resource-describe" && arguments.size() == 2) {
        print_resource(
            client.describe_resource(parse_u32(arguments[1], "资源 ID")),
            false);
        return 0;
    }
    if (name == "resource-status" && arguments.size() == 2) {
        const auto status =
            client.resource_status(parse_u32(arguments[1], "资源 ID"));
        if (json_output) {
            remotebsp::cli_json::write_resource_status(
                std::cout, node_id, status);
            return 0;
        }
        std::cout << "resource_id=0x" << std::hex << status.resource_id
                  << std::dec << " health="
                  << static_cast<unsigned>(status.health)
                  << " error_flags=0x" << std::hex << status.error_flags
                  << std::dec
                  << " health_name=" << health_name(status.health)
                  << " rx_buffered=" << status.rx_buffered
                  << " tx_buffered=" << status.tx_buffered
                  << " rx_overruns=" << status.rx_overruns
                  << " tx_overruns=" << status.tx_overruns << '\n';
        return 0;
    }
    if (name == "resource-reset" && arguments.size() == 2) {
        client.reset_resource(parse_u32(arguments[1], "资源 ID"));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "resource-contract" && arguments.size() == 2) {
        const auto contract =
            client.resource_contract(parse_u32(arguments[1], "资源 ID"));
        std::cout
            << "resource_id=0x" << std::hex << contract.resource_id
            << " access_flags=0x" << contract.access_flags << std::dec
            << " version=" << contract.version
            << " timing_resolution_ns="
            << contract.timing_resolution_ns
            << " worst_case_latency_us="
            << contract.worst_case_latency_us
            << " max_operations_per_second="
            << contract.maximum_operations_per_second
            << " queue_capacity=" << contract.queue_capacity
            << " max_rx_bps="
            << contract.maximum_rx_bits_per_second
            << " max_tx_bps="
            << contract.maximum_tx_bits_per_second << '\n';
        return 0;
    }
    if (name == "resource-acquire" &&
        (arguments.size() == 3 || arguments.size() == 4)) {
        auto mode =
            remotebsp::protocol::ResourceLeaseMode::Exclusive;
        if (arguments.size() == 4) {
            if (arguments[3] == "exclusive") {
                mode =
                    remotebsp::protocol::ResourceLeaseMode::Exclusive;
            } else if (arguments[3] == "shared-read") {
                mode =
                    remotebsp::protocol::ResourceLeaseMode::SharedRead;
            } else {
                throw std::invalid_argument(
                    "租约模式必须是 exclusive 或 shared-read");
            }
        }
        print_lease(client.acquire_resource(
            parse_u32(arguments[1], "资源 ID"),
            parse_u32(arguments[2], "租约时长"), mode));
        return 0;
    }
    if (name == "resource-renew" && arguments.size() == 4) {
        print_lease(client.renew_resource(
            parse_u32(arguments[1], "资源 ID"),
            parse_u64(arguments[2], "租约 ID"),
            parse_u32(arguments[3], "租约时长")));
        return 0;
    }
    if (name == "resource-release" && arguments.size() == 3) {
        client.release_resource(
            parse_u32(arguments[1], "资源 ID"),
            parse_u64(arguments[2], "租约 ID"));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "resource-lease-status" && arguments.size() == 2) {
        print_lease(client.resource_lease_status(
            parse_u32(arguments[1], "资源 ID")));
        return 0;
    }
    if (name == "gpio-create" &&
        (arguments.size() == 3 || arguments.size() == 4)) {
        const auto pin = parse_u32(arguments[1], "GPIO 引脚");
        if (pin > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("GPIO 引脚超过 16 位范围");
        }
        remotebsp::GpioDirection direction;
        if (arguments[2] == "input") {
            direction = remotebsp::GpioDirection::Input;
        } else if (arguments[2] == "output") {
            direction = remotebsp::GpioDirection::Output;
        } else {
            throw std::invalid_argument("GPIO 方向必须是 input 或 output");
        }
        const auto initial =
            arguments.size() == 4
                ? parse_u32(arguments[3], "GPIO 初始电平")
                : 0;
        if (initial > 1) {
            throw std::invalid_argument("GPIO 电平必须是 0 或 1");
        }
        std::cout << "object_id="
                  << client.gpio_create(static_cast<std::uint16_t>(pin),
                                        direction, initial != 0)
                  << '\n';
        return 0;
    }
    if (name == "gpio-read" && arguments.size() == 2) {
        std::cout << "value="
                  << (client.gpio_read(
                          parse_u32(arguments[1], "GPIO 对象 ID"))
                          ? 1
                          : 0)
                  << '\n';
        return 0;
    }
    if (name == "gpio-write" && arguments.size() == 3) {
        const auto value = parse_u32(arguments[2], "GPIO 电平");
        if (value > 1) {
            throw std::invalid_argument("GPIO 电平必须是 0 或 1");
        }
        client.gpio_write(parse_u32(arguments[1], "GPIO 对象 ID"),
                          value != 0);
        std::cout << "ok\n";
        return 0;
    }
    if (name == "pwm-create" &&
        (arguments.size() == 4 || arguments.size() == 5)) {
        const auto channel = parse_u32(arguments[1], "PWM 通道");
        const auto duty = parse_u32(arguments[3], "PWM 占空比");
        if (channel > std::numeric_limits<std::uint8_t>::max() ||
            duty > remotebsp::protocol::kPwmDutyScale) {
            throw std::invalid_argument("PWM 通道或占空比超出范围");
        }
        bool active_low = false;
        if (arguments.size() == 5) {
            if (arguments[4] == "active-low") {
                active_low = true;
            } else if (arguments[4] != "active-high") {
                throw std::invalid_argument(
                    "PWM 极性必须是 active-high 或 active-low");
            }
        }
        std::cout << "object_id=" << client.pwm_create({
            static_cast<std::uint8_t>(channel),
            parse_u32(arguments[2], "PWM 频率"),
            static_cast<std::uint16_t>(duty), active_low}) << '\n';
        return 0;
    }
    if (name == "pwm-write" && arguments.size() == 3) {
        const auto duty = parse_u32(arguments[2], "PWM 占空比");
        if (duty > remotebsp::protocol::kPwmDutyScale) {
            throw std::invalid_argument("PWM 占空比必须在 0..10000");
        }
        client.pwm_write(parse_u32(arguments[1], "PWM 对象 ID"),
                         static_cast<std::uint16_t>(duty));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "pwm-stop" && arguments.size() == 2) {
        client.pwm_stop(parse_u32(arguments[1], "PWM 对象 ID"));
        std::cout << "ok\n";
        return 0;
    }
    if ((name == "timed-bitstream-create" && arguments.size() == 6) ||
        (name == "ws2812-create" && arguments.size() == 2)) {
        const auto channel = parse_u32(arguments[1], "定时位流通道");
        if (channel > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("定时位流通道超过 8 位范围");
        }
        remotebsp::protocol::TimedBitstreamCreatePayload config{
            static_cast<std::uint8_t>(channel), 1250, 350, 700, 80};
        if (name == "timed-bitstream-create") {
            config.bit_period_ns = parse_u32(arguments[2], "位周期");
            config.zero_high_ns = parse_u32(arguments[3], "0 高电平");
            config.one_high_ns = parse_u32(arguments[4], "1 高电平");
            config.reset_time_us = parse_u32(arguments[5], "复位时间");
        }
        std::cout << "object_id="
                  << client.timed_bitstream_create(config) << '\n';
        return 0;
    }
    if (name == "timed-bitstream-write-hex" &&
        arguments.size() == 4) {
        const auto bit_count = parse_u32(arguments[2], "定时位流位数");
        if (bit_count == 0 ||
            bit_count > std::numeric_limits<std::uint16_t>::max()) {
            throw std::invalid_argument("定时位流位数超出范围");
        }
        client.timed_bitstream_write(
            parse_u32(arguments[1], "定时位流对象 ID"),
            {static_cast<std::uint16_t>(bit_count),
             parse_hex(arguments[3])});
        std::cout << "ok\n";
        return 0;
    }
    if (name == "ws2812-write" && arguments.size() == 3) {
        const auto rgb = parse_hex(arguments[2]);
        if (rgb.size() % 3U != 0U ||
            rgb.size() > std::numeric_limits<std::uint16_t>::max() / 8U) {
            throw std::invalid_argument(
                "WS2812 颜色必须是连续的 RRGGBB，且长度不能超限");
        }
        std::vector<std::uint8_t> grb;
        grb.reserve(rgb.size());
        for (std::size_t index = 0; index < rgb.size(); index += 3U) {
            grb.push_back(rgb[index + 1U]);
            grb.push_back(rgb[index]);
            grb.push_back(rgb[index + 2U]);
        }
        client.timed_bitstream_write(
            parse_u32(arguments[1], "WS2812 对象 ID"),
            {static_cast<std::uint16_t>(grb.size() * 8U), grb});
        std::cout << "ok pixels=" << grb.size() / 3U << '\n';
        return 0;
    }
    if (name == "timed-bitstream-abort" && arguments.size() == 2) {
        client.timed_bitstream_abort(
            parse_u32(arguments[1], "定时位流对象 ID"));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "uart-create" &&
        arguments.size() >= 3 && arguments.size() <= 7) {
        const auto port = parse_u32(arguments[1], "UART 端口");
        if (port > std::numeric_limits<std::uint8_t>::max()) {
            throw std::invalid_argument("UART 端口超过 8 位范围");
        }
        remotebsp::UartConfig config;
        config.port = static_cast<std::uint8_t>(port);
        config.baud_rate = parse_u32(arguments[2], "UART 波特率");
        if (arguments.size() >= 4) {
            config.data_bits = static_cast<std::uint8_t>(
                parse_u32(arguments[3], "UART 数据位"));
        }
        if (arguments.size() >= 5) {
            if (arguments[4] == "none") {
                config.parity = remotebsp::UartParity::None;
            } else if (arguments[4] == "odd") {
                config.parity = remotebsp::UartParity::Odd;
            } else if (arguments[4] == "even") {
                config.parity = remotebsp::UartParity::Even;
            } else {
                throw std::invalid_argument(
                    "UART 奇偶校验必须是 none、odd 或 even");
            }
        }
        if (arguments.size() >= 6) {
            config.stop_bits = static_cast<std::uint8_t>(
                parse_u32(arguments[5], "UART 停止位"));
        }
        if (arguments.size() >= 7) {
            if (arguments[6] == "poll") {
                config.receive_mode =
                    remotebsp::UartReceiveMode::Polling;
            } else if (arguments[6] == "stream") {
                config.receive_mode =
                    remotebsp::UartReceiveMode::Streaming;
            } else {
                throw std::invalid_argument(
                    "UART 接收模式必须是 poll 或 stream");
            }
        }
        std::cout << "object_id=" << client.uart_create(config) << '\n';
        return 0;
    }
    if (name == "uart-read" && arguments.size() == 3) {
        const auto data = client.uart_read(
            parse_u32(arguments[1], "UART 对象 ID"),
            parse_u32(arguments[2], "UART 最大读取长度"));
        std::cout << "data_hex=";
        print_hex(data);
        std::cout << '\n';
        return 0;
    }
    if (name == "uart-stream-read" &&
        arguments.size() >= 3 && arguments.size() <= 4) {
        const auto chunk = client.uart_stream_read(
            parse_u32(arguments[1], "UART 对象 ID"),
            parse_u32(arguments[2], "UART 最大读取长度"),
            arguments.size() == 4
                ? parse_u32(arguments[3], "UART 流读取超时")
                : 1000U);
        if (!chunk.has_value()) {
            std::cout << "timeout\n";
            return 0;
        }
        std::cout << "data_hex=";
        print_hex(chunk->data);
        std::cout << " dropped_bytes=" << chunk->dropped_bytes
                  << " lost_events=" << chunk->lost_events << '\n';
        return 0;
    }
    if (name == "uart-write" && arguments.size() == 3) {
        const std::vector<std::uint8_t> data(arguments[2].begin(),
                                             arguments[2].end());
        client.uart_write(parse_u32(arguments[1], "UART 对象 ID"), data);
        std::cout << "ok\n";
        return 0;
    }
    if (name == "uart-write-hex" && arguments.size() == 3) {
        client.uart_write(parse_u32(arguments[1], "UART 对象 ID"),
                          parse_hex(arguments[2]));
        std::cout << "ok\n";
        return 0;
    }
    if (name == "tmc2209-read" &&
        (arguments.size() == 3 || arguments.size() == 4)) {
        const auto uart_object_id =
            parse_u32(arguments[1], "UART 对象 ID");
        const auto register_value =
            parse_u32(arguments[2], "TMC2209 寄存器");
        const auto node_address = arguments.size() == 4
                                      ? parse_u32(arguments[3], "TMC2209 节点地址")
                                      : 0U;
        if (register_value > 0x7fU || node_address > 0x03U) {
            throw std::invalid_argument("TMC2209 寄存器或节点地址超出范围");
        }
        const auto register_address =
            static_cast<std::uint8_t>(register_value);
        client.uart_write(
            uart_object_id,
            remotebsp::Tmc2209::make_read_request(
                static_cast<std::uint8_t>(node_address), register_address));
        const auto response = client.uart_read(uart_object_id, 8U);
        const auto value = remotebsp::Tmc2209::decode_read_response(
            response, register_address);
        std::cout << "value=0x" << std::hex << std::setw(8)
                  << std::setfill('0') << value << std::setfill(' ')
                  << std::dec << '\n';
        return 0;
    }
    if (name == "tmc2209-write" &&
        (arguments.size() == 4 || arguments.size() == 5)) {
        const auto uart_object_id =
            parse_u32(arguments[1], "UART 对象 ID");
        const auto register_value =
            parse_u32(arguments[2], "TMC2209 寄存器");
        const auto value = parse_u32(arguments[3], "TMC2209 32位值");
        const auto node_address = arguments.size() == 5
                                      ? parse_u32(arguments[4], "TMC2209 节点地址")
                                      : 0U;
        if (register_value > 0x7fU || node_address > 0x03U) {
            throw std::invalid_argument("TMC2209 寄存器或节点地址超出范围");
        }
        client.uart_write(
            uart_object_id,
            remotebsp::Tmc2209::make_write_request(
                static_cast<std::uint8_t>(node_address),
                static_cast<std::uint8_t>(register_value), value));
        std::cout << "ok\n";
        return 0;
    }
    if ((name == "uart-write-all" ||
         name == "uart-write-all-hex") &&
        arguments.size() >= 3 && arguments.size() <= 4) {
        const auto data =
            name == "uart-write-all-hex"
                ? parse_hex(arguments[2])
                : std::vector<std::uint8_t>(
                      arguments[2].begin(), arguments[2].end());
        client.uart_write_all(
            parse_u32(arguments[1], "UART 对象 ID"), data,
            arguments.size() == 4
                ? parse_u32(arguments[3], "UART 写入超时")
                : 3000U);
        std::cout << "ok\n";
        return 0;
    }
    if (name == "motion-group-submit" && arguments.size() >= 7U) {
        remotebsp::MotionGroupPlan plan;
        plan.transaction_id = parse_u64(arguments[1], "运动组事务 ID");
        plan.group_id = parse_u32(arguments[2], "运动组 ID");
        plan.plan_generation = parse_u32(arguments[3], "运动组计划代数");
        plan.host_start_time_ns =
            arguments[4] == "auto"
                ? static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()).count()) +
                      500000000ULL
                : parse_u64(arguments[4], "运动组开始时间");
        const auto digest = parse_hex(arguments[5]);
        if (digest.size() != plan.content_digest.size()) {
            throw std::invalid_argument(
                "运动组内容摘要必须正好是 32 字节十六进制");
        }
        std::copy(digest.begin(), digest.end(),
                  plan.content_digest.begin());
        plan.members.reserve(arguments.size() - 6U);
        for (std::size_t index = 6U; index < arguments.size(); ++index) {
            plan.members.push_back(parse_motion_group_member(
                arguments[index], plan.host_start_time_ns));
        }
        print_motion_group_status(client.motion_group_submit(plan));
        return 0;
    }
    if (name == "motion-group-status" && arguments.size() == 4U) {
        print_motion_group_status(client.motion_group_status(
            parse_u64(arguments[1], "运动组事务 ID"),
            parse_u32(arguments[2], "运动组 ID"),
            parse_u32(arguments[3], "运动组计划代数")));
        return 0;
    }
    if (name == "motion-group-cancel" && arguments.size() == 4U) {
        print_motion_group_status(client.motion_group_cancel(
            parse_u64(arguments[1], "运动组事务 ID"),
            parse_u32(arguments[2], "运动组 ID"),
            parse_u32(arguments[3], "运动组计划代数")));
        return 0;
    }
    if (name == "motion-enqueue" && arguments.size() >= 6) {
        remotebsp::protocol::MotionSegmentPayload segment;
        segment.sequence = parse_u32(arguments[1], "运动段序号");
        segment.start_time_ns =
            arguments[2] == "auto"
                ? 0
                : parse_u64(arguments[2], "运动段开始时间");
        segment.duration_ns =
            parse_u64(arguments[3], "运动段持续时间");
        if (arguments[4] == "final") {
            segment.final_segment = true;
        } else if (arguments[4] == "more") {
            segment.final_segment = false;
        } else {
            throw std::invalid_argument(
                "运动段结束标志必须是 final 或 more");
        }
        segment.axes.reserve(arguments.size() - 5U);
        for (std::size_t index = 5; index < arguments.size(); ++index) {
            segment.axes.push_back(parse_motion_move(arguments[index]));
        }
        const auto accepted = client.motion_enqueue(segment);
        std::cout << "sequence=" << accepted.sequence
                  << " start_time_ns=" << accepted.start_time_ns
                  << " duration_ns=" << accepted.duration_ns
                  << " final=" << (accepted.final_segment ? 1 : 0)
                  << " axes=" << segment.axes.size() << '\n';
        return 0;
    }
    if (name == "motion-status" && arguments.size() == 1) {
        const auto status = client.motion_status();
        std::cout << "state=" << motion_state_name(status.state)
                  << " fault=" << motion_fault_name(status.fault)
                  << " node_time_ns=" << status.node_time_ns
                  << " queue_depth=" << status.queue_depth
                  << " queue_capacity=" << status.queue_capacity
                  << " last_accepted="
                  << status.last_accepted_sequence
                  << " last_completed="
                  << status.last_completed_sequence
                  << " accepted_segments="
                  << status.metrics.accepted_segments
                  << " rejected_segments="
                  << status.metrics.rejected_segments
                  << " completed_segments="
                  << status.metrics.completed_segments
                  << " emitted_steps=" << status.metrics.emitted_steps
                  << " safety_stops=" << status.metrics.safety_stops
                  << " limit_stops=" << status.metrics.limit_stops
                  << " queue_underruns="
                  << status.metrics.queue_underruns << '\n';
        for (const auto& axis : status.axes) {
            std::cout << "axis=0x" << std::hex << axis.resource_id
                      << std::dec
                      << " enabled=" << (axis.enabled ? 1 : 0)
                      << " direction_positive="
                      << (axis.direction_positive ? 1 : 0)
                      << " step=" << (axis.step_level ? 1 : 0)
                      << " position_steps=" << axis.position_steps
                      << " emitted_steps=" << axis.emitted_steps
                      << '\n';
        }
        return 0;
    }
    if (name == "motion-contract" && arguments.size() == 1) {
        const auto contract = client.motion_contract(true);
        std::cout << "version=" << contract.version
                  << " axes=" << contract.axes.size()
                  << " queue_capacity=" << contract.queue_capacity
                  << " minimum_lead_time_ns="
                  << contract.minimum_lead_time_ns
                  << " maximum_total_step_rate_hz="
                  << contract.maximum_total_step_rate_hz << '\n';
        for (const auto& axis : contract.axes) {
            std::cout << "axis=0x" << std::hex << axis.resource_id
                      << std::dec
                      << " maximum_step_rate_hz="
                      << axis.maximum_step_rate_hz
                      << " step_pulse_width_ns="
                      << axis.step_pulse_width_ns
                      << " minimum_step_low_ns="
                      << axis.minimum_step_low_ns
                      << " direction_setup_ns="
                      << axis.direction_setup_ns << '\n';
        }
        return 0;
    }
    if (name == "motion-abort" && arguments.size() == 1) {
        client.motion_abort();
        std::cout << "ok\n";
        return 0;
    }
    if (name == "motion-clear-fault" && arguments.size() == 1) {
        client.motion_clear_fault();
        std::cout << "ok\n";
        return 0;
    }

    print_usage();
    throw std::invalid_argument("未知命令或参数数量错误");
}

}

int main(int argc, char** argv) {
    bool json_output = false;
    try {
        std::string socket_path = "/tmp/toolbusd.sock";
        std::uint32_t node_id = 1;
        int index = 1;
        while (index < argc) {
            const std::string option = argv[index];
            if (option == "--json") {
                json_output = true;
                index += 1;
            } else if (option == "--socket" && index + 1 < argc) {
                socket_path = argv[index + 1];
                index += 2;
            } else if (option == "--node" && index + 1 < argc) {
                node_id = parse_u32(argv[index + 1], "节点 ID");
                index += 2;
            } else {
                break;
            }
        }
        std::vector<std::string> arguments;
        for (; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        if (json_output && (arguments.empty() ||
            (arguments[0] != "traffic-status" &&
             arguments[0] != "daemon-identity" &&
             arguments[0] != "node-list" &&
             arguments[0] != "runtime-snapshot" &&
             arguments[0] != "resource-list" &&
             arguments[0] != "resource-status"))) {
            throw std::invalid_argument(
                "--json当前仅支持Runtime只读命令");
        }
        return run(arguments, socket_path, node_id, json_output);
    } catch (const std::exception& error) {
        std::cerr << "remote-cli 错误: " << error.what() << '\n';
        return 1;
    }
}
