#pragma once

#include "remotebsp/mock_mcu/motion_executor.hpp"
#include "remotebsp/mock_mcu/time_sync_bsp.hpp"
#include "remotebsp/protocol/motion_group.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace remotebsp::mock_mcu {

enum class MockMotionGroupState : std::uint8_t {
    Idle = 0,
    Prepared,
    Armed,
    Aborted,
};

// 纯软件节点侧参与者。传入 MotionExecutor 与 MockTimeSyncBsp 后，PREPARE 在
// 执行器副本上校验，COMMIT 才修改真实队列；默认空依赖仍可用于纯协议测试。
class MockMotionGroupParticipant {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit MockMotionGroupParticipant(
        std::shared_ptr<MotionExecutor> motion = nullptr,
        std::shared_ptr<MockTimeSyncBsp> clock = nullptr);

    protocol::MotionGroupReadyPayload prepare(
        const protocol::MotionGroupPreparePayload& prepare,
        protocol::MotionGroupReadyCode forced_result =
            protocol::MotionGroupReadyCode::Ready,
        std::uint32_t session_id = 0U,
        TimePoint now = Clock::now());
    protocol::MotionGroupCommitAckPayload commit(
        const protocol::MotionGroupCommitPayload& commit,
        protocol::MotionGroupCommitCode forced_result =
            protocol::MotionGroupCommitCode::Armed,
        std::uint32_t session_id = 0U,
        TimePoint now = Clock::now());
    bool abort(const protocol::MotionGroupAbortPayload& abort,
               std::uint32_t session_id = 0U,
               TimePoint now = Clock::now()) noexcept;
    bool emergency_abort() noexcept;
    bool cancel_session(std::uint32_t session_id) noexcept;
    bool uses_resource(std::uint32_t resource_id) const noexcept;
    bool owned_by(std::uint32_t session_id) const noexcept;
    void reset() noexcept;

    MockMotionGroupState state() const noexcept;
    const std::optional<protocol::MotionGroupPreparePayload>& prepared() const
        noexcept;

private:
    static bool same_identity(
        const protocol::MotionGroupIdentityPayload& left,
        const protocol::MotionGroupIdentityPayload& right) noexcept;
    std::optional<MotionSegment> make_local_segment(
        const protocol::MotionGroupPreparePayload& prepare,
        TimePoint now) const;

    std::shared_ptr<MotionExecutor> motion_;
    std::shared_ptr<MockTimeSyncBsp> clock_;
    MockMotionGroupState state_{MockMotionGroupState::Idle};
    std::optional<protocol::MotionGroupPreparePayload> prepared_;
    std::optional<MotionSegment> prepared_motion_segment_;
    std::uint32_t owner_session_id_{};
};

}
