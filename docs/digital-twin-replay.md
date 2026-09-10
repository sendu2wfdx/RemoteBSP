# 数字孪生确定性记录与回放 v1

## 目标与边界

该闭环用于证明同一板卡描述、同一故障脚本、同一节点实例和同一随机种子在纯软件
Mock 中得到相同的状态序列。它不是 CAN/CAN-FD 时延仿真器，也不是实体 MCU 的
上电复位证据。

状态回放的输入是已有故障脚本动作：GPIO 外部输入、单路 UART 故障、节点在线/离线、
运动限位和单个 I2C/SPI 设备的下一事务状态。另有独立的 `TransportReplayRecord v2`
用于 Mock 边界的逻辑帧事件；两种记录不混用 schema，也不把逻辑帧结果伪装成物理链路
测量。

## 逻辑传输事件钩子

`transport_replay.hpp` 提供确定性的纯软件模型，并提供显式的 MockNode 边界适配：
`deliver_replayed_frame_to_mock_node` 把 H2N 逻辑交付送入现有 `handle_frame`，
`make_mock_node_reply_replay_events` 把 MockNode 回复转换为 N2H 逻辑帧事件。适配器默认不
改变 SocketCAN、USB 和 Mock USB 实现；`RecordingLinkTransport` 只在调用方显式包裹现有
`LinkTransport` 时记录逻辑边界。`mock_mcu` 可用 `--transport-record 文件` 启用该包裹，
未传此选项时链路行为保持不变。
输入事件按 `monotonic-relative-ms` 相对时间非递减排列，并用从 1 连续递增的
`sequence` 消除同一毫秒内的歧义。当前动作如下：

| 动作 | 作用域与语义 |
|---|---|
| `DelayNext` | 累加到同节点、同方向的下一帧，交付时刻为输入相对时间加延迟 |
| `DropNext` | 丢弃同节点、同方向的下一帧 |
| `DuplicateNext` | 为同节点、同方向的下一帧增加一个副本，可有限累计 |
| `NodeReboot` | 清除该节点尚未交付的延迟帧和未消费故障，并推进 `session_generation` |
| `Frame` | 携带不透明 `route` 和字节载荷；钩子不解释协议或设备业务 |
| `SendFailure` | 保存发送失败前的目标方向、路由和尝试载荷；不伪造成已交付帧 |
| `ReceiveEmpty` | 保存一次超时/空结果及调用方给出的 timeout |
| `ReceiveFailure` | 保存接收调用抛出异常的边界；不伪造成空结果或帧 |

每个帧和一次性故障都必须声明 `HostToNode` 或 `NodeToHost`，两个方向的故障状态互不
消费；reboot 本身不带方向并同时清理该节点两个方向。某节点 reboot 不会清除其他节点
的待交付帧。
同一交付时刻继续按输入序号和副本顺序稳定排列，输出的 `delivery_sequence` 从 1 连续
递增。记录包含事件数、`DropNext` 丢弃数、注入副本数、reboot 清除的待交付帧数、
重启数、最终节点代次和全部逻辑交付；reboot 失效帧不会混入主动丢弃计数。
`replay_digest` 绑定规范输入与这些输出；`verify_transport_replay` 会重新执行并比较完整
记录，而不只相信调用方给出的摘要字符串。录制器使用可注入的单调相对毫秒时钟；测试
可完全控制时间，实际 Mock 默认使用 `steady_clock`。每次观察在真实 I/O 前预留事件、
载荷容量并固定调用起始时间，I/O 后提交不再读取时钟或分配内存。成功
`send`、失败 `send`、有帧 `receive`、空结果 `receive` 和异常 `receive` 是不同事件。
故障注入器必须显式调用 `record_fault`/`record_reboot`，录制器不会从普通收发结果臆测
drop、duplicate 或 reboot。

