#include "remotebsp/protocol/fragmentation.hpp"

#include <algorithm>
#include <limits>

namespace remotebsp::protocol {
namespace {

void validate_mtu(std::size_t mtu) {
    if (mtu <= kFragmentHeaderSize) {
        throw FragmentException(FragmentError::InvalidMtu,
                                "MTU leaves no room for fragment data");
    }
}

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8U);
}

std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(in[0]) |
        (static_cast<std::uint16_t>(in[1]) << 8U));
}

}

FragmentException::FragmentException(FragmentError code, const char* message)
    : std::runtime_error(message), code_(code) {}

FragmentError FragmentException::code() const noexcept { return code_; }

std::vector<std::uint8_t> encode_fragment(const Fragment& fragment,
                                          std::size_t mtu) {
    validate_mtu(mtu);
    if ((fragment.flags & ~kFragmentFlagMask) != 0U) {
        throw FragmentException(FragmentError::InvalidFlags,
                                "fragment has reserved flag bits");
    }
    if (((fragment.flags & kFragmentFirst) != 0U) !=
        (fragment.sequence == 0U)) {
        throw FragmentException(FragmentError::InvalidSequence,
                                "only sequence zero may be first");
    }
    if (fragment.payload.empty()) {
        throw FragmentException(FragmentError::EmptyPacket,
                                "fragment payload cannot be empty");
    }
    if (kFragmentHeaderSize + fragment.payload.size() > mtu) {
        throw FragmentException(FragmentError::FrameTooLarge,
                                "fragment exceeds MTU");
    }
    if (fragment.payload.size() >
        (kFragmentLengthMask >> kFragmentLengthShift)) {
        throw FragmentException(FragmentError::FrameTooLarge,
                                "fragment payload length cannot be encoded");
    }

    std::vector<std::uint8_t> frame(kFragmentHeaderSize +
                                    fragment.payload.size());
    put_u16(frame.data(), fragment.transfer_id);
    put_u16(frame.data() + 2, fragment.sequence);
    /*
     * CAN-FD 在线路上只能使用 0..8、12、16、20、24、32、48、64
     * 这些长度。驱动可能在末片后补零，因此把真实载荷长度放在标志字节
     * 的高 6 位，低两位仍表示首片和末片，分片头仍保持 5 字节。
     */
    frame[4] = static_cast<std::uint8_t>(
        fragment.flags |
        (static_cast<std::uint8_t>(fragment.payload.size())
         << kFragmentLengthShift));
    std::copy(fragment.payload.begin(), fragment.payload.end(),
              frame.begin() + static_cast<std::ptrdiff_t>(kFragmentHeaderSize));
    return frame;
}

Fragment decode_fragment(const std::uint8_t* data, std::size_t size,
                         std::size_t mtu) {
    validate_mtu(mtu);
    if (size <= kFragmentHeaderSize) {
        throw FragmentException(FragmentError::FrameTooShort,
                                "frame has no fragment data");
    }
    if (size > mtu) {
        throw FragmentException(FragmentError::FrameTooLarge,
                                "frame exceeds MTU");
    }

    const std::size_t payload_length =
        static_cast<std::size_t>(
            (data[4] & kFragmentLengthMask) >> kFragmentLengthShift);
    if (payload_length == 0U) {
        throw FragmentException(FragmentError::EmptyPacket,
                                "fragment payload cannot be empty");
    }
    if (kFragmentHeaderSize + payload_length > mtu) {
        throw FragmentException(FragmentError::FrameTooLarge,
                                "encoded fragment payload exceeds MTU");
    }
    if (size < kFragmentHeaderSize + payload_length) {
        throw FragmentException(FragmentError::FrameTooShort,
                                "frame is shorter than encoded payload");
    }
    if (!std::all_of(data + kFragmentHeaderSize + payload_length,
                     data + size,
                     [](std::uint8_t byte) { return byte == 0U; })) {
        throw FragmentException(FragmentError::InvalidPadding,
                                "CAN-FD padding must be zero");
    }

    Fragment fragment;
    fragment.transfer_id = get_u16(data);
    fragment.sequence = get_u16(data + 2);
    fragment.flags = data[4] & kFragmentFlagMask;
    if (((fragment.flags & kFragmentFirst) != 0U) !=
        (fragment.sequence == 0U)) {
        throw FragmentException(FragmentError::InvalidSequence,
                                "only sequence zero may be first");
    }
    fragment.payload.assign(data + kFragmentHeaderSize,
                            data + kFragmentHeaderSize + payload_length);
    return fragment;
}

Fragmenter::Fragmenter(std::size_t mtu) : mtu_(mtu) { validate_mtu(mtu_); }

