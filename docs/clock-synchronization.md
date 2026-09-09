# 主机时钟同步模型

## 范围

本模块是跨板运动同步的主机侧前置算法，只负责把主机单调纳秒时间映射到单个 MCU
的自由运行计数器。当前不定义线协议、不发送同步报文、不修改 MCU 时钟，也不接触
实体板。

实现位于：

- `toolbusd/include/remotebsp/toolbusd/clock_model.hpp`
- `toolbusd/src/clock_model.cpp`
- `tests/clock_model_tests.cpp`

它输出可审计的比例、偏移参考点、RTT、漂移、误差上界和同步状态，供未来节点注册、
运动 PREPARE/READY/COMMIT 准入使用。

## 四时间戳样本

一次同步交换产生：

| 字段 | 时钟域 | 含义 |
|---|---|---|
| `host_send_ns` | 主机 | 同步请求发出时刻 |
| `node_receive_tick` | MCU | MCU 尽早锁存的接收计数器值 |
| `node_send_tick` | MCU | MCU 发响应前锁存的发送计数器值 |
| `host_receive_ns` | 主机 | 主机收到响应时刻 |

主机时间必须来自同一个单调时钟。MCU 两个时间戳必须来自同一个连续自由运行计数器，
不能一个取系统毫秒、另一个取运动定时器。

每个样本计算：

```text
host_midpoint = host_send + (host_receive - host_send) / 2
node_midpoint = node_receive + (node_send - node_receive) / 2
host_rtt = host_receive - host_send
network_rtt ≈ host_rtt - node_turnaround / nominal_tick_rate
nominal_offset = node_midpoint - nominal_scale * host_midpoint
```

减去 MCU 处理时间后，`network_rtt / 2` 是链路方向不对称未知时的基本误差包络。
算法不会把偏移估计本身包装成比该物理边界更精确的结论。

## 输入约束与回绕

`node_counter_bits` 可配置为 2 至 64 位：

- 64 位输入视为已经扩展的单调计数，倒退直接拒绝；
- 较短计数器按模数展开为逻辑 64 位值；
- 相邻接收样本以及单次 MCU 处理时间必须小于半个计数器周期；
- 等于或超过半周期的跳变无法区分“向前跨越很久”和“时间倒退”，按
  `CounterAmbiguous` 拒绝；
- 原始值超出声明位宽按 `CounterOutOfRange` 拒绝；
- 逻辑 64 位展开即将溢出时拒绝，不静默回绕。

因此同步轮询周期必须明显小于半回绕周期。例如 1 MHz 的 32 位计数器约 71.6 分钟
回绕一次，实际同步间隔应远小于约 35.8 分钟的歧义边界。

主机样本中点和接收时间也必须严格前进。被拒绝的样本不更新计数器展开参考、滑动
窗口或模型。

## 低 RTT 选择与稳健拟合

模型维护固定容量滑动窗口，重建过程为：

1. 按估计 `network_rtt` 从小到大排序，RTT 相同时优先保留较新样本；
2. 只保留配置数量的低 RTT 样本，降低排队和调度暂停造成的偏移误差；
3. 按主机时间排序，并检查样本数和时间跨度；
4. 计算所有样本对的斜率，使用 Theil–Sen 中位斜率估计频率比例；
5. 选择中间主机时刻为参考点，以中位参考值估计偏移；
6. 用残差、半 RTT 和一个 MCU tick 的量化误差形成基础误差上界；
7. 用成对斜率离散度的稳健中位数估计漂移不确定度，并设置配置下限。

模型形式为：

```text
node_tick = reference_node_tick
          + scale_ticks_per_ns * (host_ns - reference_host_ns)
```

使用参考点表达而非巨大绝对截距，可以减少长时间运行后的浮点消减误差。STEP ISR
不会运行拟合；未来只在主机上拟合，并在运动段提交前把全局段边界换算为节点 tick。

`maximum_rate_deviation_ppm` 是板卡晶振与配置一致性的硬边界。拟合结果越界时模型
保持无效，不能用一个数学上可拟合但物理上不可信的频率启动运动。

