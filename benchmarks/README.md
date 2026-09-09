# RemoteBSP / Klipper 对照基准

本目录用于预注册可复现的对照实验，不保存营销评分。当前
`comparison-plan-v1.json` 是 `draft`，因此不能作为
`comparative_benchmark` 成熟度证据，也不能解除“整体超过 Klipper”的阻塞项。
`comparison-plan-v1.schema.json` 供编辑器和外部工具使用，仓库内严格状态门槛由
`validate_comparison_plan.py` 执行。
每次配对运行可使用 `run-manifest-v1.schema.json` 保存双侧指标和原始文件索引，并由
`validate_run_manifest.py` 重新计算大小和 SHA-256。该验证器只证明列出的文件未被静默
替换，不会从仪器数据重算指标，也不能发现未列入索引的失败样本，因此本身不是比较证据。
运行记录的 `plan_sha256` 用于未来绑定计划定义。当前记录中的 revision、环境摘要、
固件摘要和指标仍是声明字段；必须由后续执行验证器与 Git 对象、生产批次、烧录回读、
环境清单和仪器原始数据交叉核对。在此以前，即使运行清单完整性验证通过，也不得写入
成熟度清单的 `comparative_benchmark`。

## 为什么必须先预注册

RemoteBSP 和 Klipper 的产品边界并不相同。RemoteBSP 面向通用远端硬件资源，Klipper
面向 3D 打印；比较只能覆盖双方可实现的共同工作负载，并把产品能力完整度放回成熟度
矩阵判断。不能因为某一个延迟数字更低，就推出整体成熟度更高。

基准设计依据 Klipper 官方文档公开的主机/MCU 命令排程、消息协议和多 MCU 时钟同步
边界：

- <https://www.klipper3d.org/MCU_Commands.html>
- <https://www.klipper3d.org/Protocol.html>
- <https://www.klipper3d.org/Code_Overview.html>
- <https://www.klipper3d.org/Features.html>

上位机功能比较以 Moonraker 官方接口文档为基线，但 Runtime/Moonraker 功能覆盖不能
混入 MCU 实时性能分数：

- <https://moonraker.readthedocs.io/en/latest/external_api/introduction/>
- <https://moonraker.readthedocs.io/en/latest/external_api/authorization/>

## 状态门槛

- `draft`：允许版本、板卡和证据路径留空，只证明实验结构可机器校验。
- `preregistered`：必须锁定双方 40 位 Git revision、同一主机和实体板、配置文件、
  仪器、阈值与输出路径；开始采样后不得静默修改计划。
- `executed`：v1 明确拒绝该状态。只有针对最终仪器格式实现双侧指标重算、完整运行
  索引、固件镜像/回读绑定、失败样本检测和独立运行验证后，才会在后续 schema 开放。

验证草案：

```sh
python3 benchmarks/validate_comparison_plan.py
```

实体环境准备好后，应先复制并锁定草案，改成 `preregistered`，提交到 Git，再开始
测量。采样必须保存双方固件、主机配置、固件哈希、逻辑分析仪原始数据、环境信息和
完整分析输出。Mock、主机计时和交叉编译只能用于调试实验工具。
