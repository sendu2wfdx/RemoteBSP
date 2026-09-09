#include "remotebsp/protocol/time_sync.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace remotebsp;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << ": 检查失败: " #condition "\n";                       \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <typename Function>
void expect_error(Function function,
                  protocol::TimeSyncPayloadError expected) {
    try {
        function();
        CHECK(false);
    } catch (const protocol::TimeSyncPayloadException& error) {
        CHECK(error.code() == expected);
    }
}

void test_request_codec() {
    const auto encoded = protocol::encode_time_sync_request();
    CHECK(encoded == std::vector<std::uint8_t>({1U, 0U, 0U, 0U}));
    const auto decoded = protocol::decode_time_sync_request(encoded);
    CHECK(decoded.version == protocol::kTimeSyncPayloadVersion);
    CHECK(decoded.flags == 0U);

    expect_error(
        [] { protocol::decode_time_sync_request({1U, 0U, 0U}); },
        protocol::TimeSyncPayloadError::InvalidLength);
    expect_error(
        [] { protocol::decode_time_sync_request({2U, 0U, 0U, 0U}); },
        protocol::TimeSyncPayloadError::UnsupportedVersion);
    expect_error(
        [] { protocol::decode_time_sync_request({1U, 1U, 0U, 0U}); },
        protocol::TimeSyncPayloadError::UnsupportedFlags);
    expect_error(
        [] { protocol::decode_time_sync_request({1U, 0U, 1U, 0U}); },
        protocol::TimeSyncPayloadError::UnsupportedFlags);
}

void test_response_codec_and_wrap() {
    const protocol::TimeSyncResponsePayload payload{
        1U, 64U, 0U, 0x1122334455667788ULL, 1000000ULL,
        0x0102030405060708ULL, 0x0102030405060718ULL};
    const auto encoded = protocol::encode_time_sync_response(payload);
    CHECK(encoded.size() == protocol::kTimeSyncResponsePayloadSize);
    const auto decoded = protocol::decode_time_sync_response(encoded);
    CHECK(decoded.version == payload.version);
    CHECK(decoded.counter_bits == payload.counter_bits);
    CHECK(decoded.boot_epoch == payload.boot_epoch);
    CHECK(decoded.nominal_tick_rate_hz == payload.nominal_tick_rate_hz);
    CHECK(decoded.node_receive_tick == payload.node_receive_tick);
    CHECK(decoded.node_send_tick == payload.node_send_tick);

    const protocol::TimeSyncResponsePayload wrapped{
        1U, 32U, 0U, 9U, 1000000ULL, 0xFFFFFFF0ULL, 0x10ULL};
    const auto wrapped_decoded = protocol::decode_time_sync_response(
        protocol::encode_time_sync_response(wrapped));
    CHECK(wrapped_decoded.node_receive_tick == 0xFFFFFFF0ULL);
    CHECK(wrapped_decoded.node_send_tick == 0x10ULL);
}

void test_response_boundaries() {
    const protocol::TimeSyncResponsePayload valid{
        1U, 64U, 0U, 1U, 1000000ULL, 100U, 120U};
    auto encoded = protocol::encode_time_sync_response(valid);
    encoded.pop_back();
    expect_error(
        [&] { protocol::decode_time_sync_response(encoded); },
        protocol::TimeSyncPayloadError::InvalidLength);

    auto mutate = [&](std::size_t index, std::uint8_t value,
                      protocol::TimeSyncPayloadError expected) {
        auto bytes = protocol::encode_time_sync_response(valid);
        bytes[index] = value;
        expect_error(
            [&] { protocol::decode_time_sync_response(bytes); }, expected);
    };
    mutate(0U, 2U, protocol::TimeSyncPayloadError::UnsupportedVersion);
    mutate(2U, 1U, protocol::TimeSyncPayloadError::UnsupportedFlags);
    mutate(4U, 0U, protocol::TimeSyncPayloadError::InvalidBootEpoch);
    mutate(1U, 1U, protocol::TimeSyncPayloadError::InvalidCounterBits);

    auto zero_rate = valid;
    zero_rate.nominal_tick_rate_hz = 0U;
    expect_error(
        [&] { protocol::encode_time_sync_response(zero_rate); },
        protocol::TimeSyncPayloadError::InvalidTickRate);

    auto out_of_range = valid;
    out_of_range.counter_bits = 8U;
    out_of_range.node_receive_tick = 256U;
    out_of_range.node_send_tick = 1U;
    expect_error(
        [&] { protocol::encode_time_sync_response(out_of_range); },
        protocol::TimeSyncPayloadError::CounterOutOfRange);

    auto reversed = valid;
    reversed.node_send_tick = reversed.node_receive_tick - 1U;
    expect_error(
        [&] { protocol::encode_time_sync_response(reversed); },
        protocol::TimeSyncPayloadError::InvalidTickOrder);

    auto ambiguous = valid;
    ambiguous.counter_bits = 8U;
    ambiguous.node_receive_tick = 10U;
    ambiguous.node_send_tick = 138U;
    expect_error(
        [&] { protocol::encode_time_sync_response(ambiguous); },
        protocol::TimeSyncPayloadError::InvalidTickOrder);
}

}

int main() {
    test_request_codec();
    test_response_codec_and_wrap();
    test_response_boundaries();
    if (failures != 0) {
        std::cerr << failures << " 个时钟同步协议测试失败\n";
        return 1;
    }
    std::cout << "所有时钟同步协议测试通过\n";
    return 0;
}
