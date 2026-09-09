#include "remotebsp/mock_mcu/board_manifest.hpp"
#include "remotebsp/mock_mcu/twin_replay.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace remotebsp::mock_mcu {
namespace {

struct JsonValue {
    enum class Type {
        Null,
        Boolean,
        Integer,
        String,
        Array,
        Object,
    };

    Type type{Type::Null};
    bool boolean{};
    std::uint64_t integer{};
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue parse() {
        skip_space();
        JsonValue value = parse_value();
        skip_space();
        if (position_ != input_.size()) {
            fail("JSON 根值之后存在多余内容");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        std::ostringstream output;
        output << message << "，字节偏移 " << position_;
        throw ManifestException(ManifestError::InvalidJson, output.str());
    }

    void skip_space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(
                   input_[position_])) != 0) {
            ++position_;
        }
    }

    char take() {
        if (position_ >= input_.size()) {
            fail("JSON 意外结束");
        }
        return input_[position_++];
    }

    bool consume(char expected) {
        if (position_ < input_.size() &&
            input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            fail("JSON 字面量无效");
        }
        position_ += literal.size();
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(
                static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4 > input_.size()) {
            fail("JSON Unicode 转义不完整");
        }
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                fail("JSON Unicode 转义包含非十六进制字符");
            }
        }
        return value;
    }

    std::string parse_string() {
        if (take() != '"') {
            fail("JSON 字符串缺少开始引号");
        }
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char current =
                static_cast<unsigned char>(take());
            if (current == '"') {
                return output;
            }
            if (current < 0x20U) {
                fail("JSON 字符串包含控制字符");
            }
            if (current != '\\') {
                output.push_back(static_cast<char>(current));
                continue;
            }
            const char escaped = take();
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    const auto codepoint = parse_hex4();
                    if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
                        fail("板卡描述暂不接受代理项 Unicode 转义");
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default:
                    fail("JSON 字符串转义无效");
            }
        }
        fail("JSON 字符串没有结束引号");
    }

    JsonValue parse_integer() {
        const std::size_t begin = position_;
        if (consume('-')) {
            fail("板卡描述只允许非负整数");
        }
        if (consume('0')) {
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(
                    input_[position_])) != 0) {
                fail("JSON 整数不能包含前导零");
            }
        } else {
            if (position_ >= input_.size() ||
                input_[position_] < '1' || input_[position_] > '9') {
                fail("JSON 整数无效");
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(
                       input_[position_])) != 0) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            fail("板卡描述不允许小数或指数");
        }
        std::uint64_t value = 0;
        const auto result = std::from_chars(
            input_.data() + begin, input_.data() + position_, value);
        if (result.ec != std::errc()) {
            fail("JSON 整数超出范围");
        }
        JsonValue output;
        output.type = JsonValue::Type::Integer;
        output.integer = value;
        return output;
    }

    JsonValue parse_array() {
        JsonValue output;
        output.type = JsonValue::Type::Array;
        take();
        skip_space();
        if (consume(']')) {
            return output;
        }
        while (true) {
            skip_space();
            output.array.push_back(parse_value());
            skip_space();
            if (consume(']')) {
                return output;
            }
            if (!consume(',')) {
                fail("JSON 数组元素之间缺少逗号");
            }
        }
    }

    JsonValue parse_object() {
        JsonValue output;
        output.type = JsonValue::Type::Object;
        take();
        skip_space();
        if (consume('}')) {
            return output;
        }
        while (true) {
            skip_space();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("JSON 对象键必须是字符串");
            }
            const std::string key = parse_string();
            skip_space();
            if (!consume(':')) {
                fail("JSON 对象键之后缺少冒号");
            }
            skip_space();
            if (!output.object.emplace(key, parse_value()).second) {
                fail("JSON 对象包含重复键");
            }
            skip_space();
            if (consume('}')) {
                return output;
            }
            if (!consume(',')) {
                fail("JSON 对象成员之间缺少逗号");
            }
        }
    }

    JsonValue parse_value() {
        if (position_ >= input_.size()) {
            fail("JSON 意外结束");
        }
        switch (input_[position_]) {
            case '{':
                return parse_object();
            case '[':
                return parse_array();
            case '"': {
                JsonValue output;
                output.type = JsonValue::Type::String;
                output.string = parse_string();
                return output;
            }
            case 't': {
                expect_literal("true");
                JsonValue output;
                output.type = JsonValue::Type::Boolean;
                output.boolean = true;
                return output;
            }
            case 'f': {
                expect_literal("false");
                JsonValue output;
                output.type = JsonValue::Type::Boolean;
                return output;
            }
            case 'n': {
                expect_literal("null");
                return {};
            }
            default:
                return parse_integer();
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

using Object = std::map<std::string, JsonValue>;

[[noreturn]] void schema_error(ManifestError code,
                               const std::string& message) {
    throw ManifestException(code, message);
}

const Object& require_object(const JsonValue& value,
                             const char* field) {
    if (value.type != JsonValue::Type::Object) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是对象");
    }
    return value.object;
}

const JsonValue& require_field(const Object& object,
                               const char* field) {
    const auto found = object.find(field);
    if (found == object.end()) {
        schema_error(ManifestError::InvalidSchema,
                     "缺少字段 " + std::string(field));
    }
    return found->second;
}

void reject_unknown(const Object& object,
                    std::initializer_list<const char*> allowed,
                    const char* context) {
    for (const auto& entry : object) {
        const bool known = std::any_of(
            allowed.begin(), allowed.end(), [&](const char* name) {
                return entry.first == name;
            });
        if (!known) {
            schema_error(ManifestError::InvalidSchema,
                         std::string(context) + " 包含未知字段 " +
                             entry.first);
        }
    }
}

std::uint64_t require_integer(const Object& object,
                              const char* field) {
    const auto& value = require_field(object, field);
    if (value.type != JsonValue::Type::Integer) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是非负整数");
    }
    return value.integer;
}

std::uint32_t require_u32(const Object& object,
                          const char* field) {
    const auto value = require_integer(object, field);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        schema_error(ManifestError::InvalidValue,
                     std::string(field) +
                         " 超出 32 位无符号整数范围");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint16_t require_u16(const Object& object,
                          const char* field) {
    const auto value = require_integer(object, field);
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        schema_error(ManifestError::InvalidValue,
                     std::string(field) +
                         " 超出 16 位无符号整数范围");
    }
    return static_cast<std::uint16_t>(value);
}

const std::string& require_string(const Object& object,
                                  const char* field) {
    const auto& value = require_field(object, field);
    if (value.type != JsonValue::Type::String) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是字符串");
    }
    return value.string;
}

bool require_boolean(const Object& object, const char* field) {
    const auto& value = require_field(object, field);
    if (value.type != JsonValue::Type::Boolean) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是布尔值");
    }
    return value.boolean;
}

const std::vector<JsonValue>& require_array(const Object& object,
                                            const char* field) {
    const auto& value = require_field(object, field);
    if (value.type != JsonValue::Type::Array) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是数组");
    }
    return value.array;
}

