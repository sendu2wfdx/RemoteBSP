#include "remotebsp/tmc2209.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

int main() {
    const auto read_request = remotebsp::Tmc2209::make_read_request(0U, 0x06U);
    assert((read_request == std::vector<std::uint8_t>{0x05U, 0x00U, 0x06U, 0x6fU}));

    const auto write_request =
        remotebsp::Tmc2209::make_write_request(0U, 0x00U, 0x000000c0U);
    assert(write_request.size() == 8U);
    assert(write_request[0] == 0x05U);
    assert(write_request[1] == 0x00U);
    assert(write_request[2] == 0x80U);
    assert(write_request[6] == 0xc0U);

    const std::vector<std::uint8_t> response{
        0x05U, 0xffU, 0x06U, 0x21U, 0x00U, 0x00U, 0x41U, 0xc6U};
    assert(remotebsp::Tmc2209::decode_read_response(response, 0x06U) ==
           0x21000041U);

    bool rejected = false;
    auto invalid_response = response;
    invalid_response.back() ^= 0x01U;
    try {
        static_cast<void>(remotebsp::Tmc2209::decode_read_response(
            invalid_response, 0x06U));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
