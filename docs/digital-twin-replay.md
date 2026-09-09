# 数字孪生确定性记录与回放 v1

## 目标与边界

该闭环用于证明同一板卡描述、同一故障脚本、同一节点实例和同一随机种子在纯软件
Mock 中得到相同的状态序列。它不是 CAN/CAN-FD 时延仿真器，也不是实体 MCU 的
上电复位证据。

当前可靠记录的输入是已有故障脚本动作：GPIO 外部输入、单路 UART 故障、节点
在线/离线、运动限位和单个 I2C/SPI 设备的下一事务状态。链路延迟、报文丢失和真正
的 MCU 重启尚无统一的 Mock 传输钩子，因此 v1 不伪造这些结果；后续应先把它们
建模为明确的传输事件，再升级 schema。

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

这些结论来自 Ubuntu WSL 的单元测试，没有访问 `vcan0`，也没有实体板卡、电气或
实时性能结论。
