#include "remotebsp/mock_mcu/gpio_bsp.hpp"

namespace remotebsp::mock_mcu {

MockGpioException::MockGpioException(MockGpioError code, const char* message)
    : std::runtime_error(message), code_(code) {}

MockGpioError MockGpioException::code() const noexcept { return code_; }

void MockGpioBsp::configure(std::uint16_t pin, GpioDirection direction,
                            bool initial_value) {
    pins_[pin] = {direction, initial_value};
    ++configure_count_;
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
        throw MockGpioException(MockGpioError::PinNotConfigured,
                                "GPIO 引脚尚未配置");
    }
    found->second.value = value;
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

}
