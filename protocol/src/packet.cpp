#include "remotebsp/protocol/packet.hpp"

#include "remotebsp/protocol/crc32.hpp"

#include <algorithm>
#include <limits>

namespace remotebsp::protocol {
namespace {

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
}

std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(in[0]) |
        (static_cast<std::uint16_t>(in[1]) << 8U));
}

std::uint32_t get_u32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8U) |
           (static_cast<std::uint32_t>(in[2]) << 16U) |
           (static_cast<std::uint32_t>(in[3]) << 24U);
}

bool valid_message_type(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(MessageType::Request) &&
           value <= static_cast<std::uint8_t>(MessageType::Event);
}

void write_header_without_crc(std::uint8_t* out, const PacketHeader& header) {
    out[0] = header.version;
    out[1] = static_cast<std::uint8_t>(header.message_type);
    put_u16(out + 2, header.command);
    put_u32(out + 4, header.session_id);
    put_u32(out + 8, header.request_id);
    put_u32(out + 12, header.object_id);
    put_u16(out + 16, header.payload_length);
    put_u16(out + 18, header.flags);
}

}

DecodeError::DecodeError(PacketError code, const char* message)
    : std::runtime_error(message), code_(code) {}

PacketError DecodeError::code() const noexcept { return code_; }

std::vector<std::uint8_t> encode(Packet packet) {
    if (packet.header.version != kProtocolVersion) {
        throw DecodeError(PacketError::UnsupportedVersion,
                          "unsupported protocol version");
    }
    if (!valid_message_type(
            static_cast<std::uint8_t>(packet.header.message_type))) {
        throw DecodeError(PacketError::InvalidMessageType,
                          "invalid message type");
    }
    if (packet.payload.size() > kMaximumPayloadSize ||
        packet.payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw DecodeError(PacketError::TooLarge, "packet exceeds maximum size");
    }

    packet.header.payload_length =
        static_cast<std::uint16_t>(packet.payload.size());
    std::vector<std::uint8_t> bytes(kPacketHeaderSize + packet.payload.size());
    write_header_without_crc(bytes.data(), packet.header);
    std::copy(packet.payload.begin(), packet.payload.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderSize));

    // CRC 字段本身不参与计算，校验输入由头部字节 [0, 20) 和载荷组成。
    std::vector<std::uint8_t> crc_input;
    crc_input.reserve(20 + packet.payload.size());
    crc_input.insert(crc_input.end(), bytes.begin(), bytes.begin() + 20);
    crc_input.insert(crc_input.end(), packet.payload.begin(),
                     packet.payload.end());
    packet.header.crc = crc32(crc_input.data(), crc_input.size());
    put_u32(bytes.data() + 20, packet.header.crc);
    return bytes;
}

Packet decode(const std::uint8_t* data, std::size_t size) {
    if (size < kPacketHeaderSize) {
        throw DecodeError(PacketError::TooShort, "packet is shorter than header");
    }
    if (size > kMaximumPacketSize) {
        throw DecodeError(PacketError::TooLarge, "packet exceeds maximum size");
    }
    if (data[0] != kProtocolVersion) {
        throw DecodeError(PacketError::UnsupportedVersion,
                          "unsupported protocol version");
    }
    if (!valid_message_type(data[1])) {
        throw DecodeError(PacketError::InvalidMessageType,
                          "invalid message type");
    }

    const auto payload_length = get_u16(data + 16);
    if (size != kPacketHeaderSize + payload_length) {
        throw DecodeError(PacketError::LengthMismatch,
                          "payload length does not match packet size");
    }

    std::vector<std::uint8_t> crc_input;
    crc_input.reserve(20 + payload_length);
    crc_input.insert(crc_input.end(), data, data + 20);
    crc_input.insert(crc_input.end(), data + kPacketHeaderSize, data + size);
    const auto expected_crc = crc32(crc_input.data(), crc_input.size());
    const auto received_crc = get_u32(data + 20);
    if (expected_crc != received_crc) {
        throw DecodeError(PacketError::CrcMismatch, "packet CRC mismatch");
    }

    Packet packet;
    packet.header.version = data[0];
    packet.header.message_type = static_cast<MessageType>(data[1]);
    packet.header.command = get_u16(data + 2);
    packet.header.session_id = get_u32(data + 4);
    packet.header.request_id = get_u32(data + 8);
    packet.header.object_id = get_u32(data + 12);
    packet.header.payload_length = payload_length;
    packet.header.flags = get_u16(data + 18);
    packet.header.crc = received_crc;
    packet.payload.assign(data + kPacketHeaderSize, data + size);
    return packet;
}

}
