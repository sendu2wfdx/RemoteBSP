# 遥测与健康契约 v1

## 当前范围

`protocol/health.hpp` 定义了 MCU、Remote Core 和 toolbusd 共用的机器可读契约，命令号
为 `HealthSnapshot (0x0021)`。toolbusd 已接入仅汇总自身可证明软件状态的生产者，并经
版本化只读 IPC、`libremotebsp`、`remote-cli --json` 和 Runtime 可信投影贯通到认证读取
接口。公共嵌入式 Remote Core 和 Mock Remote Core 已实现单节点 `HealthSnapshot` 应答，
`libremotebsp` 与 `remote-cli node-health-snapshot` 可读取原始节点快照；Runtime 已按可信
节点 UUID、当前路由 ID、来源和生产者代际接入节点投影。节点自身 `sample_time_ms` 属于节点
时间基，Runtime 不与主机时钟直接相减；主机单调时钟只计算同一采样序号持续未更新的年龄。
读取失败、离线、来源或路由不符、陈旧样本和已退休代际显式表示为 `unavailable` 或
`unknown`，且单节点失败不阻断其他节点。F072、F103 与 G431 已接入独立双页 Flash
健康生产者代际 HAL，并完成软件故障注入与交叉编译；但实体 Flash 掉电/升级保留尚未
验收，CPU/ISR/栈采样后端也仍未实现，因此未覆盖字段会诚实返回不支持，不能据此声称
已有完整实体遥测。该健康代际日志不替代尚未接入的跨板运动 `boot_epoch`。

这是一份软件状态契约，不是物理测量合同。Mock 环境不能真实测得 MCU CPU/ISR 占用、
栈水位、CAN 仲裁延迟、USB transaction 延迟、温度、电压或电气错误；这类指标必须标为
`unavailable`，不能填入看似正常的零值。

## 状态与零值

每个指标都带独立的 `MetricAvailability`：

| 状态 | 含义 | `value` 规则 |
|---|---|---|
| `available` | 当前样本确实测量或由本层计数得到 | 允许为 0 |
| `unavailable` | 此生产者或当前构建不提供该测量能力 | 必须为 0，仅作规范占位 |
| `unknown` | 理论上有来源，但当前尚无有效样本或样本已失效 | 必须为 0，仅作规范占位 |

因此 `available + value=0` 与 `unavailable/unknown + value=0` 有明确区别。消费者必须先
检查状态，不能把后两者显示为“0%”“0 次”或“健康”。`OverallHealth::Unknown` 同样不
等于 `Healthy`。某个指标完全缺席表示“本快照未报告”，它也不能被解释为 0；希望明确
声明能力缺失或暂时无样本的生产者，应分别发送 unavailable 或 unknown 项。

## 快照结构与资源上限

v1 使用 36 字节固定头和最多 48 个 12 字节指标项，单个编码载荷最多 612 字节：

- `version` 固定为 1；
- `source` 标识 MCU、Remote Core 或 toolbusd；
- `overall` 为 unknown、healthy、degraded 或 fault；
- `sample_sequence` 从 1 开始，由各生产者单调推进；
- `sample_time_ms` 是生产者启动后的相对单调毫秒，不是墙上时间；
- `node_id` 为 0～127，其中 0 可表示守护进程整体或尚未分配节点；
- `producer_generation` 必须非零，并在生产者重启或会话重建后更换；即使本次没有
  `boot/session generation` 指标，消费者也能据此使旧样本失效；
- `metrics` 必须按 `metric_id` 严格递增，不得重复；
- 头部保留位必须为 0，载荷不得有尾随字节。

固定上限使 MCU 编码、守护进程采样和不可信载荷解码都能有界运行。每个节点快照独立
编解码；某节点返回错误版本、错误长度或非法指标时，应只丢弃该节点本次样本并记录问题，
不得阻塞其他节点。本轮未定义无界历史缓存；保留历史时应由上层另行设置环形容量。

## 接线与信任边界

二进制字段本身不提供身份认证。生产者接入 MockNode、Remote Core 或 toolbusd 时，消费端
必须先从可信的本地路由或已认证会话取得预期身份，再同时核对 `source` 和 `node_id`；三者
任何一项不匹配，都只能作为隔离后的诊断输入，不能更新受信健康状态、解除故障或参与安全
决策。`producer_generation` 与 `sample_sequence` 只在这个已核对的生产者身份内比较：代际
变化使旧样本立即失效，同代序号必须单调推进，不能跨节点或跨来源拼接。

