#pragma once

#include <mutex>

namespace remotebsp::toolbusd {

// 串行覆盖“运动组状态转换 + 对应动作批次发送”。调用者进入门后才可
// 获取 daemon 状态锁，发送前必须释放状态锁但继续持有本门，避免并发取消
// 先发 ABORT、旧线程随后再发 PREPARE/COMMIT。
class MotionGroupDispatchGate {
public:
    using Guard = std::unique_lock<std::mutex>;

    Guard lock() { return Guard(mutex_); }

private:
    std::mutex mutex_;
};

}
