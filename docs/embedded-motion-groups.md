# STM32 跨板运动组参与者

STM32 公共 Remote Core 在启用 `CONFIG_REMOTEBSP_MOTION` 时编译运动组参与者，
处理 `MotionGroupPrepare`、`MotionGroupCommit` 和 `MotionGroupAbort`。关闭运动模块时，
参与者源码不参与构建，`rbsp_core_t` 也不包含其状态。

## 事务行为

- `PREPARE` 校验协议身份、启动代次、节点开始时刻、序列、资源 ID、步频和队列
  约束，只保存一个冻结段，不修改运动队列，也不产生 STEP。
- `COMMIT` 必须由相同会话携带完整相同的冻结身份。只有这一步才把冻结段提交给
  `rbsp_motion_queue_t`。重复 `PREPARE` 和 `COMMIT` 不会重复入队。
- 不同身份、不同内容或不同会话的请求会被拒绝。预备或已武装期间，普通
  `MotionEnqueue` 返回资源忙，避免插入段破坏冻结结果。
- 组段自然完成后会保留 `group_id + plan_generation` 完成水位：普通入队恢复，
  但同组旧代次不能因请求缓存淘汰而复活；更高代次可以继续。
- `ABORT` 在已武装时调用现有运动队列安全停机。普通 `MotionAbort` 也会先使预备
  事务失效并锁存 `Aborted` 故障。首次调度或 compare 安排失败时，刚提交的段会
  立即清空并进入安全输出状态。

## 启动代次和当前硬件边界

`rbsp_hal_t::motion_boot_epoch` 必须返回本次 MCU 启动唯一的非零 64 位代次；返回
零或不配置表示板卡没有可靠启动代次。此时普通单板运动仍可用，但 `TimeSync` 和
运动组命令返回 `UnsupportedCapability`，不能进入跨板同步。

不能用 UUID、从零启动的定时器、未初始化 RAM 或启动时序抖动伪造启动代次。
当前 F072、F103、G431 板级代码尚未提供经过掉电与快速复位验证的可靠来源，因此
实体固件默认保持上述安全降级。后续需要选定耐久的持久化启动计数器或可靠硬件
随机源，并在板卡上验证唯一性后才能开放跨板同步。

公共嵌入式 Core 当前尚无与主机 Mock 等价的 STEPGEN 租约管理器。参与者已经绑定
请求 `session_id`，但租约获取、续期、释放和过期联动仍是实体固件接入前的前置项；
在该项完成前不能把会话绑定描述为完整的资源租约保证。
