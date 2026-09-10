#pragma once
#include "remotebsp/protocol/adc.hpp"
#include <vector>
namespace remotebsp::mock_mcu {
class AdcBsp { public: virtual ~AdcBsp() = default; virtual std::vector<std::uint16_t> sample(const protocol::AdcSampleRequest&) = 0; };
class DeterministicAdcBsp final : public AdcBsp { public: std::vector<std::uint16_t> sample(const protocol::AdcSampleRequest&) override; };
}
