#pragma once

#include "remotebsp/protocol/packet.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::protocol {

constexpr std::size_t kFragmentHeaderSize = 5;
constexpr std::uint8_t kFragmentFirst = 0x01;
constexpr std::uint8_t kFragmentLast = 0x02;
constexpr std::uint8_t kFragmentFlagMask =
    kFragmentFirst | kFragmentLast;
constexpr unsigned kFragmentLengthShift = 2;
constexpr std::uint8_t kFragmentLengthMask = 0xFC;

struct Fragment {
    std::uint16_t transfer_id{};
    std::uint16_t sequence{};
    std::uint8_t flags{};
    std::vector<std::uint8_t> payload;
};

enum class FragmentError {
    InvalidMtu,
    EmptyPacket,
    PacketTooLarge,
    FrameTooShort,
    FrameTooLarge,
    InvalidFlags,
    InvalidPadding,
    InvalidSequence,
    UnknownTransfer,
    ConflictingDuplicate,
    DataAfterLast,
    ReassembledPacketTooLarge,
};

class FragmentException : public std::runtime_error {
public:
    FragmentException(FragmentError code, const char* message);
    FragmentError code() const noexcept;

private:
    FragmentError code_;
};

std::vector<std::uint8_t> encode_fragment(const Fragment& fragment,
                                          std::size_t mtu);
Fragment decode_fragment(const std::uint8_t* data, std::size_t size,
                         std::size_t mtu);
inline Fragment decode_fragment(const std::vector<std::uint8_t>& frame,
                                std::size_t mtu) {
    return decode_fragment(frame.data(), frame.size(), mtu);
}

class Fragmenter {
public:
    explicit Fragmenter(std::size_t mtu);

    std::vector<std::vector<std::uint8_t>> split(
        const std::vector<std::uint8_t>& packet,
        std::uint16_t transfer_id) const;
    std::size_t mtu() const noexcept;

private:
    std::size_t mtu_;
};

enum class ReassemblyStatus {
    InProgress,
    Complete,
    Duplicate,
};

struct ReassemblyResult {
    ReassemblyStatus status{ReassemblyStatus::InProgress};
    std::optional<std::vector<std::uint8_t>> packet;
};

class Reassembler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Reassembler(std::size_t mtu, std::chrono::milliseconds timeout);

    // stream_id 用于区分发送方，例如可以使用 SocketCAN 的源 CAN ID。
    ReassemblyResult accept(std::uint32_t stream_id,
                            const std::vector<std::uint8_t>& frame,
                            TimePoint now = Clock::now());
    std::size_t expire(TimePoint now = Clock::now());
    std::size_t pending() const noexcept;

private:
    struct Transfer {
        TimePoint last_update;
        std::unordered_map<std::uint16_t, std::vector<std::uint8_t>> pieces;
        std::unordered_map<std::uint16_t, std::uint8_t> piece_flags;
        std::optional<std::uint16_t> last_sequence;
        std::size_t received_size{};
        bool complete{};
        std::vector<std::uint8_t> completed_packet;
    };

    static std::uint64_t key(std::uint32_t stream_id,
                             std::uint16_t transfer_id) noexcept;
    std::optional<std::vector<std::uint8_t>> try_complete(Transfer& transfer);

    std::size_t mtu_;
    std::chrono::milliseconds timeout_;
    std::unordered_map<std::uint64_t, Transfer> transfers_;
};

}