资源边界均在分配或入队前失败关闭：最多 4096 个事件、1024 个同时待交付帧、32768
个总交付、单帧 4096 字节、输入总载荷 4 MiB、单节点下一帧累计延迟 60000 ms，且每帧
最多注入 7 个额外副本。故障和重启事件节点号限制为 1～127；广播帧或无法归属单节点
的收发结果可使用节点 0。相对时间加延迟溢出、非连续序号、时间倒退、未知动作和动作
携带多余字段均被拒绝。若容量耗尽、时钟倒退或观察无法规范编码，诊断采集停止且最终
拒绝生成完整会话；`RecordingLinkTransport` 仍执行和返回真实 I/O，不因录制器状态改变
发送/接收的成功、失败或可重试语义。该行为只保证有界诊断，不能保证无限时长采集。

节点归属完全来自方向与每帧实际 route：只有 H2N 的定向请求解析节点号，只有 N2H 的
节点响应和节点事件解析节点号；错方向 route、Discovery 广播、provisional 响应和未知
route 均记为 0。录制器不把 Mock 的启动 instance 硬套到共享总线上的所有帧，也不解析
可能跨分片的 `NodeAssign` 载荷；分配发生后，后续定向 route 自然反映当前节点号。回放
交付给 `MockNode` 时还会再次验证：节点 0 只允许精确的 Discovery 广播，稳定节点必须
同时匹配记录的 node ID 与 `request_base + node_id`，因此未知或其他节点 route 不会借
节点 0 绕过生产主循环的过滤。

## 逻辑传输会话文件 v1

`TransportReplaySession` 使用稳定的规范文本格式，文件头为
`REMOTEBSP_TRANSPORT_SESSION 1`。元数据固定声明
`evidence_scope logical-link-boundary-only` 和
`time_base monotonic-relative-ms`；随后是有界事件、派生摘要及覆盖正文的
`fnv1a64` 校验和。解析器要求逐字节规范编码，拒绝未知/尾随字段、非小写十六进制、
计数不一致、摘要或校验和不一致。最多 4096 条记录、单载荷 4096 字节、总载荷 4 MiB，
整个文件最多 16 MiB；文件入口先检查长度，并检测读取过程中增长。

写入使用 `mkstemp` 在同目录创建唯一且独占的临时文件，完整刷新并关闭后用硬链接原子
创建目标，再删除本次临时文件。策略固定为 **no-clobber**：目标已存在即报错，不覆盖、
不删除旧记录；失败只清理自己创建的临时文件。当前没有对文件或父目录执行 `fsync`，
因此只保证并发可见性与不覆盖，不承诺掉电后的持久性。`load_transport_replay_session`
解析后重新运行确定性调度并校验全部摘要。该校验用于发现损坏和不一致，不是密码学签名。

Mock 录制示例：

```text
mock_mcu mock-endpoint usb-mock --instance 7 \
  --transport-record run.transport-session.txt
```

正常退出或主循环异常退出时都会尝试落盘。若记录容量耗尽或输出失败，会报告诊断错误且
不生成伪装成完整证据的新文件；运行中的 Mock 数据面不会因此退出或改变 I/O 结果。

该钩子只证明 Mock 调度、隔离和会话代次逻辑可确定重放。它不模拟 CAN 仲裁、总线负载、
CAN-FD 位时序、USB transaction、主机调度、电气噪声或真实设备复位时长，因此不能作为
CAN/USB 物理层可靠性或实时性能证据。

## 时间与确定性

- 时间基固定为 `monotonic-relative-ms`，即 Mock 启动后的单调相对毫秒；不读取
  墙上时间。
- `at_ms` 相同的相邻事件组成一个原子检查批次。记录中的 `sequence` 从 1 连续
  递增，每个检查点保存本批实际应用的事件数。
- `random_seed` 总是写入记录。v1 的现有动作不使用随机数，但摘要仍绑定该值，
  防止未来加入概率故障时意外使用隐式随机源。
- `scenario_id` 是对解析后故障脚本的规范字段序列计算的 `fnv1a64` 标识；JSON
  空白和键顺序不会改变身份，事件内容或顺序变化一定进入身份计算。