## 状态与误差老化

状态只有三种：

| 状态 | 条件 | 运动准入 |
|---|---|---|
| `UNSYNCED` | 样本不足、跨度不足、拟合无效、查询时间倒退或模型过期 | 禁止跨板提交 |
| `SYNCED` | 模型有效、样本新鲜且当前误差上界不超过阈值 | 可以进入 READY 检查 |
| `DEGRADED` | 模型仍可解释，但样本变旧或误差上界越界 | 禁止新提交，已有事务按安全策略处理 |

误差随最后样本年龄增长：

```text
current_error_bound = base_error_bound
                      + sample_age * drift_uncertainty_ppm / 1_000_000
```

老化起点取实际入选拟合的最新样本；未入选的高 RTT 样本不能刷新模型。超过
`synchronized_max_age_ns` 先降级；超过 `model_expiry_ns` 变为未同步。即使数值
映射仍可计算，调用方也必须先检查同一时刻的 `estimate()`，不得使用过期模型执行
跨板运动。

建议运行时使用两个阈值：

- READY 阈值应严于运动组承诺，给提交确认和继续老化留余量；
- 运行停止阈值可稍宽，但一旦越界只能在共同安全边界停止或立即进入组安全状态，
  不能在运行中突跳已提交段的本地 tick。

## 确定性与限制

- 相同配置和相同样本顺序得到相同的选择、拟合和状态结果；
- 所有顺序检查、回绕展开和年龄计算使用整数；拟合使用 `long double`，测试使用物理
  误差容限而不是依赖特定位级浮点结果；
- 低 RTT 选择能抵抗高延迟/排队离群点，Theil–Sen 能抵抗少量时间戳残差离群点；
- 持续的单向链路不对称无法仅凭四时间戳完全识别，只能进入误差上界；
- 当前模型每个实例只对应一个节点和一个 `boot_epoch`。节点重启必须丢弃旧实例，
  不能把新计数器接到旧展开状态；
- 当前未估计温度模型，也不使用 CAN 硬件时间戳。未来增加更高质量时间戳时不能改变
  已有字段含义，应通过能力和样本质量标记演进。

## 已覆盖的软件测试

- `+80 ppm` 与 `-120 ppm` 的频率和双向时间映射；
- 固定上下行延迟不对称，并验证真实偏移误差落在误差上界内；
- 高 RTT 且强不对称的离群点，验证低 RTT 子集不被污染；
- 新鲜、降级、过期和查询时间倒退状态；
- 32 位计数器在单次响应和连续样本间跨越 `0xffffffff -> 0`；
- 超半周期歧义、位宽越界、主机时间顺序错误、MCU 处理时间不可能和 RTT 超限；
- 无有效模型时禁止时间换算。

这些测试只证明算法和边界条件，不代表实体 CAN 适配器、MCU 时间戳位置或晶振稳定
性已经验证。

## 后续接入点

本轮不修改 CMake。集成时需要：

1. 在 `toolbusd/CMakeLists.txt` 的 `remotebsp_request_manager` 源文件列表加入
   `src/clock_model.cpp`；
2. 在 `tests/CMakeLists.txt` 新增 `clock_model_tests`，链接
   `remotebsp_request_manager`，沿用 `-Wall -Wextra -Wpedantic -Werror`；
3. 节点注册表以后按 `node UUID + boot_epoch` 持有一个模型实例；
4. 同步协议落地后，由请求管理器记录主机发送/接收时间，MCU 返回接收/发送 tick，
   再调用 `add_sample()`；
5. PREPARE/READY 使用 `estimate(now)` 的状态、误差上界、样本年龄和模型代数做准入；
6. COMMIT 前固定所用模型代数和换算结果，不能让新样本改写已提交段。

协议设计还需补充同步请求 ID、`boot_epoch`、计数器频率/位宽、时间戳质量、模型代数
和错误码。未定义这些字段前，主机算法保持独立，不在现有消息中复用含义不相符的字段。
