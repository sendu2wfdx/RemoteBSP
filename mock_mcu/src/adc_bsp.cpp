#include "remotebsp/mock_mcu/adc_bsp.hpp"
namespace remotebsp::mock_mcu {
std::vector<std::uint16_t> DeterministicAdcBsp::sample(const protocol::AdcSampleRequest& request) {
    std::vector<std::uint16_t> values; values.reserve(request.sample_count);
    for (std::uint16_t i=0;i<request.sample_count;++i) values.push_back(static_cast<std::uint16_t>((request.resource_id*257U+i*17U)&0x0FFFU));
    return values;
}
}
