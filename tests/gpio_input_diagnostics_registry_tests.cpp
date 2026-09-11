#include "remotebsp/toolbusd/gpio_input_diagnostics_registry.hpp"

#include <cassert>

int main() {
    using namespace remotebsp::toolbusd;
    GpioInputDiagnosticsRegistry registry(2U);
    GpioInputDiagnosticTarget a{7U, {1U}, 3U, 1U, 0x01000001U, 11U, 1U};
    GpioInputDiagnosticTarget b{7U, {2U}, 4U, 2U, 0x01000002U, 12U, 2U};
    GpioInputDiagnosticTarget c{7U, {3U}, 5U, 3U, 0x01000003U, 13U, 3U};
    assert(registry.remember(a));
    assert(registry.remember(b));
    assert(!registry.remember(c));
    a.pin = 9U;
    assert(registry.remember(a));
    assert(registry.size() == 2U);
    registry.retain_generation(1U, {1U}, 4U);
    assert(registry.size() == 1U);
    assert(registry.snapshot().front().node_id == 2U);
    assert(registry.erase(2U, 12U));
    assert(registry.size() == 0U);
    assert(registry.remember(a));
    registry.invalidate_node(1U);
    assert(registry.size() == 0U);
}