std::vector<std::uint8_t> optional_byte_array(const Object& object,
                                              const char* field) {
    const auto found = object.find(field);
    if (found == object.end()) {
        return {};
    }
    if (found->second.type != JsonValue::Type::Array) {
        schema_error(ManifestError::InvalidSchema,
                     std::string(field) + " 必须是字节数组");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(found->second.array.size());
    for (const auto& value : found->second.array) {
        if (value.type != JsonValue::Type::Integer || value.integer > 255U) {
            schema_error(ManifestError::InvalidValue,
                         std::string(field) + " 元素必须位于 0～255");
        }
        bytes.push_back(static_cast<std::uint8_t>(value.integer));
    }
    return bytes;
}

protocol::BusResourceKind parse_bus_kind(const std::string& text) {
    if (text == "i2c_bus") return protocol::BusResourceKind::I2cBus;
    if (text == "i2c_device") return protocol::BusResourceKind::I2cDevice;
    if (text == "spi_bus") return protocol::BusResourceKind::SpiBus;
    if (text == "spi_device") return protocol::BusResourceKind::SpiDevice;
    schema_error(ManifestError::InvalidValue, "未知总线资源类型 " + text);
}

std::uint8_t parse_bus_flags(const std::vector<JsonValue>& values) {
    std::uint8_t flags = 0;
    for (const auto& value : values) {
        if (value.type != JsonValue::Type::String) {
            schema_error(ManifestError::InvalidSchema,
                         "bus_resource.flags 元素必须是字符串");
        }
        std::uint8_t flag = 0;
        if (value.string == "repeated_start") {
            flag = protocol::kBusContractRepeatedStart;
        } else if (value.string == "recovery") {
            flag = protocol::kBusContractRecovery;
        } else if (value.string == "full_duplex") {
            flag = protocol::kBusContractFullDuplex;
        } else if (value.string == "keep_chip_select") {
            flag = protocol::kBusContractKeepChipSelect;
        } else {
            schema_error(ManifestError::InvalidValue,
                         "未知总线合同标志 " + value.string);
        }
        if ((flags & flag) != 0) {
            schema_error(ManifestError::Conflict,
                         "总线合同标志重复 " + value.string);
        }
        flags = static_cast<std::uint8_t>(flags | flag);
    }
    return flags;
}

protocol::BusTransactionStatus parse_bus_status(const std::string& text) {
    if (text == "ok") return protocol::BusTransactionStatus::Ok;
    if (text == "nack") return protocol::BusTransactionStatus::Nack;
    if (text == "timeout") return protocol::BusTransactionStatus::Timeout;
    if (text == "busy") return protocol::BusTransactionStatus::Busy;
    if (text == "fault") return protocol::BusTransactionStatus::Fault;
    if (text == "limit_exceeded") {
        return protocol::BusTransactionStatus::LimitExceeded;
    }
    schema_error(ManifestError::InvalidValue, "未知总线故障状态 " + text);
}

protocol::ResourceType parse_resource_type(const std::string& text) {
    if (text == "gpio") {
        return protocol::ResourceType::Gpio;
    }
    if (text == "uart") {
        return protocol::ResourceType::Uart;
    }
    if (text == "spi") {
        return protocol::ResourceType::Spi;
    }
    if (text == "i2c") {
        return protocol::ResourceType::I2c;
    }
    if (text == "i2c_bus") {
        return protocol::ResourceType::I2cBus;
    }
    if (text == "i2c_device") {
        return protocol::ResourceType::I2cDevice;
    }
    if (text == "spi_bus") {
        return protocol::ResourceType::SpiBus;
    }
    if (text == "spi_device") {
        return protocol::ResourceType::SpiDevice;
    }
    if (text == "stream") {
        return protocol::ResourceType::Stream;
    }
    if (text == "adc") {
        return protocol::ResourceType::Adc;
    }
    if (text == "pwm") {
        return protocol::ResourceType::Pwm;
    }
    if (text == "timer") {
        return protocol::ResourceType::Timer;
    }
    if (text == "storage") {
        return protocol::ResourceType::Storage;
    }
    if (text == "stepgen_axis") {
        return protocol::ResourceType::StepgenAxis;
    }
    if (text == "timed_bitstream") {
        return protocol::ResourceType::TimedBitstream;
    }
    schema_error(ManifestError::InvalidValue,
                 "未知资源类型 " + text);
}

Capability resource_capability(protocol::ResourceType type) {
    switch (type) {
        case protocol::ResourceType::Gpio:
            return Capability::Gpio;
        case protocol::ResourceType::Uart:
            return Capability::Uart;
        case protocol::ResourceType::Spi:
        case protocol::ResourceType::SpiBus:
        case protocol::ResourceType::SpiDevice:
            return Capability::Spi;
        case protocol::ResourceType::I2c:
        case protocol::ResourceType::I2cBus:
        case protocol::ResourceType::I2cDevice:
            return Capability::I2c;
        case protocol::ResourceType::Adc:
            return Capability::Adc;
        case protocol::ResourceType::Pwm:
            return Capability::Pwm;
        case protocol::ResourceType::Timer:
            return Capability::Timer;
        case protocol::ResourceType::Storage:
            return Capability::Storage;
        case protocol::ResourceType::StepgenAxis:
            return Capability::Motion;
        case protocol::ResourceType::TimedBitstream:
            return Capability::TimedBitstream;
        case protocol::ResourceType::Stream:
            return Capability::Stream;
    }
    schema_error(ManifestError::InvalidValue, "资源类型没有能力位映射");
}

Capability parse_capability(const std::string& text) {
    if (text == "bootloader") {
        return Capability::Bootloader;
    }
    if (text == "motion") {
        return Capability::Motion;
    }
    return resource_capability(parse_resource_type(text));
}

std::uint16_t parse_access_flags(const std::vector<JsonValue>& values) {
    std::uint16_t flags = 0;
    for (const auto& value : values) {
        if (value.type != JsonValue::Type::String) {
            schema_error(ManifestError::InvalidSchema,
                         "contract.access 元素必须是字符串");
        }
        std::uint16_t flag = 0;
        if (value.string == "read") {
            flag = protocol::kResourceAccessReadable;
        } else if (value.string == "write") {
            flag = protocol::kResourceAccessWritable;
        } else if (value.string == "shared_read") {
            flag = protocol::kResourceAccessSharedRead;
        } else if (value.string == "exclusive_write") {
            flag = protocol::kResourceAccessExclusiveWrite;
        } else if (value.string == "lease_supported") {
            flag = protocol::kResourceAccessLeaseSupported;
        } else if (value.string == "lease_required") {
            flag = protocol::kResourceAccessLeaseRequired;
        } else {
            schema_error(ManifestError::InvalidValue,
                         "未知资源访问标志 " + value.string);
        }
        if ((flags & flag) != 0) {
            schema_error(ManifestError::Conflict,
                         "资源访问标志重复 " + value.string);
        }
        flags = static_cast<std::uint16_t>(flags | flag);
    }
    if ((flags & protocol::kResourceAccessLeaseRequired) != 0 &&
        (flags & protocol::kResourceAccessLeaseSupported) == 0) {
        schema_error(ManifestError::Conflict,
                     "lease_required 必须同时声明 lease_supported");
    }
    return flags;
}

std::array<std::uint8_t, 16> parse_uuid(const std::string& text) {
    if (text.size() != 32) {
        schema_error(ManifestError::InvalidValue,
                     "uuid 必须是 32 个十六进制字符");
    }
    std::array<std::uint8_t, 16> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto hex = text.substr(index * 2, 2);
        unsigned value = 0;
        const auto result = std::from_chars(
            hex.data(), hex.data() + hex.size(), value, 16);
        if (result.ec != std::errc() ||
            result.ptr != hex.data() + hex.size()) {
            schema_error(ManifestError::InvalidValue,
                         "uuid 包含非十六进制字符");
        }
        output[index] = static_cast<std::uint8_t>(value);
    }
    return output;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        schema_error(ManifestError::Io,
                     "无法打开文件: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        schema_error(ManifestError::Io,
                     "读取文件失败: " + path);
    }
    return contents.str();
}

const protocol::ResourceDescriptor* find_resource(
    const BoardManifest& manifest, std::uint32_t resource_id) {
    const auto found = std::find_if(
        manifest.resources.begin(), manifest.resources.end(),
        [resource_id](const auto& resource) {
            return resource.resource_id == resource_id;
        });
    return found == manifest.resources.end() ? nullptr : &*found;
}

}  // namespace

ManifestException::ManifestException(ManifestError code,
                                     const std::string& message)
    : std::runtime_error(message), code_(code) {}

ManifestError ManifestException::code() const noexcept { return code_; }