std::vector<std::vector<std::uint8_t>> Fragmenter::split(
    const std::vector<std::uint8_t>& packet,
    std::uint16_t transfer_id) const {
    if (packet.empty()) {
        throw FragmentException(FragmentError::EmptyPacket,
                                "packet cannot be empty");
    }
    if (packet.size() > kMaximumPacketSize) {
        throw FragmentException(FragmentError::PacketTooLarge,
                                "packet exceeds maximum size");
    }

    const std::size_t capacity = mtu_ - kFragmentHeaderSize;
    const std::size_t count = (packet.size() + capacity - 1U) / capacity;
    if (count > std::numeric_limits<std::uint16_t>::max()) {
        throw FragmentException(FragmentError::PacketTooLarge,
                                "packet needs too many fragments");
    }

    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset = i * capacity;
        const std::size_t length = std::min(capacity, packet.size() - offset);
        Fragment fragment;
        fragment.transfer_id = transfer_id;
        fragment.sequence = static_cast<std::uint16_t>(i);
        fragment.flags = 0;
        if (i == 0) {
            fragment.flags |= kFragmentFirst;
        }
        if (i + 1 == count) {
            fragment.flags |= kFragmentLast;
        }
        fragment.payload.assign(packet.begin() +
                                    static_cast<std::ptrdiff_t>(offset),
                                packet.begin() + static_cast<std::ptrdiff_t>(
                                                     offset + length));
        frames.push_back(encode_fragment(fragment, mtu_));
    }
    return frames;
}

std::size_t Fragmenter::mtu() const noexcept { return mtu_; }

Reassembler::Reassembler(std::size_t mtu, std::chrono::milliseconds timeout)
    : mtu_(mtu), timeout_(timeout) {
    validate_mtu(mtu_);
    if (timeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("reassembly timeout must be positive");
    }
}

std::uint64_t Reassembler::key(std::uint32_t stream_id,
                               std::uint16_t transfer_id) noexcept {
    return (static_cast<std::uint64_t>(stream_id) << 16U) | transfer_id;
}

ReassemblyResult Reassembler::accept(std::uint32_t stream_id,
                                     const std::vector<std::uint8_t>& frame,
                                     TimePoint now) {
    expire(now);
    const Fragment fragment = decode_fragment(frame, mtu_);
    const auto transfer_key = key(stream_id, fragment.transfer_id);
    auto found = transfers_.find(transfer_key);

    if (found == transfers_.end()) {
        if ((fragment.flags & kFragmentFirst) == 0U) {
            throw FragmentException(FragmentError::UnknownTransfer,
                                    "transfer must begin with first fragment");
        }
        found = transfers_
                    .emplace(transfer_key,
                             Transfer{now, {}, {}, std::nullopt, 0, false, {}})
                    .first;
    }

    Transfer& transfer = found->second;
    const auto duplicate = transfer.pieces.find(fragment.sequence);
    if (duplicate != transfer.pieces.end()) {
        if (duplicate->second != fragment.payload ||
            transfer.piece_flags.at(fragment.sequence) != fragment.flags) {
            throw FragmentException(FragmentError::ConflictingDuplicate,
                                    "duplicate sequence has different content");
        }
        transfer.last_update = now;
        return {ReassemblyStatus::Duplicate,
                transfer.complete
                    ? std::optional<std::vector<std::uint8_t>>(
                          transfer.completed_packet)
                    : std::nullopt};
    }
    if (transfer.complete ||
        (transfer.last_sequence.has_value() &&
         fragment.sequence > *transfer.last_sequence)) {
        throw FragmentException(FragmentError::DataAfterLast,
                                "fragment sequence follows last fragment");
    }
    if ((fragment.flags & kFragmentLast) != 0U &&
        transfer.last_sequence.has_value() &&
        *transfer.last_sequence != fragment.sequence) {
        throw FragmentException(FragmentError::InvalidSequence,
                                "transfer has multiple last fragments");
    }
    if (transfer.received_size + fragment.payload.size() >
        kMaximumPacketSize) {
        transfers_.erase(found);
        throw FragmentException(FragmentError::ReassembledPacketTooLarge,
                                "reassembled packet exceeds maximum size");
    }

    transfer.last_update = now;
    transfer.received_size += fragment.payload.size();
    transfer.pieces.emplace(fragment.sequence, fragment.payload);
    transfer.piece_flags.emplace(fragment.sequence, fragment.flags);
    if ((fragment.flags & kFragmentLast) != 0U) {
        transfer.last_sequence = fragment.sequence;
    }

    auto packet = try_complete(transfer);
    if (packet.has_value()) {
        transfer.complete = true;
        transfer.completed_packet = *packet;
        return {ReassemblyStatus::Complete, std::move(packet)};
    }
    return {ReassemblyStatus::InProgress, std::nullopt};
}

std::optional<std::vector<std::uint8_t>> Reassembler::try_complete(
    Transfer& transfer) {
    if (!transfer.last_sequence.has_value()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> packet;
    packet.reserve(transfer.received_size);
    for (std::uint32_t sequence = 0;
         sequence <= static_cast<std::uint32_t>(*transfer.last_sequence);
         ++sequence) {
        const auto piece =
            transfer.pieces.find(static_cast<std::uint16_t>(sequence));
        if (piece == transfer.pieces.end()) {
            return std::nullopt;
        }
        packet.insert(packet.end(), piece->second.begin(), piece->second.end());
    }
    return packet;
}

std::size_t Reassembler::expire(TimePoint now) {
    std::size_t removed = 0;
    for (auto iterator = transfers_.begin(); iterator != transfers_.end();) {
        if (now - iterator->second.last_update >= timeout_) {
            iterator = transfers_.erase(iterator);
            ++removed;
        } else {
            ++iterator;
        }
    }
    return removed;
}

std::size_t Reassembler::pending() const noexcept { return transfers_.size(); }

}