- 每个检查点和最终状态都有摘要。摘要绑定完整的机器可读板卡清单、节点实例、
  随机种子、在线状态、GPIO（含尚未创建对象的预注入输入）、各 UART 状态、运动
  状态及指标、波形状态和排序后的总线设备状态。总线的一次性 `bus_status` 不会因
  藏在后端而漏出摘要；同名但硬件合同不同的板卡也不会被当成同一回放输入。

标准 FNV-1a 64 在这里用于快速确定性一致性检测，不是密码学签名，不能证明文件来自可信
主体。需要防篡改分发时，应在记录文件之外增加签名或可信制品摘要。

## 严格 JSON schema

根对象只允许以下字段：

| 字段 | 约束 |
|---|---|
| `schema_version` | 必须为 `1` |
| `time_base` | 必须为 `monotonic-relative-ms` |
| `random_seed` | 64 位非负整数 |
| `scenario_id` | `fnv1a64:` 加 16 位小写十六进制 |
| `board_name` | 1～256 字节 |
| `node_instance` | 1～127 |
| `checkpoints` | 最多 65535 个、时间严格递增 |
| `summary` | 与检查点事件总数、数量和最终时间严格一致 |

每个检查点只允许 `sequence`、`at_ms`、`applied_events` 和 `state_digest`。摘要只
允许 `event_count`、`checkpoint_count`、`final_elapsed_ms`、`final_online` 和
`final_state_digest`。未知字段、重复键、缺字段、错误类型、非连续序号、零事件
检查点、无效摘要和大写或错误长度的摘要都会被拒绝。
整个回放 JSON 最大 16 MiB；文件入口会在分配和读取正文前检查长度，避免把任意大的
不可信文件一次性载入内存。

## API 闭环

```cpp
using namespace remotebsp::mock_mcu;

const auto manifest = load_board_manifest("board.json");
const auto scenario = load_fault_scenario("faults.json");

const auto record = record_digital_twin(manifest, scenario, 1, 42);
write_twin_replay_record(record, "run.replay.json");

const auto loaded = load_twin_replay_record("run.replay.json");
verify_digital_twin_replay(manifest, scenario, 1, loaded);
```

`verify_digital_twin_replay` 会从零重新创建孪生，逐检查点推进并比较脚本身份、板卡、
节点、事件批次和状态摘要。任何差异都以 `ManifestException` 失败关闭。写文件先写
同目录临时文件，再以平台提供的原子 rename 替换目标文件；若平台不能原子覆盖既有
文件则失败关闭并保留旧文件，不通过先删除旧文件来伪造原子性。临时文件使用
独占创建；同名临时文件或符号链接已存在时拒绝写入，避免跟随可预测链接截断旧文件。

## 已验证的软件性质

- 编码后解析并重放得到完全一致的规范 JSON；
- 同毫秒多事件只形成一个有确定顺序的检查点；
- 修改状态摘要、脚本内容、节点实例或序号会被拒绝；
- 同一输入重复记录逐字节一致；不同节点实例和不同种子摘要隔离；
- GPIO 预注入、UART 故障、节点掉线/恢复和 I2C/SPI 一次性故障均进入摘要；
- 纯代码构造的乱序场景也会被拒绝，不依赖先经过 JSON 解析器。
- 逻辑传输钩子的 delay/drop/duplicate/reboot 组合可重复得到相同交付顺序和摘要；
  单节点 reboot 不影响其他节点的延迟帧，篡改交付内容会在验证时被拒绝；
- 真实 `LinkTransport` 包装测试覆盖成功/失败发送、有帧/空结果/异常接收、方向、节点
  隔离和可注入单调时钟；会话文件往返一致，载荷篡改、尾随内容和超限输入均被拒绝。

这些结论来自 Ubuntu WSL 的单元测试，没有访问 `vcan0`，也没有实体板卡、电气或
实时性能结论。
