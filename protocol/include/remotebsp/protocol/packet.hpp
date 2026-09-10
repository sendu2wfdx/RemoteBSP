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
    TimeSync = 0x0020,
    HealthSnapshot = 0x0021,
    ResourceEnum = 0x0030,
    ResourceDescribe = 0x0031,
    ResourceStatus = 0x0032,
    ResourceReset = 0x0033,
    ResourceContract = 0x0034,
    ResourceAcquire = 0x0035,
    ResourceRenew = 0x0036,
    ResourceRelease = 0x0037,
    ResourceLeaseStatus = 0x0038,
    DeviceParameterStatus = 0x0050,
    DeviceParameterList = 0x0051,
    DeviceParameterRead = 0x0052,
    DeviceParameterUnlock = 0x0053,
    DeviceParameterWrite = 0x0054,
    DeviceParameterLock = 0x0055,
    GpioCreate = 0x0100,
    GpioRead = 0x0101,
    GpioWrite = 0x0102,
    GpioClose = 0x0103,
    UartCreate = 0x0200,
    UartRead = 0x0201,
    UartWrite = 0x0202,
    UartRxEvent = 0x0280,
    I2cContract = 0x0300,
    I2cTransfer = 0x0301,
    SpiContract = 0x0400,
    SpiTransfer = 0x0401,
    PwmCreate = 0x0600,
    PwmWrite = 0x0601,
    PwmStop = 0x0602,
    TimedBitstreamCreate = 0x0700,
    TimedBitstreamWrite = 0x0701,
    TimedBitstreamAbort = 0x0702,
    MotionEnqueue = 0x0900,
    MotionStatus = 0x0901,
    MotionAbort = 0x0902,
    MotionClearFault = 0x0903,
    MotionContract = 0x0904,
    MotionGroupPrepare = 0x0910,
    MotionGroupCommit = 0x0911,
    MotionGroupAbort = 0x0912,
    StreamContract = 0x0A00,
    StreamOpen = 0x0A01,
    StreamData = 0x0A02,
    StreamCredit = 0x0A03,
    StreamStatus = 0x0A04,
    StreamStop = 0x0A05,
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
