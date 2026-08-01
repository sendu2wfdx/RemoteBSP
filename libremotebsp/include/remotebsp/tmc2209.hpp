#pragma once

#include <cstdint>
#include <vector>

namespace remotebsp {

/* Linux 侧 TMC2209 UART 编帧与响应校验，不属于 MCU Remote Core。 */
class Tmc2209 {
public:
    static std::vector<std::uint8_t> make_read_request(
        std::uint8_t node_address, std::uint8_t register_address);
    static std::vector<std::uint8_t> make_write_request(
        std::uint8_t node_address, std::uint8_t register_address,
        std::uint32_t value);
    static std::uint32_t decode_read_response(
        const std::vector<std::uint8_t>& response,
        std::uint8_t expected_register_address);

private:
    static std::uint8_t calculate_crc(
        const std::vector<std::uint8_t>& data);
};

}  // namespace remotebsp
