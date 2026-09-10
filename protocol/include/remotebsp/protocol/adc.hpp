#pragma once

#include <cstdint>
#include <vector>

namespace remotebsp::protocol {

constexpr std::uint16_t kAdcProtocolVersion = 1;
constexpr std::uint16_t kMaximumAdcSamples = 32;
constexpr std::uint32_t kMaximumAdcTimeoutUs = 1000000;

struct AdcContract {
    std::uint16_t version{kAdcProtocolVersion};
    std::uint16_t resolution_bits{};
    std::uint32_t resource_id{};
    std::uint32_t maximum_sample_rate_hz{};
    std::uint32_t reference_mv{};
    std::uint16_t maximum_batch_samples{kMaximumAdcSamples};
};

struct AdcSampleRequest {
    std::uint32_t resource_id{};
    std::uint32_t timeout_us{};
    std::uint32_t interval_us{};
    std::uint16_t sample_count{};
};

struct AdcSampleResult {
    std::uint32_t resource_id{};
    std::uint32_t sequence{};
    std::uint32_t elapsed_us{};
    std::vector<std::uint16_t> samples;
};

std::vector<std::uint8_t> encode_adc_contract(const AdcContract& value);
AdcContract decode_adc_contract(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> encode_adc_sample_request(const AdcSampleRequest& value);
AdcSampleRequest decode_adc_sample_request(const std::vector<std::uint8_t>& data);
std::vector<std::uint8_t> encode_adc_sample_result(const AdcSampleResult& value);
AdcSampleResult decode_adc_sample_result(const std::vector<std::uint8_t>& data);

}
