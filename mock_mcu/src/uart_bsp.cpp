#include "remotebsp/mock_mcu/uart_bsp.hpp"

#include <algorithm>
#include <limits>

namespace remotebsp::mock_mcu {

UartException::UartException(UartError code, const char* message)
    : std::runtime_error(message), code_(code) {}

UartError UartException::code() const noexcept { return code_; }

MockUartBsp::MockUartBsp(std::size_t rx_capacity,
                         std::size_t tx_capacity)
    : rx_capacity_(rx_capacity), tx_capacity_(tx_capacity) {
    if (rx_capacity_ == 0 || tx_capacity_ == 0 ||
        rx_capacity_ > std::numeric_limits<std::uint32_t>::max() ||
        tx_capacity_ > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("UART 缓冲容量必须位于 1～UINT32_MAX");
    }
}

void MockUartBsp::configure(std::uint8_t port,
                            const UartConfig& config) {
    PortState& state = ports_[port];
    if (state.failed) {
        throw MockUartException(MockUartError::PortFailed,
                                "UART 端口故障");
    }
    state.config = config;
    state.rx.clear();
    state.tx.clear();
    state.configured = true;
    state.rx_overruns = 0;
    state.tx_overruns = 0;
    ++configure_count_;
}

std::vector<std::uint8_t> MockUartBsp::read(
    std::uint8_t port, std::size_t maximum_length) {
    PortState& state = require_port(port);
    if (state.failed) {
        throw MockUartException(MockUartError::PortFailed,
                                "UART 端口故障");
    }
    const std::size_t length =
        std::min(maximum_length, state.rx.size());
    std::vector<std::uint8_t> data;
    data.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        data.push_back(state.rx.front());
        state.rx.pop_front();
    }
    ++read_count_;
    return data;
}

void MockUartBsp::write(std::uint8_t port,
                        const std::vector<std::uint8_t>& data) {
    PortState& state = require_port(port);
    if (state.failed) {
        throw MockUartException(MockUartError::PortFailed,
                                "UART 端口故障");
    }
    if (data.size() > tx_capacity_ - state.tx.size()) {
        const auto overflow = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                data.size() - (tx_capacity_ - state.tx.size()),
                std::numeric_limits<std::uint32_t>::max()));
        state.tx_overruns =
            std::numeric_limits<std::uint32_t>::max() -
                        state.tx_overruns <
                    overflow
                ? std::numeric_limits<std::uint32_t>::max()
                : state.tx_overruns + overflow;
        throw MockUartException(MockUartError::BufferOverflow,
                                "UART 发送缓冲区空间不足");
    }
    state.tx.insert(state.tx.end(), data.begin(), data.end());
    ++write_count_;
}

void MockUartBsp::inject_rx(std::uint8_t port,
                            const std::vector<std::uint8_t>& data) {
    PortState& state = require_port(port);
    if (state.failed) {
        throw MockUartException(MockUartError::PortFailed,
                                "UART 端口故障");
    }
    const auto available = rx_capacity_ - state.rx.size();
    const auto accepted = std::min(available, data.size());
    state.rx.insert(state.rx.end(), data.begin(),
                    data.begin() + static_cast<std::ptrdiff_t>(accepted));
    const auto dropped = data.size() - accepted;
    const auto overflow = static_cast<std::uint32_t>(
        std::min<std::size_t>(dropped,
                              std::numeric_limits<std::uint32_t>::max()));
    state.rx_overruns =
        std::numeric_limits<std::uint32_t>::max() - state.rx_overruns <
                overflow
            ? std::numeric_limits<std::uint32_t>::max()
            : state.rx_overruns + overflow;
}

std::vector<std::uint8_t> MockUartBsp::take_tx(std::uint8_t port) {
    PortState& state = require_port(port);
    std::vector<std::uint8_t> data = std::move(state.tx);
    state.tx.clear();
    return data;
}

UartRuntimeStatus MockUartBsp::status(std::uint8_t port) const {
    const auto found = ports_.find(port);
    if (found == ports_.end()) {
        return {};
    }
    const auto& state = found->second;
    return {state.failed,
            static_cast<std::uint32_t>(state.rx.size()),
            static_cast<std::uint32_t>(state.tx.size()),
            state.rx_overruns, state.tx_overruns};
}

void MockUartBsp::set_failed(std::uint8_t port, bool failed) {
    ports_[port].failed = failed;
}

void MockUartBsp::reset(std::uint8_t port) {
    PortState& state = require_port(port);
    state.rx.clear();
    state.tx.clear();
    state.rx_overruns = 0;
    state.tx_overruns = 0;
    state.failed = false;
}

void MockUartBsp::reset_all() {
    ports_.clear();
}

const UartConfig* MockUartBsp::config(std::uint8_t port) const noexcept {
    const auto found = ports_.find(port);
    return found == ports_.end() ? nullptr : &found->second.config;
}

std::uint64_t MockUartBsp::configure_count() const noexcept {
    return configure_count_;
}

std::uint64_t MockUartBsp::read_count() const noexcept {
    return read_count_;
}

std::uint64_t MockUartBsp::write_count() const noexcept {
    return write_count_;
}

MockUartBsp::PortState& MockUartBsp::require_port(std::uint8_t port) {
    const auto found = ports_.find(port);
    if (found == ports_.end() || !found->second.configured) {
        throw MockUartException(MockUartError::PortNotConfigured,
                                "UART 端口尚未配置");
    }
    return found->second;
}

const MockUartBsp::PortState& MockUartBsp::require_port(
    std::uint8_t port) const {
    const auto found = ports_.find(port);
    if (found == ports_.end() || !found->second.configured) {
        throw MockUartException(MockUartError::PortNotConfigured,
                                "UART 端口尚未配置");
    }
    return found->second;
}

}
