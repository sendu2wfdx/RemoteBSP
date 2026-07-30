#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kPacketHeaderSize = 24;
constexpr std::size_t kMaximumPacketSize = 2048;
constexpr std::size_t kMaximumPayloadSize =
    kMaximumPacketSize - kPacketHeaderSize;
constexpr std::uint16_t kErrorResponseFlag = 0x0001;

enum class MessageType : std::uint8_t {
    Request = 1,
    Response = 2,
    Event = 3,
};

enum class Command : std::uint16_t {
    DiscoveryRequest = 0x0001,
    DiscoveryResponse = 0x0002,
    NodeAssign = 0x0003,
    Heartbeat = 0x0004,
    GetInfo = 0x0010,
    GetCapability = 0x0011,
    Ping = 0x0012,
    BootloaderEnter = 0x0013,
    BootloaderEnterUsb = 0x0014,
    ResourceEnum = 0x0030,
    ResourceDescribe = 0x0031,
    ResourceStatus = 0x0032,
    ResourceReset = 0x0033,
    GpioCreate = 0x0100,
    GpioRead = 0x0101,
    GpioWrite = 0x0102,
    UartCreate = 0x0200,
    UartRead = 0x0201,
    UartWrite = 0x0202,
    UartRxEvent = 0x0280,
};

struct PacketHeader {
    std::uint8_t version{kProtocolVersion};
    MessageType message_type{MessageType::Request};
    std::uint16_t command{};
    std::uint32_t session_id{};
    std::uint32_t request_id{};
    std::uint32_t object_id{};
    std::uint16_t payload_length{};
    std::uint16_t flags{};
    std::uint32_t crc{};
};

struct Packet {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

enum class PacketError {
    TooShort,
    TooLarge,
    UnsupportedVersion,
    InvalidMessageType,
    LengthMismatch,
    CrcMismatch,
};

class DecodeError : public std::runtime_error {
public:
    DecodeError(PacketError code, const char* message);
    PacketError code() const noexcept;

private:
    PacketError code_;
};

std::vector<std::uint8_t> encode(Packet packet);
Packet decode(const std::uint8_t* data, std::size_t size);
inline Packet decode(const std::vector<std::uint8_t>& bytes) {
    return decode(bytes.data(), bytes.size());
}

}
