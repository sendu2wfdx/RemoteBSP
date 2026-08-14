#include "remotebsp/protocol/waveform.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace remotebsp::protocol;

template <typename Function>
void expect_invalid(Function function) {
    bool failed = false;
    try {
        function();
    } catch (const WaveformPayloadException&) {
        failed = true;
    }
    assert(failed);
}

int main() {
    const PwmCreatePayload pwm{2, 25000, 3750, true};
    const auto pwm_bytes = encode_pwm_create(pwm);
    assert((pwm_bytes == std::vector<std::uint8_t>{
        2, 0xA8, 0x61, 0, 0, 0xA6, 0x0E, 1}));
    const auto pwm_roundtrip = decode_pwm_create(pwm_bytes);
    assert(pwm_roundtrip.channel == 2);
    assert(pwm_roundtrip.frequency_hz == 25000);
    assert(pwm_roundtrip.duty == 3750);
    assert(pwm_roundtrip.active_low);
    assert(decode_pwm_duty(encode_pwm_duty(10000)) == 10000);
    expect_invalid([] { (void)encode_pwm_duty(10001); });

    const TimedBitstreamCreatePayload timing{
        0, 1250, 350, 700, 80};
    const auto timing_roundtrip = decode_timed_bitstream_create(
        encode_timed_bitstream_create(timing));
    assert(timing_roundtrip.bit_period_ns == 1250);
    assert(timing_roundtrip.zero_high_ns == 350);
    assert(timing_roundtrip.one_high_ns == 700);
    assert(timing_roundtrip.reset_time_us == 80);

    const TimedBitstreamWritePayload bits{
        20, {0x12, 0x34, 0x50}};
    const auto bits_roundtrip = decode_timed_bitstream_write(
        encode_timed_bitstream_write(bits));
    assert(bits_roundtrip.bit_count == 20);
    assert(bits_roundtrip.data == bits.data);
    expect_invalid([] {
        (void)encode_timed_bitstream_write({9, {0x80}});
    });
    expect_invalid([] {
        (void)decode_timed_bitstream_create(
            std::vector<std::uint8_t>(17, 0));
    });
}
