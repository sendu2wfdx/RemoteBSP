#pragma once
#include "remotebsp/protocol/timer.hpp"
namespace remotebsp::mock_mcu {
class TimerBsp { public: virtual ~TimerBsp()=default; virtual protocol::TimerExecuteResult execute(const protocol::TimerExecuteRequest&)=0; };
class DeterministicTimerBsp final : public TimerBsp { public: protocol::TimerExecuteResult execute(const protocol::TimerExecuteRequest&) override; };
}