BoardManifest parse_board_manifest(std::string_view json_text) {
    const auto root_value = JsonParser(json_text).parse();
    const auto& root = require_object(root_value, "板卡描述根");
    reject_unknown(root,
                   {"schema_version", "name", "board_type", "uuid",
                    "firmware_version", "capabilities",
                    "resource_groups", "reserved_resources", "bus_resources",
                    "motion_axes", "motion_maximum_total_step_rate_hz",
                    "waveform_endpoints"},
                   "板卡描述根");

    BoardManifest manifest;
    manifest.schema_version = require_u32(root, "schema_version");
    if (manifest.schema_version == 0 ||
        manifest.schema_version > kBoardManifestSchemaVersion) {
        schema_error(ManifestError::InvalidSchema,
                     "不支持的板卡描述 schema_version");
    }
    if (manifest.schema_version == 1 && root.count("bus_resources") != 0) {
        schema_error(ManifestError::InvalidSchema,
                     "schema_version 1 不能声明 bus_resources");
    }
    manifest.name = require_string(root, "name");
    if (manifest.name.empty() || manifest.name.size() > 64) {
        schema_error(ManifestError::InvalidValue,
                     "name 长度必须位于 1～64 字节");
    }
    manifest.node_info.board_type = require_u32(root, "board_type");
    manifest.node_info.uuid = parse_uuid(require_string(root, "uuid"));

    const auto& firmware = require_array(root, "firmware_version");
    if (firmware.size() != 3) {
        schema_error(ManifestError::InvalidValue,
                     "firmware_version 必须包含三个整数");
    }
    std::array<std::uint16_t, 3> version{};
    for (std::size_t index = 0; index < version.size(); ++index) {
        if (firmware[index].type != JsonValue::Type::Integer ||
            firmware[index].integer >
                std::numeric_limits<std::uint16_t>::max()) {
            schema_error(ManifestError::InvalidValue,
                         "firmware_version 元素超出范围");
        }
        version[index] =
            static_cast<std::uint16_t>(firmware[index].integer);
    }
    manifest.node_info.firmware_major = version[0];
    manifest.node_info.firmware_minor = version[1];
    manifest.node_info.firmware_patch = version[2];

    for (const auto& value : require_array(root, "capabilities")) {
        if (value.type != JsonValue::Type::String) {
            schema_error(ManifestError::InvalidSchema,
                         "capabilities 元素必须是字符串");
        }
        const auto bit = capability_mask(parse_capability(value.string));
        if ((manifest.capabilities & bit) != 0) {
            schema_error(ManifestError::Conflict,
                         "能力重复 " + value.string);
        }
        manifest.capabilities |= bit;
    }

    std::set<std::uint32_t> resource_ids;
    std::set<std::pair<protocol::ResourceType, std::uint16_t>>
        resource_instances;
    for (const auto& group_value :
         require_array(root, "resource_groups")) {
        const auto& group = require_object(group_value, "resource_group");
        reject_unknown(
            group,
            {"type", "first_instance", "count", "id_base", "source",
             "rx_capacity", "tx_capacity", "contract"},
            "resource_group");
        const auto type =
            parse_resource_type(require_string(group, "type"));
        const auto first_instance =
            require_u16(group, "first_instance");
        const auto count = require_u16(group, "count");
        const auto id_base = require_u32(group, "id_base");
        if (count == 0 || count > 256) {
            schema_error(ManifestError::InvalidValue,
                         "resource_group.count 必须位于 1～256");
        }
        if (static_cast<std::uint32_t>(first_instance) + count >
            65536U) {
            schema_error(ManifestError::InvalidValue,
                         "资源实例号范围溢出");
        }
        if (type == protocol::ResourceType::Uart &&
            static_cast<std::uint32_t>(first_instance) + count > 256U) {
            schema_error(ManifestError::InvalidValue,
                         "UART 实例号必须位于 0～255");
        }
        if (id_base > std::numeric_limits<std::uint32_t>::max() -
                          (count - 1U)) {
            schema_error(ManifestError::InvalidValue,
                         "资源 ID 范围溢出");
        }
        const std::string& source = require_string(group, "source");
        std::uint16_t resource_flags = 0;
        if (source == "native") {
            resource_flags = protocol::kResourceFlagNative;
        } else if (source == "expanded") {
            resource_flags = protocol::kResourceFlagExpanded;
        } else {
            schema_error(ManifestError::InvalidValue,
                         "source 必须是 native 或 expanded");
        }
        const auto rx_capacity = require_u32(group, "rx_capacity");
        const auto tx_capacity = require_u32(group, "tx_capacity");

        const auto& contract = require_object(
            require_field(group, "contract"), "contract");
        reject_unknown(
            contract,
            {"access", "timing_resolution_ns",
             "worst_case_latency_us",
             "maximum_operations_per_second", "queue_capacity",
             "maximum_rx_bits_per_second",
             "maximum_tx_bits_per_second"},
            "contract");
        const auto access_flags =
            parse_access_flags(require_array(contract, "access"));
        const auto timing_resolution_ns =
            require_u32(contract, "timing_resolution_ns");
        const auto worst_case_latency_us =
            require_u32(contract, "worst_case_latency_us");
        const auto maximum_operations_per_second =
            require_u32(contract, "maximum_operations_per_second");
        const auto queue_capacity =
            require_u32(contract, "queue_capacity");
        const auto maximum_rx_bits_per_second =
            require_u32(contract, "maximum_rx_bits_per_second");
        const auto maximum_tx_bits_per_second =
            require_u32(contract, "maximum_tx_bits_per_second");

        if ((manifest.capabilities &
             capability_mask(resource_capability(type))) == 0) {
            schema_error(
                ManifestError::Conflict,
                "资源组对应的能力未在 capabilities 中声明");
        }
        if (manifest.resources.size() >
            0xFFFFU - static_cast<std::size_t>(count)) {
            schema_error(ManifestError::InvalidValue,
                         "公开资源总数超过协议上限 65535");
        }
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const auto instance = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(first_instance) + offset);
            const auto resource_id = id_base + offset;
            if (!resource_ids.insert(resource_id).second ||
                !resource_instances.emplace(type, instance).second) {
                schema_error(ManifestError::Conflict,
                             "资源 ID 或类型/实例重复");
            }
            manifest.resources.push_back(
                {resource_id, type, instance, resource_flags,
                 rx_capacity, tx_capacity});
            manifest.contracts.push_back(
                {resource_id, protocol::kResourceContractVersion,
                 access_flags, timing_resolution_ns,
                 worst_case_latency_us,
                 maximum_operations_per_second, queue_capacity,
                 maximum_rx_bits_per_second,
                 maximum_tx_bits_per_second});
        }
    }

    std::set<std::pair<protocol::ResourceType, std::uint16_t>>
        reserved_instances;
    for (const auto& reserved_value :
         require_array(root, "reserved_resources")) {
        const auto& reserved =
            require_object(reserved_value, "reserved_resource");
        reject_unknown(reserved, {"type", "instance", "owner"},
                       "reserved_resource");
        ReservedResource entry;
        entry.type =
            parse_resource_type(require_string(reserved, "type"));
        entry.instance = require_u16(reserved, "instance");
        entry.owner = require_string(reserved, "owner");
        if (entry.owner.empty() || entry.owner.size() > 64) {
            schema_error(ManifestError::InvalidValue,
                         "reserved_resource.owner 长度必须位于 1～64 字节");
        }
        const auto key = std::make_pair(entry.type, entry.instance);
        if (!reserved_instances.insert(key).second) {
            schema_error(ManifestError::Conflict,
                         "保留资源类型/实例重复");
        }
        if (resource_instances.count(key) != 0) {
            schema_error(ManifestError::Conflict,
                         "同一资源不能既公开又被内部占用");
        }
        if ((entry.type == protocol::ResourceType::Spi ||
             entry.type == protocol::ResourceType::SpiBus) &&
            (resource_instances.count(
                 {protocol::ResourceType::Spi, entry.instance}) != 0 ||
             resource_instances.count(
                 {protocol::ResourceType::SpiBus, entry.instance}) != 0)) {
            schema_error(ManifestError::Conflict,
                         "内部占用的 SPI 控制器不能作为外部总线枚举");
        }
        if ((entry.type == protocol::ResourceType::I2c ||
             entry.type == protocol::ResourceType::I2cBus) &&
            (resource_instances.count(
                 {protocol::ResourceType::I2c, entry.instance}) != 0 ||
             resource_instances.count(
                 {protocol::ResourceType::I2cBus, entry.instance}) != 0)) {
            schema_error(ManifestError::Conflict,
                         "内部占用的 I2C 控制器不能作为外部总线枚举");
        }
        manifest.reserved_resources.push_back(std::move(entry));
    }

    std::set<std::uint32_t> configured_bus_resources;
    std::set<std::pair<std::uint32_t, std::uint16_t>> i2c_addresses;
    std::set<std::pair<std::uint32_t, std::uint16_t>> spi_chip_selects;
    if (manifest.schema_version >= 2) {
        for (const auto& bus_value : require_array(root, "bus_resources")) {
            const auto& bus = require_object(bus_value, "bus_resource");
            reject_unknown(
                bus,
                {"resource_id", "kind", "flags",
                 "parent_bus_resource_id", "maximum_clock_hz",
                 "maximum_transfer_bytes", "queue_capacity",
                 "minimum_timeout_us", "maximum_timeout_us",
                 "maximum_operations_per_second", "i2c_address",
                 "spi_mode", "bits_per_word", "spi_chip_select",
                 "initial_data",
                 "deterministic_response"},
                "bus_resource");
            BusManifestResource config;
            config.contract.resource_id = require_u32(bus, "resource_id");
            config.contract.kind =
                parse_bus_kind(require_string(bus, "kind"));
            config.contract.flags =
                parse_bus_flags(require_array(bus, "flags"));
            config.contract.parent_bus_resource_id =
                require_u32(bus, "parent_bus_resource_id");
            config.contract.maximum_clock_hz =
                require_u32(bus, "maximum_clock_hz");
            config.contract.maximum_transfer_bytes =
                require_u16(bus, "maximum_transfer_bytes");
            config.contract.queue_capacity =
                require_u16(bus, "queue_capacity");
            config.contract.minimum_timeout_us =
                require_u32(bus, "minimum_timeout_us");
            config.contract.maximum_timeout_us =
                require_u32(bus, "maximum_timeout_us");
            config.contract.maximum_operations_per_second =
                require_u32(bus, "maximum_operations_per_second");
            config.initial_data = optional_byte_array(bus, "initial_data");
            config.deterministic_response =
                optional_byte_array(bus, "deterministic_response");

            const bool i2c_device = config.contract.kind ==
                                    protocol::BusResourceKind::I2cDevice;
            const bool spi_device = config.contract.kind ==
                                    protocol::BusResourceKind::SpiDevice;
            const bool is_device = i2c_device || spi_device;
            if (i2c_device) {
                config.i2c_address = require_u16(bus, "i2c_address");
                if (*config.i2c_address == 0 || *config.i2c_address > 0x7FU ||
                    bus.count("spi_mode") != 0 ||
                    bus.count("bits_per_word") != 0 ||
                    bus.count("spi_chip_select") != 0 ||
                    !config.deterministic_response.empty()) {
                    schema_error(ManifestError::InvalidValue,
                                 "I2C 设备静态字段无效");
                }
            } else if (spi_device) {
                const auto mode = require_u16(bus, "spi_mode");
                const auto bits = require_u16(bus, "bits_per_word");
                config.spi_chip_select =
                    require_u16(bus, "spi_chip_select");
                if (mode > 3 || bits < 4 || bits > 16 ||
                    bus.count("i2c_address") != 0 ||
                    !config.initial_data.empty()) {
                    schema_error(ManifestError::InvalidValue,
                                 "SPI 设备静态字段无效");
                }
                config.spi_mode = static_cast<std::uint8_t>(mode);
                config.bits_per_word = static_cast<std::uint8_t>(bits);
            } else if (bus.count("i2c_address") != 0 ||
                       bus.count("spi_mode") != 0 ||
                       bus.count("bits_per_word") != 0 ||
                       bus.count("spi_chip_select") != 0 ||
                       !config.initial_data.empty() ||
                       !config.deterministic_response.empty()) {
                schema_error(ManifestError::InvalidValue,
                             "总线资源不能声明设备数据字段");
            }
            try {
                static_cast<void>(protocol::encode_bus_resource_contract(
                    config.contract));
            } catch (const protocol::BusStreamPayloadException& error) {
                schema_error(ManifestError::InvalidValue,
                             std::string("总线合同无效: ") + error.what());
            }
            if ((config.initial_data.size() >
                 config.contract.maximum_transfer_bytes) ||
                (config.deterministic_response.size() >
                 config.contract.maximum_transfer_bytes)) {
                schema_error(ManifestError::InvalidValue,
                             "Mock 总线数据超过设备合同最大事务长度");
            }
            if (!configured_bus_resources.insert(
                     config.contract.resource_id).second) {
                schema_error(ManifestError::Conflict,
                             "bus_resources 包含重复资源 ID");
            }
            const auto* resource = find_resource(
                manifest, config.contract.resource_id);
            protocol::ResourceType expected = protocol::ResourceType::I2cBus;
            switch (config.contract.kind) {
                case protocol::BusResourceKind::I2cBus:
                    expected = protocol::ResourceType::I2cBus;
                    break;
                case protocol::BusResourceKind::I2cDevice:
                    expected = protocol::ResourceType::I2cDevice;
                    break;
                case protocol::BusResourceKind::SpiBus:
                    expected = protocol::ResourceType::SpiBus;
                    break;
                case protocol::BusResourceKind::SpiDevice:
                    expected = protocol::ResourceType::SpiDevice;
                    break;
            }
            if (resource == nullptr || resource->type != expected) {
                schema_error(ManifestError::Conflict,
                             "总线合同引用了缺失或类型不匹配的公开资源");
            }
            if (is_device) {
                const auto* parent = find_resource(
                    manifest, config.contract.parent_bus_resource_id);
                const auto parent_type = i2c_device
                    ? protocol::ResourceType::I2cBus
                    : protocol::ResourceType::SpiBus;
                if (parent == nullptr || parent->type != parent_type) {
                    schema_error(ManifestError::Conflict,
                                 "总线设备父资源缺失或类型不匹配");
                }
                const auto endpoint = i2c_device
                    ? *config.i2c_address
                    : *config.spi_chip_select;
                auto& endpoints = i2c_device
                    ? i2c_addresses
                    : spi_chip_selects;
                if (!endpoints.emplace(
                         config.contract.parent_bus_resource_id,
                         endpoint).second) {
                    schema_error(ManifestError::Conflict,
                                 "同一总线上的设备地址或片选重复");
                }
            }
            manifest.bus_resources.push_back(std::move(config));
        }
    }
    for (const auto& resource : manifest.resources) {
        const bool typed_bus_resource =
            resource.type == protocol::ResourceType::I2cBus ||
            resource.type == protocol::ResourceType::I2cDevice ||
            resource.type == protocol::ResourceType::SpiBus ||
            resource.type == protocol::ResourceType::SpiDevice;
        if (typed_bus_resource &&
            configured_bus_resources.count(resource.resource_id) == 0) {
            schema_error(ManifestError::Conflict,
                         "公开总线资源缺少 bus_resources 合同");
        }
    }
    for (const auto& config : manifest.bus_resources) {
        const bool is_device =
            config.contract.kind == protocol::BusResourceKind::I2cDevice ||
            config.contract.kind == protocol::BusResourceKind::SpiDevice;
        if (!is_device) {
            continue;
        }
        const auto parent = std::find_if(
            manifest.bus_resources.begin(), manifest.bus_resources.end(),
            [&config](const auto& candidate) {
                return candidate.contract.resource_id ==
                       config.contract.parent_bus_resource_id;
            });
        if (parent == manifest.bus_resources.end()) {
            schema_error(ManifestError::Conflict,
                         "总线设备父资源缺少总线合同");
        }
        const auto& child_contract = config.contract;
        const auto& parent_contract = parent->contract;
        if (child_contract.maximum_clock_hz >
                parent_contract.maximum_clock_hz ||
            child_contract.maximum_transfer_bytes >
                parent_contract.maximum_transfer_bytes ||
            child_contract.queue_capacity > parent_contract.queue_capacity ||
            child_contract.minimum_timeout_us <
                parent_contract.minimum_timeout_us ||
            child_contract.maximum_timeout_us >
                parent_contract.maximum_timeout_us ||
            (parent_contract.maximum_operations_per_second != 0 &&
             child_contract.maximum_operations_per_second >
                 parent_contract.maximum_operations_per_second) ||
            (child_contract.flags &
             static_cast<std::uint8_t>(~parent_contract.flags)) != 0) {
            schema_error(ManifestError::InvalidValue,
                         "设备合同超出父总线能力边界");
        }
    }

    const auto waveform_endpoints = root.find("waveform_endpoints");
    std::set<std::pair<protocol::ResourceType, std::uint16_t>>
        configured_waveform_endpoints;
    if (waveform_endpoints != root.end()) {
        if (waveform_endpoints->second.type != JsonValue::Type::Array) {
            schema_error(ManifestError::InvalidSchema,
                         "waveform_endpoints 必须是数组");
        }
        for (const auto& endpoint_value :
             waveform_endpoints->second.array) {
            const auto& endpoint =
                require_object(endpoint_value, "waveform_endpoint");
            reject_unknown(
                endpoint,
                {"type", "instance", "pin", "timer", "channel",
                 "dma_channel", "maximum_frequency_hz", "maximum_bits",
                 "maximum_bit_rate"},
                "waveform_endpoint");
            WaveformEndpointCapability capability;
            capability.type =
                parse_resource_type(require_string(endpoint, "type"));
            capability.instance = require_u16(endpoint, "instance");
            capability.pin = require_u16(endpoint, "pin");
            const auto timer = require_u16(endpoint, "timer");
            const auto channel = require_u16(endpoint, "channel");
            const auto dma_channel = require_u16(endpoint, "dma_channel");
            capability.maximum_frequency_hz =
                require_u32(endpoint, "maximum_frequency_hz");
            capability.maximum_bits =
                require_u16(endpoint, "maximum_bits");
            capability.maximum_bit_rate =
                require_u32(endpoint, "maximum_bit_rate");
            if ((capability.type != protocol::ResourceType::Pwm &&
                 capability.type !=
                     protocol::ResourceType::TimedBitstream) ||
                timer == 0U || timer > 255U || channel == 0U ||
                channel > 4U || dma_channel > 255U ||
                capability.pin > 255U) {
                schema_error(ManifestError::InvalidValue,
                             "波形端点类型或硬件字段无效");
            }
            capability.timer = static_cast<std::uint8_t>(timer);
            capability.channel = static_cast<std::uint8_t>(channel);
            capability.dma_channel =
                static_cast<std::uint8_t>(dma_channel);
            const auto key =
                std::make_pair(capability.type, capability.instance);
            if (!configured_waveform_endpoints.insert(key).second ||
                resource_instances.count(key) == 0U) {
                schema_error(ManifestError::Conflict,
                             "波形端点未引用唯一公开资源实例");
            }
            if (capability.type == protocol::ResourceType::Pwm) {
                if (capability.dma_channel != 0U ||
                    capability.maximum_frequency_hz == 0U ||
                    capability.maximum_bits != 0U ||
                    capability.maximum_bit_rate != 0U) {
                    schema_error(ManifestError::InvalidValue,
                                 "PWM端点能力字段无效");
                }
            } else if (capability.dma_channel == 0U ||
                       capability.maximum_frequency_hz != 0U ||
                       capability.maximum_bits == 0U ||
                       capability.maximum_bit_rate == 0U) {
                schema_error(ManifestError::InvalidValue,
                             "定时位流端点能力字段无效");
            }
            manifest.waveform_endpoints.push_back(capability);
        }
        for (const auto& resource : manifest.resources) {
            if ((resource.type == protocol::ResourceType::Pwm ||
                 resource.type ==
                     protocol::ResourceType::TimedBitstream) &&
                configured_waveform_endpoints.count(
                    {resource.type, resource.instance}) == 0U) {
                schema_error(ManifestError::Conflict,
                             "公开波形资源缺少硬件端点能力");
            }
        }
    }

    const auto motion_axes = root.find("motion_axes");
    std::set<std::uint32_t> configured_motion_axes;
    if (motion_axes != root.end()) {
        if (motion_axes->second.type != JsonValue::Type::Array) {
            schema_error(ManifestError::InvalidSchema,
                         "motion_axes 必须是数组");
        }
        for (const auto& axis_value : motion_axes->second.array) {
            const auto& axis =
                require_object(axis_value, "motion_axis");
            reject_unknown(
                axis,
                {"resource_id", "maximum_step_rate_hz",
                 "step_pulse_width_ns", "minimum_step_low_ns",
                 "direction_setup_ns"},
                "motion_axis");
            MotionAxisConfig config;
            config.resource_id = require_u32(axis, "resource_id");
            config.maximum_step_rate_hz =
                require_u32(axis, "maximum_step_rate_hz");
            config.step_pulse_width_ns =
                require_u32(axis, "step_pulse_width_ns");
            config.minimum_step_low_ns =
                require_u32(axis, "minimum_step_low_ns");
            config.direction_setup_ns =
                require_u32(axis, "direction_setup_ns");
            const auto* resource =
                find_resource(manifest, config.resource_id);
            if (resource == nullptr ||
                resource->type !=
                    protocol::ResourceType::StepgenAxis) {
                schema_error(
                    ManifestError::Conflict,
                    "motion_axis 必须引用公开的 STEPGEN_AXIS 资源");
            }
            if (!configured_motion_axes.insert(config.resource_id).second) {
                schema_error(ManifestError::Conflict,
                             "motion_axis 资源 ID 重复");
            }
            manifest.motion_axes.push_back(config);
        }
    }
    for (const auto& resource : manifest.resources) {
        if (resource.type == protocol::ResourceType::StepgenAxis &&
            configured_motion_axes.count(resource.resource_id) == 0) {
            schema_error(ManifestError::Conflict,
                         "公开的 STEPGEN_AXIS 缺少 motion_axes 配置");
        }
    }
    std::optional<std::uint32_t> motion_queue_capacity;
    for (const auto& resource : manifest.resources) {
        if (resource.type != protocol::ResourceType::StepgenAxis) {
            continue;
        }
        const auto found = std::find_if(
            manifest.contracts.begin(), manifest.contracts.end(),
            [&resource](const auto& contract) {
                return contract.resource_id == resource.resource_id;
            });
        if (found == manifest.contracts.end() ||
            found->queue_capacity == 0 ||
            found->queue_capacity > 1024) {
            schema_error(
                ManifestError::InvalidValue,
                "STEPGEN_AXIS 必须声明 1～1024 的运动段队列容量");
        }
        if (motion_queue_capacity.has_value() &&
            *motion_queue_capacity != found->queue_capacity) {
            schema_error(
                ManifestError::Conflict,
                "同一节点的 STEPGEN_AXIS 必须共享相同运动队列容量");
        }
        motion_queue_capacity = found->queue_capacity;
    }
    if (manifest.motion_axes.size() >
        protocol::kMaximumMotionAxes) {
        schema_error(
            ManifestError::InvalidValue,
            "单节点运动轴数量超过协议允许的 64 轴");
    }
    if (motion_queue_capacity.has_value()) {
        manifest.motion_queue_capacity = *motion_queue_capacity;
    }
    if (!manifest.motion_axes.empty()) {
        manifest.motion_maximum_total_step_rate_hz =
            require_u32(root, "motion_maximum_total_step_rate_hz");
        if (manifest.motion_maximum_total_step_rate_hz == 0U) {
            schema_error(ManifestError::InvalidValue,
                         "运动整板总STEP频率预算必须非零");
        }
    } else if (root.find("motion_maximum_total_step_rate_hz") !=
               root.end()) {
        schema_error(ManifestError::Conflict,
                     "没有运动轴时不能声明整板总STEP频率预算");
    }
    return manifest;
}

