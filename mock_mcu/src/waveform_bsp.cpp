#include "remotebsp/mock_mcu/waveform_bsp.hpp"

#include <algorithm>
#include <stdexcept>

namespace remotebsp::mock_mcu {

void WaveformBsp::pwm_configure(
    const protocol::PwmCreatePayload& config) {
    static_cast<void>(protocol::encode_pwm_create(config));
    auto& state = pwm_[config.channel];
    state.channel = config.channel;
    state.frequency_hz = config.frequency_hz;
    state.duty = config.duty;
    state.active_low = config.active_low;
    state.running = true;
    ++state.update_count;
}

void WaveformBsp::pwm_write(std::uint8_t channel, std::uint16_t duty) {
    static_cast<void>(protocol::encode_pwm_duty(duty));
    auto found = pwm_.find(channel);
    if (found == pwm_.end()) {
        throw std::runtime_error("PWM 通道尚未配置");
    }
    found->second.duty = duty;
    found->second.running = true;
    ++found->second.update_count;
}

void WaveformBsp::pwm_stop(std::uint8_t channel) {
    auto found = pwm_.find(channel);
    if (found == pwm_.end()) {
        throw std::runtime_error("PWM 通道尚未配置");
    }
    found->second.running = false;
    found->second.duty = 0;
    ++found->second.update_count;
}

void WaveformBsp::bitstream_configure(
    const protocol::TimedBitstreamCreatePayload& config) {
    static_cast<void>(protocol::encode_timed_bitstream_create(config));
    auto& state = bitstreams_[config.channel];
    state.timing = config;
    state.bit_count = 0;
    state.data.clear();
    state.busy = false;
}

void WaveformBsp::bitstream_write(
    std::uint8_t channel, std::uint16_t bit_count,
    const std::vector<std::uint8_t>& data) {
    static_cast<void>(protocol::encode_timed_bitstream_write(
        {bit_count, data}));
    auto found = bitstreams_.find(channel);
    if (found == bitstreams_.end()) {
        throw std::runtime_error("定时位流通道尚未配置");
    }
    if (found->second.busy) {
        throw std::runtime_error("定时位流通道忙");
    }
    found->second.busy = true;
    found->second.bit_count = bit_count;
    found->second.data = data;
    ++found->second.write_count;
    // Mock 在同一原子调用内完成输出，但保留最后一帧供数字孪生观察。
    found->second.busy = false;
}

void WaveformBsp::bitstream_abort(std::uint8_t channel) {
    auto found = bitstreams_.find(channel);
    if (found == bitstreams_.end()) {
        throw std::runtime_error("定时位流通道尚未配置");
    }
    found->second.busy = false;
}

void WaveformBsp::reset_pwm(std::uint8_t channel) {
    pwm_.erase(channel);
}

void WaveformBsp::reset_bitstream(std::uint8_t channel) {
    bitstreams_.erase(channel);
}

void WaveformBsp::reset_all() {
    pwm_.clear();
    bitstreams_.clear();
}

std::vector<PwmSnapshot> WaveformBsp::pwm_snapshot() const {
    std::vector<PwmSnapshot> result;
    for (const auto& entry : pwm_) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return left.channel < right.channel;
              });
    return result;
}

std::vector<TimedBitstreamSnapshot>
WaveformBsp::bitstream_snapshot() const {
    std::vector<TimedBitstreamSnapshot> result;
    for (const auto& entry : bitstreams_) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) {
                  return left.timing.channel < right.timing.channel;
              });
    return result;
}

}
