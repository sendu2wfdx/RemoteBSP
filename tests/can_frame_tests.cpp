#include "remotebsp/transport/can_frame_codec.hpp"

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
                      << ": 检查失败: " #condition "\n";                        \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <typename Function>
void check_error(CanFrameError expected, Function function) {
    try {
        function();
        CHECK(false);
    } catch (const CanFrameException& error) {
        CHECK(error.code() == expected);
    }
}

void test_classical_frame() {
    const CanMessage input{0x321, false, {1, 2, 3, 4, 5, 6, 7, 8}};
    const can_frame wire = encode_classical_frame(input);
    CHECK(wire.can_id == 0x321);
    CHECK(wire.can_dlc == 8);
    CHECK(decode_classical_frame(wire).identifier == input.identifier);
    CHECK(decode_classical_frame(wire).data == input.data);

    CanMessage too_large = input;
    too_large.data.push_back(9);
    check_error(CanFrameError::PayloadTooLarge,
                [&] { encode_classical_frame(too_large); });
}

void test_fd_frame() {
    CanMessage input;
    input.identifier = 0x1ABCDE;
    input.extended_identifier = true;
    input.data.resize(64);
    for (std::size_t i = 0; i < input.data.size(); ++i) {
        input.data[i] = static_cast<std::uint8_t>(i);
    }

    const canfd_frame wire = encode_fd_frame(input);
    CHECK((wire.can_id & CAN_EFF_FLAG) != 0);
    CHECK(wire.len == 64);
    const auto output = decode_fd_frame(wire);
    CHECK(output.identifier == input.identifier);
    CHECK(output.extended_identifier);
    CHECK(output.data == input.data);

    CanMessage padded{0x123, false, std::vector<std::uint8_t>(30, 0x5A)};
    const canfd_frame padded_wire = encode_fd_frame(padded);
    CHECK(padded_wire.len == 32);
    CHECK(std::equal(padded_wire.data, padded_wire.data + 30,
                     padded.data.begin()));
    CHECK(padded_wire.data[30] == 0 && padded_wire.data[31] == 0);
}

void test_invalid_frames() {
    check_error(CanFrameError::InvalidIdentifier, [] {
        encode_classical_frame(CanMessage{CAN_SFF_MASK + 1U, false, {}});
    });

    can_frame remote{};
    remote.can_id = 1U | CAN_RTR_FLAG;
    check_error(CanFrameError::RemoteTransmissionRequest,
                [&] { decode_classical_frame(remote); });

    canfd_frame error{};
    error.can_id = CAN_ERR_FLAG;
    check_error(CanFrameError::ErrorFrame, [&] { decode_fd_frame(error); });

    canfd_frame invalid_length{};
    invalid_length.len = CANFD_MAX_DLEN + 1U;
    check_error(CanFrameError::InvalidLength,
                [&] { decode_fd_frame(invalid_length); });
}

}

int main() {
    test_classical_frame();
    test_fd_frame();
    test_invalid_frames();
    if (failures != 0) {
        std::cerr << failures << " 个测试失败\n";
        return 1;
    }
    std::cout << "所有 CAN 帧测试通过\n";
    return 0;
}
