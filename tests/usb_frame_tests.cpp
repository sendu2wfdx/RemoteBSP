#include "remotebsp/transport/usb_frame_codec.hpp"
#include "remotebsp/transport/libusb_transport.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace remotebsp::transport;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                      \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

void test_split_and_coalesced_input() {
    const LinkFrame first{0x700U, {1U, 2U, 3U}};
    const LinkFrame second{0x581U, std::vector<std::uint8_t>(64U, 0x5AU)};
    const auto first_wire = encode_usb_frame(first);
    const auto second_wire = encode_usb_frame(second);

    UsbFrameDecoder decoder;
    decoder.append(first_wire.data(), 5U);
    CHECK(!decoder.pop().has_value());
    std::vector<std::uint8_t> remainder(
        first_wire.begin() + 5, first_wire.end());
    remainder.insert(remainder.end(), second_wire.begin(), second_wire.end());
    decoder.append(remainder);

    const auto first_output = decoder.pop();
    CHECK(first_output.has_value());
    CHECK(first_output->route == first.route);
    CHECK(first_output->data == first.data);
    const auto second_output = decoder.pop();
    CHECK(second_output.has_value());
    CHECK(second_output->route == second.route);
    CHECK(second_output->data == second.data);
    CHECK(!decoder.pop().has_value());
}

void test_invalid_input() {
    try {
        static_cast<void>(encode_usb_frame(LinkFrame{1U, {}}));
        CHECK(false);
    } catch (const UsbFrameException& error) {
        CHECK(error.code() == UsbFrameError::EmptyPayload);
    }

    auto wire = encode_usb_frame(LinkFrame{1U, {1U}});
    wire[4] = 99U;
    UsbFrameDecoder decoder;
    decoder.append(wire);
    try {
        static_cast<void>(decoder.pop());
        CHECK(false);
    } catch (const UsbFrameException& error) {
        CHECK(error.code() == UsbFrameError::UnsupportedVersion);
    }
}

void test_device_selector() {
    const auto selector = parse_usb_device_selector(
        "0x1234:0x5678@BOARD-01");
    CHECK(selector.vendor_id == 0x1234U);
    CHECK(selector.product_id == 0x5678U);
    CHECK(selector.serial_number == "BOARD-01");
    try {
        static_cast<void>(parse_usb_device_selector("1234"));
        CHECK(false);
    } catch (const std::invalid_argument&) {
    }
}

}

int main() {
    test_split_and_coalesced_input();
    test_invalid_input();
    test_device_selector();
    if (failures != 0) {
        std::cerr << failures << " 个 USB 帧测试失败\n";
        return 1;
    }
    std::cout << "USB 链路帧测试通过\n";
    return 0;
}
