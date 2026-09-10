# Timer 通用资源

Timer 第一阶段把板卡专用固件静态声明的定时器端点暴露为三个有界、原子的操作：
固定窗口边沿计数、相邻边沿周期捕获和单次定时等待。协议版本为 1；合同公布可用操作、
计数时钟和最长操作时间。它不生成 PWM 或任意波形，不承担 STEP/DIR 运动调度，也不允许
运行时更改引脚、AF、分频器或定时器复用。

`TimerExecute` 请求包含资源 ID、操作、参数时间和总超时，时间均以微秒表示且必须位于
1～1,000,000；参数不得超过超时。结果包含单调序号、无符号 64 位测量值和实际耗时。
计数返回窗口内边沿数，周期捕获返回硬件 tick 数，单次等待返回实际等待 tick 数。

执行强制要求会话绑定的独占通用资源租约；无租约、共享模式、其他会话、已释放或已超时的
租约都会在触碰 BSP 前拒绝。每个请求只调用一次 BSP，编码失败、超时结果或 BSP 异常均作为该资源操作失败；
序号只在完整成功后推进，一个 Timer 端点的故障不改变其他端点。

开发环境可用：

```text
remote-cli --node 1 timer-contract 0x07000001
remote-cli --node 1 timer-execute 0x07000001 counter 1000 2000
remote-cli --node 1 timer-execute 0x07000001 capture 100000 100000
remote-cli --node 1 timer-execute 0x07000001 one-shot 500 500
```

第二阶段已把 Timer 纳入统一 `ResourceEnum`、`ResourceDescribe`、`ResourceStatus` 和
能力位。默认 Mock 板公布两路 Timer，并由数字孪生创建确定性 BSP；可视状态同时给出
`timer.supported` 和资源 ID，便于主机侧发现，而不是依赖约定编号。

STM32 公共 Remote Core 提供默认关闭的 `CONFIG_REMOTEBSP_TIMER`、静态端点表和单次执行
回调边界。只有资源表和回调均存在、ID/能力/时钟/最长操作时间全部合法时，固件才公布
Timer 能力与资源；单纯打开编译选项不会制造虚假端点。F072、F103、G431 的公共骨架纳入
交叉编译门禁，但均未配置板级端点。因此这些结果只证明软件纵切和三类 MCU 的编译兼容，
不代表 STM32 实体 Timer 已完成输入捕获。实体后端仍须验证 AF、溢出、时钟精度和输入
电气链路，Studio 在确认前不开放候选。
