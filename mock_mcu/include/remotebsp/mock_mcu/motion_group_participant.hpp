#pragma once

#include "remotebsp/protocol/motion_group.hpp"

#include <optional>

namespace remotebsp::mock_mcu {

enum class MockMotionGroupState : std::uint8_t {
    Idle = 0,
    Prepared,
    Armed,
    Aborted,
};

// 纯软件节点侧替身，只验证事务身份、幂等性和状态跃迁；尚未接入 RemoteCore
// 或实体运动执行器，因此不会产生 GPIO 波形。
class MockMotionGroupParticipant {
public:
    protocol::MotionGroupReadyPayload prepare(
        const protocol::MotionGroupPreparePayload& prepare,
        protocol::MotionGroupReadyCode forced_result =
            protocol::MotionGroupReadyCode::Ready);
    protocol::MotionGroupCommitAckPayload commit(
        const protocol::MotionGroupCommitPayload& commit,
        protocol::MotionGroupCommitCode forced_result =
            protocol::MotionGroupCommitCode::Armed);
    bool abort(const protocol::MotionGroupAbortPayload& abort) noexcept;
    void reset() noexcept;

    MockMotionGroupState state() const noexcept;
    const std::optional<protocol::MotionGroupPreparePayload>& prepared() const
        noexcept;

private:
    static bool same_identity(
        const protocol::MotionGroupIdentityPayload& left,
        const protocol::MotionGroupIdentityPayload& right) noexcept;

    MockMotionGroupState state_{MockMotionGroupState::Idle};
    std::optional<protocol::MotionGroupPreparePayload> prepared_;
};

}
