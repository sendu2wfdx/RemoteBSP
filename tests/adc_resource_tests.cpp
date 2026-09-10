#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/adc.hpp"
#include "remotebsp/protocol/resource.hpp"
#include <cassert>

using namespace remotebsp;
namespace {
class SelectiveAdcBsp final : public mock_mcu::AdcBsp {
public:
    std::vector<std::uint16_t> sample(const protocol::AdcSampleRequest& request) override {
        if (request.resource_id == 0x05000001U) throw std::runtime_error("模拟端点故障");
        return std::vector<std::uint16_t>(request.sample_count, 123U);
    }
};
protocol::Packet request(protocol::Command command, std::vector<std::uint8_t> payload) {
    protocol::Packet value; value.header.message_type=protocol::MessageType::Request;
    value.header.command=static_cast<std::uint16_t>(command); value.header.session_id=7; value.payload=std::move(payload); return value;
}
std::vector<std::uint8_t> body(const protocol::Packet& response) {
    assert(response.payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::Ok));
    return {response.payload.begin()+1,response.payload.end()};
}
}
int main() {
    constexpr std::uint32_t id=0x05000001U;
    protocol::AdcContract contract{1,12,id,10000,3300,32};
    assert(protocol::decode_adc_contract(protocol::encode_adc_contract(contract)).resource_id==id);
    protocol::AdcSampleRequest request_value{id,1000,100,4};
    assert(protocol::decode_adc_sample_request(protocol::encode_adc_sample_request(request_value)).sample_count==4);
    bool rejected=false; try { (void)protocol::encode_adc_sample_request({id,99,100,2}); } catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);

    std::vector<protocol::ResourceDescriptor> resources{{id,protocol::ResourceType::Adc,1,protocol::kResourceFlagNative,0,0}};
    mock_mcu::RemoteCore core({},mock_mcu::capability_mask(mock_mcu::Capability::Adc),nullptr,nullptr,resources);
    const auto got_contract=protocol::decode_adc_contract(body(core.handle(request(protocol::Command::AdcContract,protocol::encode_resource_id(id)))));
    assert(got_contract.maximum_batch_samples==32);
    const auto first=protocol::decode_adc_sample_result(body(core.handle(request(protocol::Command::AdcSample,protocol::encode_adc_sample_request(request_value)))));
    assert(first.sequence==1 && first.elapsed_us==300 && first.samples.size()==4);
    assert(first.samples[1]==static_cast<std::uint16_t>((id*257U+17U)&0x0FFFU));
    const auto second=protocol::decode_adc_sample_result(body(core.handle(request(protocol::Command::AdcSample,protocol::encode_adc_sample_request({id,100,0,1})))));
    assert(second.sequence==2 && second.samples.size()==1);
    const auto bad=core.handle(request(protocol::Command::AdcSample,protocol::encode_adc_sample_request({id,1000,50,2})));
    assert(bad.payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::InvalidPayload));
    const auto missing=core.handle(request(protocol::Command::AdcContract,protocol::encode_resource_id(id+1)));
    assert(missing.payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::ObjectNotFound));

    // 一个 ADC 端点的 BSP 故障不得污染同一节点的其他静态 ADC 资源。
    constexpr std::uint32_t peer=id+1U;
    mock_mcu::RemoteCore isolated({},mock_mcu::capability_mask(mock_mcu::Capability::Adc),nullptr,nullptr,
        {{id,protocol::ResourceType::Adc,1,protocol::kResourceFlagNative,0,0},
         {peer,protocol::ResourceType::Adc,2,protocol::kResourceFlagNative,0,0}});
    isolated.set_adc_bsp(std::make_shared<SelectiveAdcBsp>());
    const auto failed=isolated.handle(request(protocol::Command::AdcSample,
        protocol::encode_adc_sample_request({id,100,0,1})));
    assert(failed.payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::ResourceFailed));
    const auto healthy=protocol::decode_adc_sample_result(body(isolated.handle(request(protocol::Command::AdcSample,
        protocol::encode_adc_sample_request({peer,100,0,1})))));
    assert(healthy.samples==std::vector<std::uint16_t>({123U}));
}
