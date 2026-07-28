#include "remotebsp/protocol/crc32.hpp"
#include "remotebsp/protocol/packet.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace remotebsp::protocol;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": check failed: " #condition "\n";                    \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <typename Function>
void check_error(PacketError expected, Function function) {
    try {
        function();
        CHECK(false);
    } catch (const DecodeError& error) {
        CHECK(error.code() == expected);
    }
}

Packet sample_packet() {
    Packet packet;
    packet.header.message_type = MessageType::Request;
    packet.header.command = static_cast<std::uint16_t>(Command::Ping);
    packet.header.session_id = 0x11223344U;
    packet.header.request_id = 0x55667788U;
    packet.header.object_id = 0x99AABBCCU;
    packet.header.flags = 0x1234U;
    packet.payload = {0xDE, 0xAD, 0xBE, 0xEF};
    return packet;
}

void test_crc_known_vector() {
    const std::string input = "123456789";
    CHECK(crc32(reinterpret_cast<const std::uint8_t*>(input.data()),
                input.size()) == 0xCBF43926U);
}

void test_round_trip_and_endian() {
    const auto bytes = encode(sample_packet());
    CHECK(bytes.size() == 28);
    CHECK(bytes[2] == 0x12);
    CHECK(bytes[3] == 0x00);
    CHECK(bytes[4] == 0x44);
    CHECK(bytes[5] == 0x33);
    CHECK(bytes[6] == 0x22);
    CHECK(bytes[7] == 0x11);
    CHECK(bytes[16] == 0x04);
    CHECK(bytes[17] == 0x00);

    const auto packet = decode(bytes);
    CHECK(packet.header.version == kProtocolVersion);
    CHECK(packet.header.message_type == MessageType::Request);
    CHECK(packet.header.command == static_cast<std::uint16_t>(Command::Ping));
    CHECK(packet.header.session_id == 0x11223344U);
    CHECK(packet.header.request_id == 0x55667788U);
    CHECK(packet.header.object_id == 0x99AABBCCU);
    CHECK(packet.header.flags == 0x1234U);
    CHECK(packet.header.payload_length == 4);
    CHECK(packet.payload == std::vector<std::uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}));
}

void test_validation() {
    auto bytes = encode(sample_packet());

    check_error(PacketError::TooShort,
                [&] { decode(bytes.data(), kPacketHeaderSize - 1); });

    auto bad_version = bytes;
    bad_version[0] = 2;
    check_error(PacketError::UnsupportedVersion, [&] { decode(bad_version); });

    auto bad_type = bytes;
    bad_type[1] = 0xFF;
    check_error(PacketError::InvalidMessageType, [&] { decode(bad_type); });

    auto bad_length = bytes;
    bad_length[16] = 5;
    check_error(PacketError::LengthMismatch, [&] { decode(bad_length); });

    auto bad_crc = bytes;
    bad_crc.back() ^= 1U;
    check_error(PacketError::CrcMismatch, [&] { decode(bad_crc); });

    Packet too_large;
    too_large.payload.resize(kMaximumPayloadSize + 1);
    check_error(PacketError::TooLarge, [&] { encode(too_large); });
}

}

int main() {
    test_crc_known_vector();
    test_round_trip_and_endian();
    test_validation();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All protocol tests passed\n";
    return 0;
}
