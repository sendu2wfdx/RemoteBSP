#include "remotebsp/tmc2209.hpp"

#include <stdexcept>

namespace remotebsp {
namespace {

constexpr std::uint8_t kSync = 0x05U;
constexpr std::uint8_t kMasterReplyAddress = 0xffU;
constexpr std::uint8_t kWriteMask = 0x80U;

void validate_address(std::uint8_t address) {
    if (address > 0x03U) {
        throw std::invalid_argument("TMC2209 节点地址必须在 0 到 3 之间");
    }
}

void validate_register(std::uint8_t register_address) {
    if ((register_address & kWriteMask) != 0U) {
        throw std::invalid_argument("TMC2209 寄存器地址必须小于 0x80");
    }
}

}  // namespace

std::uint8_t Tmc2209::calculate_crc(
    const std::vector<std::uint8_t>& data) {
    std::uint8_t crc = 0U;
    for (const std::uint8_t value : data) {
        std::uint8_t current = value;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            if (((crc >> 7U) ^ (current & 0x01U)) != 0U) {
                crc = static_cast<std::uint8_t>((crc << 1U) ^ 0x07U);
            } else {
                crc = static_cast<std::uint8_t>(crc << 1U);
            }
            current = static_cast<std::uint8_t>(current >> 1U);
        }
    }
    return crc;
}

std::vector<std::uint8_t> Tmc2209::make_read_request(
    std::uint8_t node_address, std::uint8_t register_address) {
    validate_address(node_address);
    validate_register(register_address);
    std::vector<std::uint8_t> request{
        kSync, node_address, register_address};
    request.push_back(calculate_crc(request));
    return request;
}

std::vector<std::uint8_t> Tmc2209::make_write_request(
    std::uint8_t node_address, std::uint8_t register_address,
    std::uint32_t value) {
    validate_address(node_address);
    validate_register(register_address);
    std::vector<std::uint8_t> request{
        kSync,
        node_address,
        static_cast<std::uint8_t>(register_address | kWriteMask),
        static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value),
    };
    request.push_back(calculate_crc(request));
    return request;
}

std::uint32_t Tmc2209::decode_read_response(
    const std::vector<std::uint8_t>& response,
    std::uint8_t expected_register_address) {
    validate_register(expected_register_address);
    if (response.size() != 8U || response[0] != kSync ||
        response[1] != kMasterReplyAddress ||
        response[2] != expected_register_address ||
        calculate_crc(std::vector<std::uint8_t>(response.begin(),
                                                response.end() - 1U)) !=
            response.back()) {
        throw std::runtime_error("TMC2209 读响应格式或 CRC 无效");
    }
    return (static_cast<std::uint32_t>(response[3]) << 24U) |
           (static_cast<std::uint32_t>(response[4]) << 16U) |
           (static_cast<std::uint32_t>(response[5]) << 8U) |
           static_cast<std::uint32_t>(response[6]);
}

}  // namespace remotebsp
