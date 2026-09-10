# toolbusd 健康快照生产与可信接线

本文说明 `HealthSnapshot v1` 在 toolbusd 软件侧的生产边界。它只描述可由当前
软件对象证明的数据，不是 CAN、USB 或 MCU 物理层测量报告。

## 来源与时间基

- `source` 固定为 `Toolbusd`，`node_id` 固定为 `0`；远端节点不得生成或转发
  这个来源的快照。
- `producer_generation` 必须与 daemon instance 同生命周期，由 daemon 启动时
  生成独立、非零的 64 位世代；不能简单截断 UUID，也不能在重启后复用。
- `sample_time_ms` 是生产者启动后的相对单调毫秒，不是 Unix 时间。
- `sample_sequence` 在单个生产者世代内严格递增；序号耗尽时停止生产，不能回绕。

消费端必须先从可信本地 IPC 会话取得 daemon 身份，再同时核对
`source=Toolbusd`、`node_id=0` 与 `producer_generation`。daemon 换代后应先切换可信
世代并清空旧序号水位；旧世代载荷一律拒绝。同一序号只允许字节完全相同的幂等
重放，低序号、同序号异载荷和时钟回退均不得覆盖最后一个有效样本。

## 当前可证明指标

`ToolbusdHealthProducer` 直接接收 `RequestManager` 和 `TrafficController` 已有状态
的快照值。生产边界会再次校验流量分类汇总、容量、版本和计数溢出，并立即执行
一次公共 v1 编码/解码校验。

| 指标 | availability | 语义 |
|---|---|---|
| `request_queue_depth` | available | 采样瞬间的 toolbusd 待处理请求数 |
| `session_generation` | available | 与头部 `producer_generation` 相同 |
| `uptime_milliseconds` | available | daemon 本世代相对运行时间 |
| `active_lease_count` | available/unknown | 仅在 RuntimeControlGate 明确提供时 available |
| `resource_fault_count` | available/unknown | 仅在资源注册表明确提供时 available |
| CPU、ISR、栈、重试、超时、物理 Rx/Tx/drop | unavailable | 当前软件对象没有对应测量，不能用 0 冒充 |

toolbusd 的流量控制器统计的是“准入决策”，不代表帧已被物理链路发送。为避免
与标准 `tx_frame_total` 混淆，使用生产者扩展 ID：

| ID | 名称 | 说明 |
|---:|---|---|
| `0x8001` | `traffic_admitted_packet_total` | 通过准入的逻辑包累计数 |
| `0x8002` | `traffic_rejected_packet_total` | 被准入控制拒绝的逻辑包累计数 |
| `0x8003` | `traffic_guaranteed_overrun_total` | 保证类流量越过预算的累计数 |
| `0x8004` | `traffic_admitted_frame_total` | 准入估算产生的链路帧累计数 |
| `0x8005` | `traffic_estimated_wire_time_ns` | 准入模型估算的累计线时，不是实测值 |

累计拒绝或越界不能单独证明系统此刻故障，因此生产者当前将 `overall` 保持为
`unknown`。后续只有在加入明确、带时效的故障状态机后才能输出 `degraded/fault`。

## Runtime 投影

`runtime_api.health_projection.TrustedToolbusdHealthProjection` 对 wire 载荷执行长度、
版本、保留位、排序、重复 ID、单位、availability/value、队列深度/容量和代际一致性
校验。稳定投影保留数值 `metric_id` 与排序后的指标列表；未知扩展使用
`unknown_metric_<id>` / `unknown_unit_<id>`，从而允许非安全用途的前向兼容。

`unavailable` 和 `unknown` 的 `value` 在 API 中统一为 `null`，只有 `available` 的
零值才表示真实测量为零。任何解码、路由、代际或陈旧错误都只拒绝当前载荷，不能
污染上一份已接受快照，也不能自动切换可信 daemon 世代。

## 运行中链路

运行中的 toolbusd 提供 `HealthSnapshot` 本地只读 IPC v1。请求携带 IPC 版本和零
保留位；响应在同一个消息中原子携带 128 位 `daemon_instance_id` 与 HealthSnapshot
v1 载荷，避免分别查询身份和快照产生重启竞态。`libremotebsp::Client` 和
`remote-cli --json health-snapshot` 不改变指标语义，只执行有界解码和结构化转发。

Runtime Provider 将同一实例 ID 与唯一生产者世代绑定，并串行化健康 IPC 调用，避免
旧 daemon 的迟到结果覆盖新 daemon。认证后的 `GET /api/v1/health` 在
`toolbusd_health` 中返回稳定投影；公开未认证调用仍只返回 liveness，完全不读取
RuntimeSnapshot 或健康 Provider。健康遥测失败只把该字段标为暂时不可用，不会把已
成功读取的节点/资源快照伪装成失败，也不会返回后端异常文本。

## 非目标

- 不声称已经采集 MCU 的 CPU、ISR、栈或外设物理状态。
- 不从流量准入计数推断 CAN/USB 实际发送、接收或丢帧。
- 不允许远端包声明 `source=Toolbusd` 后直接进入可信健康状态。
- 不允许消费端仅凭 payload 自报的 generation 建立信任；必须使用同一 IPC 响应里的
  daemon instance 进行绑定。
