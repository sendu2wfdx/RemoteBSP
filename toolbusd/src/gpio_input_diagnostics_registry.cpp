#include "remotebsp/toolbusd/gpio_input_diagnostics_registry.hpp"

#include <algorithm>

namespace remotebsp::toolbusd {

GpioInputDiagnosticsRegistry::GpioInputDiagnosticsRegistry(
    std::size_t capacity) : capacity_(capacity) {}

bool GpioInputDiagnosticsRegistry::remember(
    const GpioInputDiagnosticTarget& target) {
    if (capacity_ == 0U || target.node_id == 0U ||
        target.object_id == 0U || target.resource_id == 0U) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const auto& item) { return item.node_id == target.node_id &&
                                      item.object_id == target.object_id; });
    if (found != entries_.end()) {
        *found = target;
        return true;
    }
    if (entries_.size() >= capacity_) return false;
    entries_.push_back(target);
    return true;
}

bool GpioInputDiagnosticsRegistry::erase(std::uint32_t node_id,
                                         std::uint32_t object_id) {
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const auto& item) { return item.node_id == node_id &&
                                      item.object_id == object_id; }),
        entries_.end());
    return entries_.size() != before;
}

void GpioInputDiagnosticsRegistry::invalidate_node(std::uint32_t node_id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const auto& item) { return item.node_id == node_id; }), entries_.end());
}

void GpioInputDiagnosticsRegistry::retain_generation(
    std::uint32_t node_id, const std::array<std::uint8_t, 16>& uuid,
    std::uint64_t generation) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const auto& item) { return item.node_id == node_id &&
            (item.node_uuid != uuid || item.node_generation != generation); }),
        entries_.end());
}

std::vector<GpioInputDiagnosticTarget>
GpioInputDiagnosticsRegistry::snapshot() const { return entries_; }
std::size_t GpioInputDiagnosticsRegistry::size() const noexcept {
    return entries_.size();
}

}
