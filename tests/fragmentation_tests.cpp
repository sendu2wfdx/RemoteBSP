#include "remotebsp/protocol/fragmentation.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
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
void check_error(FragmentError expected, Function function) {
    try {
        function();
        CHECK(false);
    } catch (const FragmentException& error) {
        CHECK(error.code() == expected);
    }
}

std::vector<std::uint8_t> make_packet(std::size_t size) {
    std::vector<std::uint8_t> packet(size);
    for (std::size_t i = 0; i < size; ++i) {
        packet[i] = static_cast<std::uint8_t>((i * 37U) & 0xFFU);
    }
    return packet;
}

void test_round_trip(std::size_t mtu) {
    const auto packet = make_packet(2048);
    Fragmenter fragmenter(mtu);
    const auto frames = fragmenter.split(packet, 0x1234);
    CHECK(!frames.empty());
    CHECK(frames.front().size() <= mtu);

    const auto first = decode_fragment(frames.front(), mtu);
    const auto last = decode_fragment(frames.back(), mtu);
    CHECK(first.sequence == 0);
    CHECK(first.transfer_id == 0x1234);
    CHECK((first.flags & kFragmentFirst) != 0);
    CHECK((last.flags & kFragmentLast) != 0);

    Reassembler reassembler(mtu, std::chrono::milliseconds(100));
    ReassemblyResult result;
    for (const auto& frame : frames) {
        result = reassembler.accept(7, frame);
    }
    CHECK(result.status == ReassemblyStatus::Complete);
    CHECK(result.packet.has_value());
    CHECK(*result.packet == packet);
}

void test_out_of_order_and_duplicate() {
    const auto packet = make_packet(100);
    const auto frames = Fragmenter(8).split(packet, 9);
    Reassembler reassembler(8, std::chrono::milliseconds(100));

    CHECK(reassembler.accept(1, frames[0]).status ==
          ReassemblyStatus::InProgress);
    CHECK(reassembler.accept(1, frames[0]).status ==
          ReassemblyStatus::Duplicate);
    for (std::size_t i = frames.size(); i-- > 1;) {
        const auto result = reassembler.accept(1, frames[i]);
        if (i == 1) {
            CHECK(result.status == ReassemblyStatus::Complete);
            CHECK(result.packet == packet);
        }
    }
}

void test_timeout_and_stream_isolation() {
    const auto frames = Fragmenter(64).split(make_packet(100), 3);
    Reassembler reassembler(64, std::chrono::milliseconds(50));
    const auto start = Reassembler::TimePoint{};
    reassembler.accept(10, frames[0], start);
    reassembler.accept(11, frames[0], start);
    CHECK(reassembler.pending() == 2);
    CHECK(reassembler.expire(start + std::chrono::milliseconds(49)) == 0);
    CHECK(reassembler.expire(start + std::chrono::milliseconds(50)) == 2);
    check_error(FragmentError::UnknownTransfer, [&] {
        reassembler.accept(10, frames[1],
                           start + std::chrono::milliseconds(51));
    });
}

void test_invalid_fragments() {
    check_error(FragmentError::InvalidMtu, [] { Fragmenter fragmenter(5); });
    check_error(FragmentError::EmptyPacket,
                [] { Fragmenter(8).split({}, 1); });
    check_error(FragmentError::PacketTooLarge, [] {
        Fragmenter(64).split(make_packet(kMaximumPacketSize + 1), 1);
    });

    Fragment invalid_first{1, 2, kFragmentFirst, {1}};
    check_error(FragmentError::InvalidSequence,
                [&] { encode_fragment(invalid_first, 8); });

    const std::vector<std::uint8_t> invalid_length = {1, 0, 0, 0, 0x80, 1};
    check_error(FragmentError::FrameTooLarge,
                [&] { decode_fragment(invalid_length, 8); });

    auto padded = Fragmenter(64).split(make_packet(30), 7).back();
    padded.resize(48, 0);
    CHECK(decode_fragment(padded, 64).payload.size() == 30);
    padded.back() = 1;
    check_error(FragmentError::InvalidPadding,
                [&] { decode_fragment(padded, 64); });

    const auto frames = Fragmenter(8).split(make_packet(20), 2);
    Reassembler reassembler(8, std::chrono::milliseconds(100));
    check_error(FragmentError::UnknownTransfer,
                [&] { reassembler.accept(1, frames[1]); });

    reassembler.accept(1, frames[0]);
    auto conflicting = frames[0];
    conflicting.back() ^= 1U;
    check_error(FragmentError::ConflictingDuplicate,
                [&] { reassembler.accept(1, conflicting); });

    auto conflicting_flags = frames[0];
    conflicting_flags[4] |= kFragmentLast;
    check_error(FragmentError::ConflictingDuplicate,
                [&] { reassembler.accept(1, conflicting_flags); });
}

}

int main() {
    test_round_trip(8);
    test_round_trip(64);
    test_out_of_order_and_duplicate();
    test_timeout_and_stream_isolation();
    test_invalid_fragments();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All fragmentation tests passed\n";
    return 0;
}
