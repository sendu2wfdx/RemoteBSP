#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace remotebsp::mock_mcu {

enum class GpioDirection : std::uint8_t {
    Input = 0,
    Output = 1,
};

class GpioBsp {
public:
    virtual ~GpioBsp() = default;

    virtual void configure(std::uint16_t pin, GpioDirection direction,
                           bool initial_value) = 0;
    virtual void reset_all() = 0;
    virtual bool read(std::uint16_t pin) const = 0;
    virtual void write(std::uint16_t pin, bool value) = 0;
};

enum class MockGpioError {
    PinNotConfigured,
    WriteToInput,
    InjectToOutput,
    WriteFailed,
};

struct GpioPinSnapshot {
    std::uint16_t pin{};
    GpioDirection direction{GpioDirection::Input};
    bool value{};
};

struct GpioPendingInputSnapshot {
    std::uint16_t pin{};
    bool value{};
};

class MockGpioException : public std::runtime_error {
public:
    MockGpioException(MockGpioError code, const char* message);
    MockGpioError code() const noexcept;

private:
    MockGpioError code_;
};

class MockGpioBsp final : public GpioBsp {
public:
    void configure(std::uint16_t pin, GpioDirection direction,
                   bool initial_value) override;
    void reset_all() override;
    bool read(std::uint16_t pin) const override;
    void write(std::uint16_t pin, bool value) override;

    void set_input_value(std::uint16_t pin, bool value);
    void fail_next_write() noexcept;
    std::uint64_t configure_count() const noexcept;
    std::uint64_t read_count() const noexcept;
    std::uint64_t write_count() const noexcept;
    std::vector<GpioPinSnapshot> snapshot() const;
    std::vector<GpioPendingInputSnapshot> pending_input_snapshot() const;

private:
    struct PinState {
        GpioDirection direction{GpioDirection::Input};
        bool value{};
    };

    std::unordered_map<std::uint16_t, PinState> pins_;
    std::unordered_map<std::uint16_t, bool> pending_input_values_;
    std::uint64_t configure_count_{};
    mutable std::uint64_t read_count_{};
    std::uint64_t write_count_{};
    bool fail_next_write_{};
};

}