BoardManifest load_board_manifest(const std::string& path) {
    return parse_board_manifest(read_file(path));
}

NodeInfo instantiate_node_info(const BoardManifest& manifest,
                               std::uint32_t instance) {
    if (instance == 0 || instance > 127) {
        schema_error(ManifestError::InvalidValue,
                     "Mock MCU 实例号必须位于 1～127");
    }
    NodeInfo info = manifest.node_info;
    info.uuid[15] = static_cast<std::uint8_t>(instance);
    return info;
}

FaultScenario parse_fault_scenario(std::string_view json_text) {
    const auto root_value = JsonParser(json_text).parse();
    const auto& root = require_object(root_value, "故障场景根");
    reject_unknown(root, {"schema_version", "events"}, "故障场景根");
    FaultScenario scenario;
    scenario.schema_version = require_u32(root, "schema_version");
    if (scenario.schema_version != kFaultScenarioSchemaVersion) {
        schema_error(ManifestError::InvalidSchema,
                     "不支持的故障场景 schema_version");
    }
    std::uint64_t previous_time = 0;
    bool first = true;
    for (const auto& event_value : require_array(root, "events")) {
        if (scenario.events.size() >= 0xFFFFU) {
            schema_error(ManifestError::InvalidValue,
                         "故障事件数量超过 65535");
        }
        const auto& event = require_object(event_value, "故障事件");
        reject_unknown(event,
                       {"at_ms", "action", "resource_id", "value", "status"},
                       "故障事件");
        FaultEvent parsed;
        parsed.at_ms = require_integer(event, "at_ms");
        if (parsed.at_ms >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            schema_error(ManifestError::InvalidValue,
                         "故障事件 at_ms 超出单调时钟范围");
        }
        const auto& action = require_string(event, "action");
        if (action == "uart_failed") {
            parsed.action = FaultAction::SetUartFailed;
            parsed.resource_id = require_u32(event, "resource_id");
        } else if (action == "gpio_input") {
            parsed.action = FaultAction::SetGpioInput;
            parsed.resource_id = require_u32(event, "resource_id");
        } else if (action == "node_online") {
            parsed.action = FaultAction::SetNodeOnline;
            if (event.count("resource_id") != 0) {
                schema_error(ManifestError::InvalidSchema,
                             "node_online 事件不能包含 resource_id");
            }
        } else if (action == "motion_limit") {
            parsed.action = FaultAction::TriggerMotionLimit;
            parsed.resource_id = require_u32(event, "resource_id");
            if (event.count("value") != 0) {
                schema_error(ManifestError::InvalidSchema,
                             "motion_limit 事件不能包含 value");
            }
        } else if (action == "bus_status") {
            parsed.action = FaultAction::SetBusStatus;
            parsed.resource_id = require_u32(event, "resource_id");
            parsed.bus_status =
                parse_bus_status(require_string(event, "status"));
            if (event.count("value") != 0) {
                schema_error(ManifestError::InvalidSchema,
                             "bus_status 事件不能包含 value");
            }
        } else {
            schema_error(ManifestError::InvalidValue,
                         "未知故障动作 " + action);
        }
        if (parsed.action == FaultAction::SetUartFailed ||
            parsed.action == FaultAction::SetGpioInput ||
            parsed.action == FaultAction::SetNodeOnline) {
            parsed.value = require_boolean(event, "value");
        }
        if (parsed.action != FaultAction::SetBusStatus &&
            event.count("status") != 0) {
            schema_error(ManifestError::InvalidSchema,
                         "只有 bus_status 事件可以包含 status");
        }
        if (!first && parsed.at_ms < previous_time) {
            schema_error(ManifestError::InvalidValue,
                         "故障事件必须按 at_ms 非递减排列");
        }
        first = false;
        previous_time = parsed.at_ms;
        scenario.events.push_back(parsed);
    }
    return scenario;
}

