#include "remotebsp/mock_mcu/timer_bsp.hpp"
namespace remotebsp::mock_mcu {
protocol::TimerExecuteResult DeterministicTimerBsp::execute(const protocol::TimerExecuteRequest&r){std::uint64_t value{};switch(r.operation){case protocol::TimerOperation::CounterWindow:value=r.parameter_us/100U;break;case protocol::TimerOperation::PeriodCapture:value=1000U;break;case protocol::TimerOperation::OneShot:value=r.parameter_us;break;}return {r.resource_id,r.operation,1U,value,r.parameter_us};}
}
