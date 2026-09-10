# Timer 通用资源

Timer 第一阶段把板卡专用固件静态声明的定时器端点暴露为三个有界、原子的操作：
固定窗口边沿计数、相邻边沿周期捕获和单次定时等待。协议版本为 1；合同公布可用操作、
计数时钟和最长操作时间。它不生成 PWM 或任意波形，不承担 STEP/DIR 运动调度，也不允许
运行时更改引脚、AF、分频器或定时器复用。

`TimerExecute` 请求包含资源 ID、操作、参数时间和总超时，时间均以微秒表示且必须位于
1～1,000,000；参数不得超过超时。结果包含单调序号、无符号 64 位测量值和实际耗时。
计数返回窗口内边沿数，周期捕获返回硬件 tick 数，单次等待返回实际等待 tick 数。

执行服从通用资源租约：无租约时允许访问；一旦资源被其他会话独占，请求失败而不会触碰
BSP。每个请求只调用一次 BSP，编码失败、超时结果或 BSP 异常均作为该资源操作失败；
序号只在完整成功后推进，一个 Timer 端点的故障不改变其他端点。

开发环境可用：

```text
remote-cli --node 1 timer-contract 0x07000001
remote-cli --node 1 timer-execute 0x07000001 counter 1000 2000
remote-cli --node 1 timer-execute 0x07000001 capture 100000 100000
remote-cli --node 1 timer-execute 0x07000001 one-shot 500 500
```

当前实现是协议、Mock Remote Core/BSP、`libremotebsp` 与 CLI 的软件纵切，不代表 STM32
实体 Timer 已完成输入捕获。实体实现仍须由 Kconfig 固化合法引脚和定时器通道，并在 BSP
层验证溢出、时钟精度和输入电气链路。