FaultScenario load_fault_scenario(const std::string& path) {
    return parse_fault_scenario(read_file(path));
}

DigitalTwin::DigitalTwin(BoardManifest manifest, FaultScenario scenario)
    : manifest_(std::move(manifest)),
      scenario_(std::move(scenario)),
      gpio_(std::make_shared<MockGpioBsp>()) {
    if (scenario_.schema_version != kFaultScenarioSchemaVersion) {
        schema_error(ManifestError::InvalidSchema,
                     "不支持的故障场景 schema_version");
    }
    if (scenario_.events.size() > 0xFFFFU) {
        schema_error(ManifestError::InvalidValue,
                     "故障事件数量超过 65535");
    }
    for (std::size_t index = 0; index < scenario_.events.size(); ++index) {
        const auto& event = scenario_.events[index];
        if ((index != 0U &&
             event.at_ms < scenario_.events[index - 1U].at_ms) ||
            event.at_ms > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
            schema_error(ManifestError::InvalidValue,
                         "故障事件时间必须非递减且位于单调时钟范围内");
        }
        if (event.action == FaultAction::TriggerMotionLimit &&
            event.at_ms > std::numeric_limits<std::uint64_t>::max() /
                              1000000ULL) {
            schema_error(ManifestError::InvalidValue,
                         "运动限位事件时间换算溢出");
        }
    }
    std::uint32_t uart_rx_capacity = 1;
    std::uint32_t uart_tx_capacity = 1;
    for (const auto& resource : manifest_.resources) {
        if (resource.type == protocol::ResourceType::Uart) {
            uart_rx_capacity =
                std::max(uart_rx_capacity, resource.rx_capacity);
            uart_tx_capacity =
                std::max(uart_tx_capacity, resource.tx_capacity);
        }
    }
    uart_ = std::make_shared<MockUartBsp>(
        uart_rx_capacity, uart_tx_capacity);
    if ((manifest_.capabilities & capability_mask(Capability::Pwm)) != 0U ||
        (manifest_.capabilities &
         capability_mask(Capability::TimedBitstream)) != 0U) {
        waveform_ = std::make_shared<WaveformBsp>();
    }
    if (!manifest_.motion_axes.empty()) {
        motion_ = std::make_shared<MotionExecutor>(
            manifest_.motion_axes,
            manifest_.motion_queue_capacity, 1000000ULL,
            manifest_.motion_maximum_total_step_rate_hz);
    }
    if (!manifest_.bus_resources.empty()) {
        bus_ = std::make_shared<MockBusBsp>();
        for (const auto& config : manifest_.bus_resources) {
            if (config.contract.kind !=
                    protocol::BusResourceKind::I2cDevice &&
                config.contract.kind !=
                    protocol::BusResourceKind::SpiDevice) {
                continue;
            }
            bus_->add_device(config.contract, config.initial_data);
            if (config.contract.kind ==
                protocol::BusResourceKind::SpiDevice) {
                bus_->set_spi_response(config.contract.resource_id,
                                       config.deterministic_response);
            }
        }
    }
    for (const auto& event : scenario_.events) {
        if (event.action == FaultAction::SetUartFailed) {
            require_resource(event.resource_id,
                             protocol::ResourceType::Uart);
        } else if (event.action == FaultAction::SetGpioInput) {
            require_resource(event.resource_id,
                             protocol::ResourceType::Gpio);
        } else if (event.action == FaultAction::TriggerMotionLimit) {
            require_resource(event.resource_id,
                             protocol::ResourceType::StepgenAxis);
            if (!motion_) {
                schema_error(ManifestError::InvalidValue,
                             "运动限位事件要求板卡启用运动执行器");
            }
        } else if (event.action == FaultAction::SetBusStatus) {
            const auto* resource = find_resource(manifest_, event.resource_id);
            if (event.bus_status >
                    protocol::BusTransactionStatus::LimitExceeded ||
                !bus_ || resource == nullptr ||
                (resource->type != protocol::ResourceType::I2cDevice &&
                 resource->type != protocol::ResourceType::SpiDevice)) {
                schema_error(ManifestError::InvalidValue,
                             "总线故障事件引用了不存在或类型错误的设备");
            }
        } else if (event.action == FaultAction::SetNodeOnline) {
            if (event.resource_id != 0) {
                schema_error(ManifestError::InvalidValue,
                             "节点级故障事件不能引用资源");
            }
        } else {
            schema_error(ManifestError::InvalidValue,
                         "故障事件动作枚举无效");
        }
    }
}

