#include "remotebsp/mock_mcu/remote_core.hpp"
#include "remotebsp/protocol/resource.hpp"
#include "remotebsp/protocol/timer.hpp"
#include <cassert>
using namespace remotebsp;
namespace {
class Selective final:public mock_mcu::TimerBsp{public:protocol::TimerExecuteResult execute(const protocol::TimerExecuteRequest&r)override{if(r.resource_id==0x07000001U)throw std::runtime_error("故障");return{r.resource_id,r.operation,1,42,r.parameter_us};}};
protocol::Packet req(protocol::Command c,std::vector<std::uint8_t> p,std::uint32_t session=7){protocol::Packet v;v.header.message_type=protocol::MessageType::Request;v.header.command=static_cast<std::uint16_t>(c);v.header.session_id=session;v.payload=std::move(p);return v;}
std::vector<std::uint8_t> body(const protocol::Packet&r){assert(r.payload.front()==0);return{r.payload.begin()+1,r.payload.end()};}
}
int main(){constexpr std::uint32_t id=0x07000001U,peer=id+1;
 auto contract=protocol::TimerContract{1,std::uint16_t(protocol::kTimerCapabilityCounterWindow|protocol::kTimerCapabilityPeriodCapture|protocol::kTimerCapabilityOneShot),id,1000000,1000000};assert(protocol::decode_timer_contract(protocol::encode_timer_contract(contract)).tick_hz==1000000);
 auto value=protocol::TimerExecuteRequest{id,protocol::TimerOperation::CounterWindow,1000,2000};assert(protocol::decode_timer_execute_request(protocol::encode_timer_execute_request(value)).parameter_us==1000);
 bool rejected=false;try{(void)protocol::encode_timer_execute_request({id,protocol::TimerOperation::OneShot,2000,1000});}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 mock_mcu::RemoteCore core({},mock_mcu::capability_mask(mock_mcu::Capability::Timer),nullptr,nullptr,{{id,protocol::ResourceType::Timer,1,protocol::kResourceFlagNative,0,0}},{{id,1,std::uint16_t(protocol::kResourceAccessReadable|protocol::kResourceAccessWritable|protocol::kResourceAccessExclusiveWrite|protocol::kResourceAccessLeaseSupported),1000,1000000,1000,0,0,0}});
 assert(protocol::decode_timer_contract(body(core.handle(req(protocol::Command::TimerContract,protocol::encode_resource_id(id))))).resource_id==id);
 auto first=protocol::decode_timer_execute_result(body(core.handle(req(protocol::Command::TimerExecute,protocol::encode_timer_execute_request(value)))));assert(first.sequence==1&&first.value==10&&first.elapsed_us==1000);
 auto second=protocol::decode_timer_execute_result(body(core.handle(req(protocol::Command::TimerExecute,protocol::encode_timer_execute_request({id,protocol::TimerOperation::OneShot,50,50})))));assert(second.sequence==2&&second.value==50);
 // 无租约时可使用；存在其他会话独占租约后必须拒绝。
 auto lease=protocol::encode_resource_lease_request({id,1000,protocol::ResourceLeaseMode::Exclusive});auto acquired=core.handle(req(protocol::Command::ResourceAcquire,lease,99));assert(acquired.payload.front()==0);
 auto denied=core.handle(req(protocol::Command::TimerExecute,protocol::encode_timer_execute_request(value),7));assert(denied.payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::AccessDenied));
 // 一个端点故障不污染另一个 Timer 端点。
 mock_mcu::RemoteCore isolated({},mock_mcu::capability_mask(mock_mcu::Capability::Timer),nullptr,nullptr,{{id,protocol::ResourceType::Timer,1,protocol::kResourceFlagNative,0,0},{peer,protocol::ResourceType::Timer,2,protocol::kResourceFlagNative,0,0}});isolated.set_timer_bsp(std::make_shared<Selective>());
 assert(isolated.handle(req(protocol::Command::TimerExecute,protocol::encode_timer_execute_request(value))).payload.front()==static_cast<std::uint8_t>(mock_mcu::StatusCode::ResourceFailed));
 auto healthy=protocol::decode_timer_execute_result(body(isolated.handle(req(protocol::Command::TimerExecute,protocol::encode_timer_execute_request({peer,protocol::TimerOperation::PeriodCapture,100,100})))));assert(healthy.value==42&&healthy.sequence==1);
}
