#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

enum class UartParity : std::uint8_t {
    None = 0,
    Odd = 1,
    Even = 2,
};

struct UartConfig {
    std::uint32_t baud_rate{};
    std::uint8_t data_bits{};
    std::uint8_t stop_bits{};
    UartParity parity{UartParity::None};
};

struct UartRuntimeStatus {
    bool failed{};
    std::uint32_t rx_buffered{};
    std::uint32_t tx_buffered{};
    std::uint32_t rx_overruns{};
    std::uint32_t tx_overruns{};
};

class UartBsp {
public:
    virtual ~UartBsp() = default;

    virtual void configure(std::uint8_t port,
                           const UartConfig& config) = 0;
    virtual std::vector<std::uint8_t> read(std::uint8_t port,
                                           std::size_t maximum_length) = 0;
    virtual void write(std::uint8_t port,
                       const std::vector<std::uint8_t>& data) = 0;
    virtual UartRuntimeStatus status(std::uint8_t port) const = 0;
    virtual void reset(std::uint8_t port) = 0;
    virtual void reset_all() = 0;
};

enum class UartError {
    PortNotConfigured,
    BufferOverflow,
    PortFailed,
};

class UartException : public std::runtime_error {
public:
    UartException(UartError code, const char* message);
    UartError code() const noexcept;

private:
    UartError code_;
};

using MockUartError = UartError;
using MockUartException = UartException;

class MockUartBsp final : public UartBsp {
public:
    explicit MockUartBsp(std::size_t rx_capacity = 4096,
                         std::size_t tx_capacity = 4096);

    void configure(std::uint8_t port,
                   const UartConfig& config) override;
    std::vector<std::uint8_t> read(std::uint8_t port,
                                   std::size_t maximum_length) override;
    void write(std::uint8_t port,
               const std::vector<std::uint8_t>& data) override;
    UartRuntimeStatus status(std::uint8_t port) const override;
    void reset(std::uint8_t port) override;
    void reset_all() override;

    void inject_rx(std::uint8_t port,
                   const std::vector<std::uint8_t>& data);
    std::vector<std::uint8_t> take_tx(std::uint8_t port);
    void set_failed(std::uint8_t port, bool failed);
    const UartConfig* config(std::uint8_t port) const noexcept;
    std::uint64_t configure_count() const noexcept;
    std::uint64_t read_count() const noexcept;
    std::uint64_t write_count() const noexcept;

private:
    struct PortState {
        UartConfig config;
        std::deque<std::uint8_t> rx;
        std::vector<std::uint8_t> tx;
        bool configured{};
        bool failed{};
        std::uint32_t rx_overruns{};
        std::uint32_t tx_overruns{};
    };

    PortState& require_port(std::uint8_t port);
    const PortState& require_port(std::uint8_t port) const;

    std::unordered_map<std::uint8_t, PortState> ports_;
    std::size_t rx_capacity_;
    std::size_t tx_capacity_;
    std::uint64_t configure_count_{};
    std::uint64_t read_count_{};
    std::uint64_t write_count_{};
};

}
