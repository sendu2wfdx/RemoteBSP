# 单板 I2C/SPI 故障隔离

## 审计结论

本次只审计单块 MCU 工具板内的固件与协议闭环，不包含跨板同步或多节点验证。
UART、PWM 和 TimedBitstream 已有后端故障锁存、`RESOURCE_STATUS` 可观测状态和
`RESOURCE_RESET` 恢复路径；I2C/SPI 在 HAL 返回 `FAULT` 或违反收发长度合同时，
此前只返回当次事务错误，后续状态与 MCU 健康快照无法看到该故障。

## 已实现闭环

- Remote Core 为每个 I2C/SPI 静态资源保存独立故障锁存，不按控制器合并。
- HAL 返回 `FAULT`、非法状态值、越界长度或“成功但收发不完整”时，仅把目标资源
  标记为 `Failed`，并在 `RESOURCE_STATUS` 中设置 `BACKEND_FAILURE`。
- MCU 健康快照的故障资源数量包含已锁存的 I2C/SPI 资源。
- 新增可选 `bus_reset` HAL 回调。`RESOURCE_RESET` 只复位指定资源；存在租约时，
  非所有者被拒绝。复位失败保持故障锁存，成功后清除目标锁存并释放目标租约。
- 同一控制器上的其他设备资源不继承故障、不丢失租约，避免单设备异常扩大。

`NACK`、`TIMEOUT`、`BUSY` 和限额拒绝仍是可重试的事务结果，不自动锁存为后端故障；
板级代码只有在确认控制器或设备后端失效时才应返回 `FAULT`。

F072、F103、G431 当前板级 HAL 尚未提供能保证“只复位指定资源”的安全原语，因此均不
注册 `bus_reset`，对这些实体固件请求总线 `RESOURCE_RESET` 会明确返回
`UNSUPPORTED_CAPABILITY`。这不会误落入 UART、PWM 或 TimedBitstream 的复位分支；
后续只有板级实现满足单资源隔离约束时才应注册该回调。

## 软件验证

`embedded_bus_core_tests` 使用 Mock HAL 覆盖：HAL 合同违约锁存、状态错误位、同控制器
设备隔离、非所有者复位拒绝、复位失败保留锁存、复位成功清除锁存并仅释放目标租约。
验证不依赖实体板卡，也不涉及跨板行为。
