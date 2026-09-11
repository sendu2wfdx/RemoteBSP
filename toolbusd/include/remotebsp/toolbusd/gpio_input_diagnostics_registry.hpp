#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace remotebsp::toolbusd {

struct GpioInputDiagnosticTarget {
    std::uint32_t daemon_session_id{};
    std::array<std::uint8_t, 16> node_uuid{};
    std::uint64_t node_generation{};
    std::uint32_t node_id{};
    std::uint32_t resource_id{};
    std::uint32_t object_id{};
    std::uint16_t pin{};
};

class GpioInputDiagnosticsRegistry {
public:
    explicit GpioInputDiagnosticsRegistry(std::size_t capacity);
    bool remember(const GpioInputDiagnosticTarget& target);
    bool erase(std::uint32_t node_id, std::uint32_t object_id);
    void invalidate_node(std::uint32_t node_id);
    void retain_generation(std::uint32_t node_id,
                           const std::array<std::uint8_t, 16>& uuid,
                           std::uint64_t generation);
    std::vector<GpioInputDiagnosticTarget> snapshot() const;
    std::size_t size() const noexcept;

private:
    std::size_t capacity_{};
    std::vector<GpioInputDiagnosticTarget> entries_;
};

}