const BoardManifest& DigitalTwin::manifest() const noexcept {
    return manifest_;
}

const std::shared_ptr<MockGpioBsp>& DigitalTwin::gpio() const noexcept {
    return gpio_;
}

const std::shared_ptr<MockUartBsp>& DigitalTwin::uart() const noexcept {
    return uart_;
}

const std::shared_ptr<MotionExecutor>& DigitalTwin::motion() const noexcept {
    return motion_;
}

const std::shared_ptr<WaveformBsp>& DigitalTwin::waveform() const noexcept {
    return waveform_;
}

const std::shared_ptr<MockBusBsp>& DigitalTwin::bus() const noexcept {
    return bus_;
}

bool DigitalTwin::online() const noexcept { return online_; }

std::optional<std::uint64_t> DigitalTwin::next_event_ms() const noexcept {
    if (next_event_index_ >= scenario_.events.size()) {
        return std::nullopt;
    }
    return scenario_.events[next_event_index_].at_ms;
}

std::size_t DigitalTwin::advance_to(std::uint64_t elapsed_ms) {
    std::size_t applied = 0;
    while (next_event_index_ < scenario_.events.size() &&
           scenario_.events[next_event_index_].at_ms <= elapsed_ms) {
        apply(scenario_.events[next_event_index_]);
        ++next_event_index_;
        ++applied;
    }
    if (motion_) {
        if (elapsed_ms >
            std::numeric_limits<std::uint64_t>::max() / 1000000ULL) {
            schema_error(ManifestError::InvalidValue,
                         "数字孪生运动时间溢出");
        }
        auto edges =
            motion_->advance_to(elapsed_ms * 1000000ULL);
        pending_motion_edges_.insert(
            pending_motion_edges_.end(), edges.begin(), edges.end());
    }
    return applied;
}

std::vector<MotionEdge> DigitalTwin::take_motion_edges() {
    std::vector<MotionEdge> edges =
        std::move(pending_motion_edges_);
    pending_motion_edges_.clear();
    return edges;
}

const protocol::ResourceDescriptor& DigitalTwin::require_resource(
    std::uint32_t resource_id, protocol::ResourceType type) const {
    const auto* resource =
        find_resource(manifest_, resource_id);
    if (resource == nullptr || resource->type != type) {
        schema_error(ManifestError::InvalidValue,
                     "故障事件引用了不存在或类型不匹配的资源");
    }
    return *resource;
}

void DigitalTwin::apply(const FaultEvent& event) {
    if (event.action == FaultAction::SetUartFailed) {
        const auto& resource = require_resource(
            event.resource_id, protocol::ResourceType::Uart);
        uart_->set_failed(static_cast<std::uint8_t>(resource.instance),
                          event.value);
    } else if (event.action == FaultAction::SetGpioInput) {
        const auto& resource = require_resource(
            event.resource_id, protocol::ResourceType::Gpio);
        gpio_->set_input_value(resource.instance, event.value);
    } else if (event.action == FaultAction::TriggerMotionLimit) {
        auto edges = motion_->trigger_limit(
            event.resource_id, event.at_ms * 1000000ULL);
        pending_motion_edges_.insert(
            pending_motion_edges_.end(), edges.begin(), edges.end());
    } else if (event.action == FaultAction::SetBusStatus) {
        bus_->set_next_status(event.resource_id, event.bus_status);
    } else {
        online_ = event.value;
    }
}

RemoteCore make_remote_core(const DigitalTwin& twin,
                            std::uint32_t instance) {
    const auto& manifest = twin.manifest();
    auto device_parameters = std::make_shared<DeviceParameterStore>();
    return RemoteCore(instantiate_node_info(manifest, instance),
                      manifest.capabilities |
                          capability_mask(Capability::DeviceParameters),
                       twin.gpio(), twin.uart(),
                       manifest.resources, manifest.contracts,
                       twin.motion(), twin.waveform(), device_parameters,
                       twin.bus());
}

namespace {

constexpr std::size_t kMaximumReplayCheckpoints = 0xFFFFU;
constexpr std::size_t kMaximumReplayJsonBytes = 16U * 1024U * 1024U;

class StableDigest {
public:
    void add_u64(std::uint64_t value) noexcept {
        for (unsigned index = 0; index < 8U; ++index) {
            add_byte(static_cast<std::uint8_t>(value >> (index * 8U)));
        }
    }

    void add_string(std::string_view value) noexcept {
        add_u64(value.size());
        for (const char byte : value) {
            add_byte(static_cast<std::uint8_t>(byte));
        }
    }

    template <typename Container>
    void add_bytes(const Container& value) noexcept {
        add_u64(value.size());
        for (const auto byte : value) {
            add_byte(static_cast<std::uint8_t>(byte));
        }
    }

    std::string text() const {
        std::ostringstream output;
        output << "fnv1a64:" << std::hex << std::setfill('0')
               << std::setw(16) << value_;
        return output.str();
    }

private:
    void add_byte(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }

