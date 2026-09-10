#include "remotebsp/protocol/adc.hpp"

#include <limits>
#include <stdexcept>

namespace remotebsp::protocol {
namespace {
void u16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(v); out.push_back(v >> 8U); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (unsigned i=0;i<4;++i) out.push_back(v >> (8U*i)); }
std::uint16_t r16(const std::vector<std::uint8_t>& d, std::size_t p) { return d[p] | (std::uint16_t(d[p+1]) << 8U); }
std::uint32_t r32(const std::vector<std::uint8_t>& d, std::size_t p) { return r16(d,p) | (std::uint32_t(r16(d,p+2)) << 16U); }
void valid_request(const AdcSampleRequest& v) {
    if (!v.resource_id || !v.timeout_us || v.timeout_us > kMaximumAdcTimeoutUs ||
        !v.sample_count || v.sample_count > kMaximumAdcSamples)
        throw std::invalid_argument("ADC采样请求超出协议边界");
    if (v.sample_count > 1U && !v.interval_us)
        throw std::invalid_argument("ADC批量采样间隔不能为零");
    const auto duration = std::uint64_t(v.interval_us) * (v.sample_count - 1U);
    if (duration > v.timeout_us) throw std::invalid_argument("ADC批量采样时长超过超时");
}
}
std::vector<std::uint8_t> encode_adc_contract(const AdcContract& v) {
    if (v.version != kAdcProtocolVersion || !v.resource_id || v.resolution_bits < 6U || v.resolution_bits > 16U ||
        !v.maximum_sample_rate_hz || !v.reference_mv || !v.maximum_batch_samples || v.maximum_batch_samples > kMaximumAdcSamples)
        throw std::invalid_argument("ADC合同无效");
    std::vector<std::uint8_t> out; out.reserve(20); u16(out,v.version); u16(out,v.resolution_bits); u32(out,v.resource_id);
    u32(out,v.maximum_sample_rate_hz); u32(out,v.reference_mv); u16(out,v.maximum_batch_samples); u16(out,0); return out;
}
AdcContract decode_adc_contract(const std::vector<std::uint8_t>& d) {
    if (d.size()!=20 || r16(d,18)!=0) throw std::invalid_argument("ADC合同编码无效");
    AdcContract v{r16(d,0),r16(d,2),r32(d,4),r32(d,8),r32(d,12),r16(d,16)}; (void)encode_adc_contract(v); return v;
}
std::vector<std::uint8_t> encode_adc_sample_request(const AdcSampleRequest& v) {
    valid_request(v); std::vector<std::uint8_t> out; out.reserve(16); u32(out,v.resource_id); u32(out,v.timeout_us); u32(out,v.interval_us); u16(out,v.sample_count); u16(out,0); return out;
}
AdcSampleRequest decode_adc_sample_request(const std::vector<std::uint8_t>& d) {
    if (d.size()!=16 || r16(d,14)!=0) throw std::invalid_argument("ADC采样请求编码无效");
    AdcSampleRequest v{r32(d,0),r32(d,4),r32(d,8),r16(d,12)}; valid_request(v); return v;
}
std::vector<std::uint8_t> encode_adc_sample_result(const AdcSampleResult& v) {
    if (!v.resource_id || v.samples.empty() || v.samples.size()>kMaximumAdcSamples) throw std::invalid_argument("ADC采样结果无效");
    std::vector<std::uint8_t> out; out.reserve(16+v.samples.size()*2); u32(out,v.resource_id);u32(out,v.sequence);u32(out,v.elapsed_us);u16(out,static_cast<std::uint16_t>(v.samples.size()));u16(out,0); for(auto x:v.samples)u16(out,x); return out;
}
AdcSampleResult decode_adc_sample_result(const std::vector<std::uint8_t>& d) {
    if(d.size()<18 || r16(d,14)!=0 || r16(d,12)==0 || r16(d,12)>kMaximumAdcSamples || d.size()!=16U+2U*r16(d,12)) throw std::invalid_argument("ADC采样结果编码无效");
    AdcSampleResult v{r32(d,0),r32(d,4),r32(d,8),{}}; for(std::size_t p=16;p<d.size();p+=2)v.samples.push_back(r16(d,p)); if(!v.resource_id)throw std::invalid_argument("ADC采样结果资源无效"); return v;
}
}
