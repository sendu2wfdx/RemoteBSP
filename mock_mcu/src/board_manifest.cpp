#include "remotebsp/mock_mcu/board_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
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
            return Capability::Spi;
        case protocol::ResourceType::I2c:
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
                    "resource_groups", "reserved_resources",
                    "motion_axes", "motion_maximum_total_step_rate_hz",
                    "waveform_endpoints"},
                   "板卡描述根");

    BoardManifest manifest;
    manifest.schema_version = require_u32(root, "schema_version");
    if (manifest.schema_version != kBoardManifestSchemaVersion) {
        schema_error(ManifestError::InvalidSchema,
                     "不支持的板卡描述 schema_version");
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
        manifest.reserved_resources.push_back(std::move(entry));
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
        reject_unknown(event, {"at_ms", "action", "resource_id", "value"},
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
        } else {
            schema_error(ManifestError::InvalidValue,
                         "未知故障动作 " + action);
        }
        if (parsed.action != FaultAction::TriggerMotionLimit) {
            parsed.value = require_boolean(event, "value");
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
        } else if (event.resource_id != 0) {
            schema_error(ManifestError::InvalidValue,
                         "节点级故障事件不能引用资源");
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
                       twin.motion(), twin.waveform(), device_parameters);
}

}  // namespace remotebsp::mock_mcu