    std::uint64_t value_{14695981039346656037ULL};
};

void add_manifest_identity(StableDigest& digest,
                           const BoardManifest& manifest) {
    digest.add_u64(manifest.schema_version);
    digest.add_string(manifest.name);
    digest.add_bytes(manifest.node_info.uuid);
    digest.add_u64(manifest.node_info.firmware_major);
    digest.add_u64(manifest.node_info.firmware_minor);
    digest.add_u64(manifest.node_info.firmware_patch);
    digest.add_u64(manifest.node_info.board_type);
    digest.add_u64(manifest.node_info.protocol_version);
    digest.add_u64(manifest.capabilities);

    auto resources = manifest.resources;
    std::sort(resources.begin(), resources.end(), [](const auto& left,
                                                     const auto& right) {
        return left.resource_id < right.resource_id;
    });
    digest.add_u64(resources.size());
    for (const auto& resource : resources) {
        digest.add_bytes(protocol::encode_resource_descriptor(resource));
    }

    auto contracts = manifest.contracts;
    std::sort(contracts.begin(), contracts.end(), [](const auto& left,
                                                     const auto& right) {
        return left.resource_id < right.resource_id;
    });
    digest.add_u64(contracts.size());
    for (const auto& contract : contracts) {
        digest.add_bytes(protocol::encode_resource_contract(contract));
    }

    auto buses = manifest.bus_resources;
    std::sort(buses.begin(), buses.end(), [](const auto& left,
                                            const auto& right) {
        return left.contract.resource_id < right.contract.resource_id;
    });
    digest.add_u64(buses.size());
    for (const auto& bus : buses) {
        digest.add_bytes(
            protocol::encode_bus_resource_contract(bus.contract));
        digest.add_u64(bus.i2c_address.has_value() ? 1U : 0U);
        digest.add_u64(bus.i2c_address.value_or(0U));
        digest.add_u64(bus.spi_mode.has_value() ? 1U : 0U);
        digest.add_u64(bus.spi_mode.value_or(0U));
        digest.add_u64(bus.bits_per_word.has_value() ? 1U : 0U);
        digest.add_u64(bus.bits_per_word.value_or(0U));
        digest.add_u64(bus.spi_chip_select.has_value() ? 1U : 0U);
        digest.add_u64(bus.spi_chip_select.value_or(0U));
        digest.add_bytes(bus.initial_data);
        digest.add_bytes(bus.deterministic_response);
    }

    auto reserved = manifest.reserved_resources;
    std::sort(reserved.begin(), reserved.end(), [](const auto& left,
                                                   const auto& right) {
        if (left.type != right.type) return left.type < right.type;
        if (left.instance != right.instance) {
            return left.instance < right.instance;
        }
        return left.owner < right.owner;
    });
    digest.add_u64(reserved.size());
    for (const auto& resource : reserved) {
        digest.add_u64(static_cast<std::uint8_t>(resource.type));
        digest.add_u64(resource.instance);
        digest.add_string(resource.owner);
    }

    auto axes = manifest.motion_axes;
    std::sort(axes.begin(), axes.end(), [](const auto& left,
                                          const auto& right) {
        return left.resource_id < right.resource_id;
    });
    digest.add_u64(axes.size());
    for (const auto& axis : axes) {
        digest.add_u64(axis.resource_id);
        digest.add_u64(axis.maximum_step_rate_hz);
        digest.add_u64(axis.step_pulse_width_ns);
        digest.add_u64(axis.minimum_step_low_ns);
        digest.add_u64(axis.direction_setup_ns);
        digest.add_u64(axis.driver_resource_id);
        digest.add_u64(axis.driver_type);
    }
    digest.add_u64(manifest.motion_queue_capacity);
    digest.add_u64(manifest.motion_maximum_total_step_rate_hz);

    auto waveform = manifest.waveform_endpoints;
    std::sort(waveform.begin(), waveform.end(), [](const auto& left,
                                                   const auto& right) {
        if (left.type != right.type) return left.type < right.type;
        return left.instance < right.instance;
    });
    digest.add_u64(waveform.size());
    for (const auto& endpoint : waveform) {
        digest.add_u64(static_cast<std::uint8_t>(endpoint.type));
        digest.add_u64(endpoint.instance);
        digest.add_u64(endpoint.pin);
        digest.add_u64(endpoint.timer);
        digest.add_u64(endpoint.channel);
        digest.add_u64(endpoint.dma_channel);
        digest.add_u64(endpoint.maximum_frequency_hz);
        digest.add_u64(endpoint.maximum_bits);
        digest.add_u64(endpoint.maximum_bit_rate);
    }
}

std::string scenario_identity(const FaultScenario& scenario) {
    if (scenario.events.size() > kMaximumReplayCheckpoints) {
        schema_error(ManifestError::InvalidValue,
                     "回放输入故障事件数量超过 65535");
    }
    StableDigest digest;
    digest.add_u64(scenario.schema_version);
    digest.add_u64(scenario.events.size());
    for (const auto& event : scenario.events) {
        digest.add_u64(event.at_ms);
        digest.add_u64(static_cast<std::uint8_t>(event.action));
        digest.add_u64(event.resource_id);
        digest.add_u64(event.value ? 1U : 0U);
        digest.add_u64(static_cast<std::uint8_t>(event.bus_status));
    }
    return digest.text();
}

std::string twin_state_digest(const DigitalTwin& twin,
                              std::uint32_t node_instance,
                              std::uint64_t random_seed) {
    StableDigest digest;
    add_manifest_identity(digest, twin.manifest());
    digest.add_u64(node_instance);
    digest.add_u64(random_seed);
    digest.add_u64(twin.online() ? 1U : 0U);

    const auto gpio = twin.gpio()->snapshot();
    digest.add_u64(gpio.size());
    for (const auto& pin : gpio) {
        digest.add_u64(pin.pin);
        digest.add_u64(static_cast<std::uint8_t>(pin.direction));
        digest.add_u64(pin.value ? 1U : 0U);
    }
    const auto pending_gpio = twin.gpio()->pending_input_snapshot();
    digest.add_u64(pending_gpio.size());
    for (const auto& pin : pending_gpio) {
        digest.add_u64(pin.pin);
        digest.add_u64(pin.value ? 1U : 0U);
    }

    std::vector<std::pair<std::uint32_t, std::uint16_t>> uart_resources;
    for (const auto& resource : twin.manifest().resources) {
        if (resource.type == protocol::ResourceType::Uart) {
            uart_resources.emplace_back(resource.resource_id,
                                        resource.instance);
        }
    }
    std::sort(uart_resources.begin(), uart_resources.end());
    digest.add_u64(uart_resources.size());
    for (const auto& item : uart_resources) {
        const auto status = twin.uart()->status(
            static_cast<std::uint8_t>(item.second));
        digest.add_u64(item.first);
        digest.add_u64(status.failed ? 1U : 0U);
        digest.add_u64(status.rx_buffered);
        digest.add_u64(status.tx_buffered);
        digest.add_u64(status.rx_overruns);
        digest.add_u64(status.tx_overruns);
    }

    if (twin.motion()) {
        const auto status = twin.motion()->status();
        digest.add_u64(1U);
        digest.add_u64(static_cast<std::uint8_t>(status.state));
        digest.add_u64(static_cast<std::uint8_t>(status.fault));
        digest.add_u64(status.node_time_ns);
        digest.add_u64(status.queue_depth);
        digest.add_u64(status.queue_capacity);
        digest.add_u64(status.last_accepted_sequence);
        digest.add_u64(status.last_completed_sequence);
        digest.add_u64(status.metrics.accepted_segments);
        digest.add_u64(status.metrics.rejected_segments);
        digest.add_u64(status.metrics.completed_segments);
        digest.add_u64(status.metrics.emitted_edges);
        digest.add_u64(status.metrics.emitted_steps);
        digest.add_u64(status.metrics.safety_stops);
        digest.add_u64(status.metrics.limit_stops);
        digest.add_u64(status.metrics.queue_underruns);
        digest.add_u64(status.metrics.maximum_queue_depth);
        digest.add_u64(status.axes.size());
        for (const auto& axis : status.axes) {
            digest.add_u64(axis.resource_id);
            digest.add_u64(axis.enabled ? 1U : 0U);
            digest.add_u64(axis.direction_positive ? 1U : 0U);
            digest.add_u64(axis.step_level ? 1U : 0U);
            digest.add_u64(static_cast<std::uint64_t>(axis.position_steps));
            digest.add_u64(axis.emitted_steps);
        }
    } else {
        digest.add_u64(0U);
    }

    if (twin.waveform()) {
        digest.add_u64(1U);
        const auto pwm = twin.waveform()->pwm_snapshot();
        digest.add_u64(pwm.size());
        for (const auto& channel : pwm) {
            digest.add_u64(channel.channel);
            digest.add_u64(channel.frequency_hz);
            digest.add_u64(channel.duty);
            digest.add_u64(channel.active_low ? 1U : 0U);
            digest.add_u64(channel.running ? 1U : 0U);
            digest.add_u64(channel.update_count);
        }
        const auto bitstreams = twin.waveform()->bitstream_snapshot();
        digest.add_u64(bitstreams.size());
        for (const auto& stream : bitstreams) {
            digest.add_u64(stream.timing.channel);
            digest.add_u64(stream.timing.bit_period_ns);
            digest.add_u64(stream.timing.zero_high_ns);
            digest.add_u64(stream.timing.one_high_ns);
            digest.add_u64(stream.timing.reset_time_us);
            digest.add_u64(stream.bit_count);
            digest.add_bytes(stream.data);
            digest.add_u64(stream.busy ? 1U : 0U);
            digest.add_u64(stream.write_count);
        }
    } else {
        digest.add_u64(0U);
    }

    const auto buses = twin.bus() ? twin.bus()->snapshot()
                                  : std::vector<MockBusDeviceSnapshot>{};
    digest.add_u64(buses.size());
    for (const auto& bus : buses) {
        digest.add_u64(bus.resource_id);
        digest.add_u64(static_cast<std::uint8_t>(bus.next_status));
        digest.add_u64(bus.data.size());
        for (const auto byte : bus.data) digest.add_u64(byte);
        digest.add_u64(bus.spi_response.size());
        for (const auto byte : bus.spi_response) digest.add_u64(byte);
    }
    return digest.text();
}

bool valid_digest(const std::string& value) {
    if (value.size() != 24U || value.compare(0U, 8U, "fnv1a64:") != 0) {
        return false;
    }
    return std::all_of(value.begin() + 8, value.end(), [](char digit) {
        return (digit >= '0' && digit <= '9') ||
               (digit >= 'a' && digit <= 'f');
    });
}

void validate_replay_record(const TwinReplayRecord& record) {
    if (record.schema_version != kTwinReplaySchemaVersion) {
        schema_error(ManifestError::InvalidSchema,
                     "不支持的数字孪生回放 schema_version");
    }
    if (record.time_base != kTwinReplayTimeBase) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放时间基无效");
    }
    if (!valid_digest(record.scenario_id) || record.board_name.empty() ||
        record.board_name.size() > 256U || record.node_instance == 0U ||
        record.node_instance > 127U ||
        record.checkpoints.size() > kMaximumReplayCheckpoints ||
        record.summary.checkpoint_count != record.checkpoints.size() ||
        !valid_digest(record.summary.final_state_digest)) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放根字段或摘要无效");
    }
    std::uint64_t total_events = 0U;
    std::uint64_t previous_at_ms = 0U;
    for (std::size_t index = 0; index < record.checkpoints.size(); ++index) {
        const auto& checkpoint = record.checkpoints[index];
        if (checkpoint.sequence != index + 1U ||
            checkpoint.applied_events == 0U ||
            !valid_digest(checkpoint.state_digest) ||
            checkpoint.at_ms > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max()) ||
            (index != 0U && checkpoint.at_ms <= previous_at_ms)) {
            schema_error(ManifestError::InvalidValue,
                         "数字孪生回放检查点顺序或字段无效");
        }
        previous_at_ms = checkpoint.at_ms;
        total_events += checkpoint.applied_events;
        if (total_events > kMaximumReplayCheckpoints) {
            schema_error(ManifestError::InvalidValue,
                         "数字孪生回放事件总数超过 65535");
        }
    }
    const auto expected_final_elapsed_ms = record.checkpoints.empty()
                                               ? 0U
                                               : record.checkpoints.back().at_ms;
    if (record.summary.event_count != total_events ||
        record.summary.final_elapsed_ms != expected_final_elapsed_ms) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放摘要与检查点不一致");
    }
}

