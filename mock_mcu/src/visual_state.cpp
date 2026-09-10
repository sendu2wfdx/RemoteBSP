#include "remotebsp/mock_mcu/visual_state.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace remotebsp::mock_mcu {
namespace {

std::string json_string(std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    result += '"';
    return result;
}

const char* motion_state_name(MotionState state) {
    switch (state) {
    case MotionState::Idle: return "idle";
    case MotionState::Armed: return "armed";
    case MotionState::Running: return "running";
    case MotionState::Faulted: return "faulted";
    }
    return "unknown";
}

const char* motion_fault_name(MotionFault fault) {
    switch (fault) {
    case MotionFault::None: return "none";
    case MotionFault::Aborted: return "aborted";
    case MotionFault::LimitTriggered: return "limit-triggered";
    case MotionFault::QueueUnderrun: return "queue-underrun";
    case MotionFault::TimingDeadlineMissed: return "timing-deadline-missed";
    }
    return "unknown";
}

bool ws2812_timing(const TimedBitstreamSnapshot& state) {
    return state.timing.bit_period_ns >= 1100 &&
           state.timing.bit_period_ns <= 1400 &&
           state.timing.zero_high_ns >= 200 &&
           state.timing.zero_high_ns <= 500 &&
           state.timing.one_high_ns >= 550 &&
           state.timing.one_high_ns <= 900 &&
           state.timing.reset_time_us >= 50;
}

std::string rgb_string(std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue) {
    std::ostringstream output;
    output << "#" << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<unsigned>(red)
           << std::setw(2) << static_cast<unsigned>(green)
           << std::setw(2) << static_cast<unsigned>(blue);
    return output.str();
}

}

void write_visual_state(const DigitalTwin& twin, std::uint64_t elapsed_ms,
                        const std::string& path) {
    if (path.empty()) {
        return;
    }
    const std::string temporary_path = path + ".tmp";
    std::ofstream output(temporary_path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("无法创建 Mock 可视化状态文件");
    }
    output << "{\n  \"schema_version\": 1,\n"
           << "  \"board_name\": " << json_string(twin.manifest().name) << ",\n"
           << "  \"online\": " << (twin.online() ? "true" : "false") << ",\n"
           << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
           << "  \"gpio\": [";
    const auto gpio = twin.gpio() ? twin.gpio()->snapshot()
                                  : std::vector<GpioPinSnapshot>{};
    for (std::size_t index = 0; index < gpio.size(); ++index) {
        const auto& pin = gpio[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"pin\": " << pin.pin
               << ", \"direction\": \""
               << (pin.direction == GpioDirection::Output ? "output" : "input")
               << "\", \"value\": " << (pin.value ? "true" : "false") << "}";
    }
    output << (gpio.empty() ? "],\n" : "\n  ],\n");

    const auto motion = twin.motion() ? twin.motion()->status() : MotionStatus{};
    output << "  \"motion\": {\"state\": \"" << motion_state_name(motion.state)
           << "\", \"fault\": \"" << motion_fault_name(motion.fault)
           << "\", \"queue_depth\": " << motion.queue_depth
           << ", \"queue_capacity\": " << motion.queue_capacity
           << ", \"queue_low_watermark\": "
           << motion.queue_low_watermark
           << ", \"queue_low\": "
           << (motion.queue_low ? "true" : "false")
           << ", \"axes\": [";
    for (std::size_t index = 0; index < motion.axes.size(); ++index) {
        const auto& axis = motion.axes[index];
        output << (index == 0 ? "\n" : ",\n")
               << "      {\"resource_id\": " << axis.resource_id
               << ", \"enabled\": " << (axis.enabled ? "true" : "false")
               << ", \"direction_positive\": "
               << (axis.direction_positive ? "true" : "false")
               << ", \"step_level\": " << (axis.step_level ? "true" : "false")
               << ", \"position_steps\": " << axis.position_steps
               << ", \"emitted_steps\": " << axis.emitted_steps << "}";
    }
    output << (motion.axes.empty() ? "]},\n" : "\n    ]},\n");

    const auto pwm = twin.waveform()
                         ? twin.waveform()->pwm_snapshot()
                         : std::vector<PwmSnapshot>{};
    output << "  \"pwm\": [";
    for (std::size_t index = 0; index < pwm.size(); ++index) {
        const auto& channel = pwm[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"channel\": " << static_cast<unsigned>(channel.channel)
               << ", \"frequency_hz\": " << channel.frequency_hz
               << ", \"duty\": " << channel.duty
               << ", \"active_low\": "
               << (channel.active_low ? "true" : "false")
               << ", \"running\": "
               << (channel.running ? "true" : "false") << "}";
    }
    output << (pwm.empty() ? "],\n" : "\n  ],\n");

    const auto streams = twin.waveform()
                             ? twin.waveform()->bitstream_snapshot()
                             : std::vector<TimedBitstreamSnapshot>{};
    const bool supported = std::any_of(
        twin.manifest().resources.begin(), twin.manifest().resources.end(),
        [](const auto& resource) {
            return resource.type ==
                   protocol::ResourceType::TimedBitstream;
        });
    output << "  \"ws2812\": {\"supported\": "
           << (supported ? "true" : "false") << ", \"strips\": [";
    bool first_strip = true;
    for (const auto& stream : streams) {
        if (!ws2812_timing(stream) || stream.bit_count % 24U != 0U ||
            stream.data.size() < stream.bit_count / 8U) {
            continue;
        }
        output << (first_strip ? "\n" : ",\n")
               << "    {\"channel\": "
               << static_cast<unsigned>(stream.timing.channel)
               << ", \"pixels\": [";
        const std::size_t pixels = stream.bit_count / 24U;
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            const std::uint8_t green = stream.data[pixel * 3U];
            const std::uint8_t red = stream.data[pixel * 3U + 1U];
            const std::uint8_t blue = stream.data[pixel * 3U + 2U];
            output << (pixel == 0 ? "" : ", ")
                   << json_string(rgb_string(red, green, blue));
        }
        output << "]}";
        first_strip = false;
    }
    output << (first_strip ? "]}\n}\n" : "\n  ]}\n}\n");
    output.close();
    if (!output) {
        throw std::runtime_error("写入 Mock 可视化状态文件失败");
    }
    if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
            throw std::runtime_error("替换 Mock 可视化状态文件失败");
        }
    }
}

}