`HealthSource::Toolbusd` 只能由本机 toolbusd 状态采集路径产生。来自 CAN、USB、Mock 链路
或其他远端输入、却自称 toolbusd 的快照必须拒绝进入受信状态，不能因为枚举值合法而接受。
未知非零 `source` 为前向兼容会被解码并原样保留，但在消费者显式支持并绑定可信路由前，
不得参与任何安全判断，也不得冒充 MCU、Remote Core 或 toolbusd。当前 toolbusd 正式接线
在同一个 IPC 响应中原子返回 daemon instance ID 与独立随机、非零的生产者代际；Runtime
按实例和代际建立可信路由，并拒绝陈旧序号、同序号异载荷和采样时钟回退。该绑定只覆盖
受信本机 toolbusd 来源，不代表 MCU/Remote Core 已完成身份认证或生产者接线。

## 标准指标

| 指标 | 单位 | 合理生产者与注意事项 |
|---|---|---|
| CPU/ISR load | permille | 只有存在可靠计时窗口的 MCU/进程采样器才能 available；Mock 为 unavailable |
| minimum stack free | bytes | 只有 RTOS/裸机栈水位后端才能 available；0 可表示实测耗尽，因此不能代替 unavailable |
| request queue depth/capacity | count | toolbusd 请求管理器可提供 |
| motion queue depth/capacity | count | 启用运动模块的 Remote Core/MCU 可提供 |
| stream buffered/capacity | bytes | 启用 Stream 且能获得一致快照时可提供 |
| retry/timeout totals | count | toolbusd 请求管理器可单调累计 |
| duplicate/unexpected responses | count | toolbusd 匹配层可累计 |
| RX/TX/drop frame totals | count | 仅表示对应软件边界看到的逻辑帧；不代表物理总线计数 |
| boot/session/clock-model generation | generation | 已知时必须大于 0；用于判断计数器重置和旧样本失效 |
| uptime | milliseconds | 生产者自身的单调运行时间 |
| active leases/resource faults | count | Remote Core 或 toolbusd 可由当前状态计算 |
| sample overruns | count | 有界周期采样器跳过样本时累计 |

嵌入式 HAL 的 `health_sample` 回调必须提供每次重启均变化的非零生产者代际，并通过字段位图
逐项声明 CPU、ISR、最小栈余量和采样丢失计数是否真实可用。回调缺失、代际为零、字段位非法
或负载超过 1000 时，Remote Core 返回 `UnsupportedCapability`，不会生成全零“正常”快照。
运动队列深度/容量、活动租约数、资源故障数和 uptime 由公共 Core 从同一时刻的软件状态计算；
同步处理路径没有请求队列，因此请求队列深度和容量明确标为 `unavailable`。

F072、F103 与 G431 共用同一 STM32 健康 HAL。该 HAL 在应用映像末端、设备参数区之前
保留独立双页 Flash 日志，并只在启动时以 CRC、反码和末尾提交标记推进一次代际；写入、
校验、日志歧义或耗尽失败时，固件继续提供其他功能，但健康回调失败关闭。运行期采样不擦写
Flash，也不执行动态分配。当前三板没有能够覆盖完整调度窗口且已校准的 CPU/ISR 占用采样，
也没有可靠栈水位填充，因此这些字段继续为 `unavailable`，不能把默认零值解释成健康。

CPU 和 ISR 负载的 available 值限制为 0～1000。队列或缓冲区的 available 容量必须大于
0；当深度和容量同时 available 时，深度不得超过容量。available 的代际值必须大于 0。
累计计数器允许为 0；生产者重启导致计数归零时，必须更换头部必填的
`producer_generation`，消费者不得跨代直接求差。可选的 boot/session generation 指标仅
补充描述具体子系统代际，不能替代头部生产者代际。

## 兼容演进

- v1 解码器严格拒绝未知 schema 版本，因为不同版本可能改变二进制布局。
- 新增指标使用新的非零 `metric_id`；旧解码器会保留未知 ID，不会猜测含义。
- 未知非零 `source` 也原样保留，消费者应按未知生产者处理，而非冒充 MCU/toolbusd。
- 标准指标必须使用契约规定的单位。未知指标允许携带未知非零单位 ID并原样保留，便于
  未来扩展；单位 0 始终非法。
- availability 和 overall health 会影响安全判断，因此未知枚举值严格拒绝，不能自动
  降级为 healthy 或 available。

该策略允许在不改布局的前提下添加来源、指标和单位，同时对会改变安全语义的字段失败
关闭。若未来需要直方图、浮点值或多窗口统计，应发布新版本，不能复用 v1 的整数值字段。

## 已验证边界

Ubuntu WSL 单元测试覆盖规范往返、available 零值与 unavailable/unknown 的区别、未知
来源/指标/单位保留、重复与乱序指标、标准单位不匹配、非 available 非零值、负载范围、
队列关系、必填生产者代际与可选指标零代际、数量上限、保留位、截断和尾随字节。测试
使用始终执行的显式检查，不依赖 `assert`，并在 Release/`NDEBUG` 配置验证。嵌入式单测还
覆盖 MCU 样本字段、代际、序号、时间基、运动队列以及不可用语义；Mock 单测覆盖生产者代际
隔离与单调序号。本测试不使用 vcan，也不形成实体硬件或实时性能结论。
