#include "remotebsp/mock_mcu/gpio_bsp.hpp"

#include <algorithm>

namespace remotebsp::mock_mcu {

MockGpioException::MockGpioException(MockGpioError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MockGpioError MockGpioException::code() const noexcept { return code_; }

void MockGpioBsp::configure(std::uint16_t pin, GpioDirection direction,
                            bool initial_value) {
    if (direction == GpioDirection::Input) {
        const auto pending = pending_input_values_.find(pin);
        if (pending != pending_input_values_.end()) {
            initial_value = pending->second;
        }
    }
    pins_[pin] = {direction, initial_value};
    ++configure_count_;
}

void MockGpioBsp::reset_all() {
    pins_.clear();
}

bool MockGpioBsp::read(std::uint16_t pin) const {
    const auto found = pins_.find(pin);
    if (found == pins_.end()) {
        throw MockGpioException(MockGpioError::PinNotConfigured,
                                "GPIO 引脚尚未配置");
    }
    ++read_count_;
    return found->second.value;
}

void MockGpioBsp::write(std::uint16_t pin, bool value) {
    const auto found = pins_.find(pin);
    if (found == pins_.end()) {
        throw MockGpioException(MockGpioError::PinNotConfigured,
                                "GPIO 引脚尚未配置");
    }
    if (found->second.direction != GpioDirection::Output) {
        throw MockGpioException(MockGpioError::WriteToInput,
                                "不能写入 GPIO 输入引脚");
    }
    found->second.value = value;
    ++write_count_;
}

void MockGpioBsp::set_input_value(std::uint16_t pin, bool value) {
    const auto found = pins_.find(pin);
    if (found == pins_.end()) {
        pending_input_values_[pin] = value;
        return;
    }
    if (found->second.direction != GpioDirection::Input) {
        throw MockGpioException(MockGpioError::InjectToOutput,
                                "不能向 GPIO 输出注入输入电平");
    }
    found->second.value = value;
    pending_input_values_[pin] = value;
}

std::uint64_t MockGpioBsp::configure_count() const noexcept {
    return configure_count_;
}

std::uint64_t MockGpioBsp::read_count() const noexcept {
    return read_count_;
}

std::uint64_t MockGpioBsp::write_count() const noexcept {
    return write_count_;
}

std::vector<GpioPinSnapshot> MockGpioBsp::snapshot() const {
    std::vector<GpioPinSnapshot> result;
    result.reserve(pins_.size());
    for (const auto& [pin, state] : pins_) {
        result.push_back({pin, state.direction, state.value});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.pin < right.pin;
    });
    return result;
}

}