std::string replay_json_string(std::string_view value) {
    std::string result{"\""};
    for (const char character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    schema_error(ManifestError::InvalidValue,
                                 "回放字符串包含不支持的控制字符");
                }
                result += character;
        }
    }
    result += '"';
    return result;
}

}  // namespace

TwinReplayRecord record_digital_twin(
    const BoardManifest& manifest, const FaultScenario& scenario,
    std::uint32_t node_instance, std::uint64_t random_seed) {
    static_cast<void>(instantiate_node_info(manifest, node_instance));
    DigitalTwin twin(manifest, scenario);
    TwinReplayRecord record;
    record.random_seed = random_seed;
    record.scenario_id = scenario_identity(scenario);
    record.board_name = manifest.name;
    record.node_instance = node_instance;

    std::size_t event_index = 0U;
    while (event_index < scenario.events.size()) {
        const auto at_ms = scenario.events[event_index].at_ms;
        std::size_t expected = 0U;
        while (event_index + expected < scenario.events.size() &&
               scenario.events[event_index + expected].at_ms == at_ms) {
            ++expected;
        }
        const auto applied = twin.advance_to(at_ms);
        if (applied != expected) {
            schema_error(ManifestError::Conflict,
                         "数字孪生故障脚本推进结果不确定");
        }
        record.checkpoints.push_back({
            static_cast<std::uint32_t>(record.checkpoints.size() + 1U),
            at_ms, static_cast<std::uint32_t>(applied),
            twin_state_digest(twin, node_instance, random_seed)});
        event_index += expected;
    }
    record.summary.event_count =
        static_cast<std::uint32_t>(scenario.events.size());
    record.summary.checkpoint_count =
        static_cast<std::uint32_t>(record.checkpoints.size());
    record.summary.final_elapsed_ms = scenario.events.empty()
                                          ? 0U
                                          : scenario.events.back().at_ms;
    record.summary.final_online = twin.online();
    record.summary.final_state_digest =
        twin_state_digest(twin, node_instance, random_seed);
    validate_replay_record(record);
    return record;
}

void verify_digital_twin_replay(
    const BoardManifest& manifest, const FaultScenario& scenario,
    std::uint32_t node_instance, const TwinReplayRecord& record) {
    validate_replay_record(record);
    const auto expected = record_digital_twin(
        manifest, scenario, node_instance, record.random_seed);
    if (record.scenario_id != expected.scenario_id ||
        record.board_name != expected.board_name ||
        record.node_instance != expected.node_instance ||
        record.summary.event_count != expected.summary.event_count ||
        record.summary.checkpoint_count != expected.summary.checkpoint_count ||
        record.summary.final_elapsed_ms != expected.summary.final_elapsed_ms ||
        record.summary.final_online != expected.summary.final_online ||
        record.summary.final_state_digest !=
            expected.summary.final_state_digest ||
        record.checkpoints.size() != expected.checkpoints.size()) {
        schema_error(ManifestError::Conflict,
                     "数字孪生回放身份或最终摘要不匹配");
    }
    for (std::size_t index = 0; index < record.checkpoints.size(); ++index) {
        const auto& actual = record.checkpoints[index];
        const auto& wanted = expected.checkpoints[index];
        if (actual.sequence != wanted.sequence ||
            actual.at_ms != wanted.at_ms ||
            actual.applied_events != wanted.applied_events ||
            actual.state_digest != wanted.state_digest) {
            schema_error(ManifestError::Conflict,
                         "数字孪生回放检查点摘要不匹配");
        }
    }
}

std::string encode_twin_replay_record(const TwinReplayRecord& record) {
    validate_replay_record(record);
    std::ostringstream output;
    output << "{\n  \"schema_version\": " << record.schema_version
           << ",\n  \"time_base\": " << replay_json_string(record.time_base)
           << ",\n  \"random_seed\": " << record.random_seed
           << ",\n  \"scenario_id\": "
           << replay_json_string(record.scenario_id)
           << ",\n  \"board_name\": "
           << replay_json_string(record.board_name)
           << ",\n  \"node_instance\": " << record.node_instance
           << ",\n  \"checkpoints\": [";
    for (std::size_t index = 0; index < record.checkpoints.size(); ++index) {
        const auto& checkpoint = record.checkpoints[index];
        output << (index == 0U ? "\n" : ",\n")
               << "    {\"sequence\": " << checkpoint.sequence
               << ", \"at_ms\": " << checkpoint.at_ms
               << ", \"applied_events\": " << checkpoint.applied_events
               << ", \"state_digest\": "
               << replay_json_string(checkpoint.state_digest) << "}";
    }
    output << (record.checkpoints.empty() ? "],\n" : "\n  ],\n")
           << "  \"summary\": {\"event_count\": "
           << record.summary.event_count
           << ", \"checkpoint_count\": "
           << record.summary.checkpoint_count
           << ", \"final_elapsed_ms\": "
           << record.summary.final_elapsed_ms
           << ", \"final_online\": "
           << (record.summary.final_online ? "true" : "false")
           << ", \"final_state_digest\": "
           << replay_json_string(record.summary.final_state_digest)
           << "}\n}\n";
    return output.str();
}

TwinReplayRecord parse_twin_replay_record(std::string_view json_text) {
    if (json_text.size() > kMaximumReplayJsonBytes) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放 JSON 超过 16 MiB");
    }
    const auto root_value = JsonParser(json_text).parse();
    const auto& root = require_object(root_value, "数字孪生回放根");
    reject_unknown(root,
                   {"schema_version", "time_base", "random_seed",
                    "scenario_id", "board_name", "node_instance",
                    "checkpoints", "summary"},
                   "数字孪生回放根");
    TwinReplayRecord record;
    record.schema_version = require_u32(root, "schema_version");
    record.time_base = require_string(root, "time_base");
    record.random_seed = require_integer(root, "random_seed");
    record.scenario_id = require_string(root, "scenario_id");
    record.board_name = require_string(root, "board_name");
    record.node_instance = require_u32(root, "node_instance");
    const auto& checkpoints = require_array(root, "checkpoints");
    if (checkpoints.size() > kMaximumReplayCheckpoints) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放检查点数量超过 65535");
    }
    record.checkpoints.reserve(checkpoints.size());
    for (const auto& value : checkpoints) {
        const auto& checkpoint = require_object(value, "回放检查点");
        reject_unknown(checkpoint,
                       {"sequence", "at_ms", "applied_events",
                        "state_digest"},
                       "回放检查点");
        record.checkpoints.push_back({
            require_u32(checkpoint, "sequence"),
            require_integer(checkpoint, "at_ms"),
            require_u32(checkpoint, "applied_events"),
            require_string(checkpoint, "state_digest")});
    }
    const auto& summary = require_object(require_field(root, "summary"),
                                         "回放摘要");
    reject_unknown(summary,
                   {"event_count", "checkpoint_count", "final_elapsed_ms",
                    "final_online", "final_state_digest"},
                   "回放摘要");
    record.summary = {
        require_u32(summary, "event_count"),
        require_u32(summary, "checkpoint_count"),
        require_integer(summary, "final_elapsed_ms"),
        require_boolean(summary, "final_online"),
        require_string(summary, "final_state_digest")};
    validate_replay_record(record);
    return record;
}

TwinReplayRecord load_twin_replay_record(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        schema_error(ManifestError::Io, "无法读取数字孪生回放文件");
    }
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) >
                        kMaximumReplayJsonBytes) {
        schema_error(ManifestError::InvalidValue,
                     "数字孪生回放 JSON 超过 16 MiB");
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    if (!contents.empty() &&
        !input.read(contents.data(), static_cast<std::streamsize>(size))) {
        schema_error(ManifestError::Io, "读取数字孪生回放文件失败");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        schema_error(ManifestError::Io,
                     "读取期间数字孪生回放文件长度发生变化");
    }
    return parse_twin_replay_record(contents);
}

void write_twin_replay_record(const TwinReplayRecord& record,
                              const std::string& path) {
    if (path.empty()) {
        schema_error(ManifestError::Io, "数字孪生回放文件路径不能为空");
    }
    const auto encoded = encode_twin_replay_record(record);
    const auto temporary = path + ".tmp";
    // 使用独占创建，拒绝遗留临时文件及同名符号链接，避免跟随可预测的
    // `.tmp` 链接截断目标文件。并发写同一路径时由其中一方失败关闭。
    std::FILE* output = std::fopen(temporary.c_str(), "wbx");
    if (output == nullptr) {
        schema_error(ManifestError::Io,
                     "无法独占创建数字孪生回放临时文件");
    }
    const auto written = std::fwrite(encoded.data(), 1U, encoded.size(),
                                     output);
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (written != encoded.size() || !flushed || !closed) {
        std::remove(temporary.c_str());
        schema_error(ManifestError::Io,
                     "写入数字孪生回放文件失败");
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        schema_error(ManifestError::Io,
                     "原子替换数字孪生回放文件失败");
    }
}

}  // namespace remotebsp::mock_mcu
