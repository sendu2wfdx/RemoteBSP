#pragma once

#include "remotebsp/protocol/waveform.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

struct PwmSnapshot {
    std::uint8_t channel{};
    std::uint32_t frequency_hz{};
    std::uint16_t duty{};
    bool active_low{};
    bool running{};
    std::uint64_t update_count{};
};

struct TimedBitstreamSnapshot {
    protocol::TimedBitstreamCreatePayload timing;
    std::uint16_t bit_count{};
    std::vector<std::uint8_t> data;
    bool busy{};
    std::uint64_t write_count{};
};

class WaveformBsp {
public:
    void pwm_configure(const protocol::PwmCreatePayload& config);
    void pwm_write(std::uint8_t channel, std::uint16_t duty);
    void pwm_stop(std::uint8_t channel);
    void bitstream_configure(
        const protocol::TimedBitstreamCreatePayload& config);
    void bitstream_write(std::uint8_t channel, std::uint16_t bit_count,
                         const std::vector<std::uint8_t>& data);
    void bitstream_abort(std::uint8_t channel);
    void reset_pwm(std::uint8_t channel);
    void reset_bitstream(std::uint8_t channel);
    void reset_all();

    std::vector<PwmSnapshot> pwm_snapshot() const;
    std::vector<TimedBitstreamSnapshot> bitstream_snapshot() const;

private:
    std::unordered_map<std::uint8_t, PwmSnapshot> pwm_;
    std::unordered_map<std::uint8_t, TimedBitstreamSnapshot> bitstreams_;
};

}
