# 统一板卡描述与数字孪生 Mock

## 目标与边界

统一板卡描述用于消除 Mock、固件校验器、配置器和文档分别维护资源表造成的
矛盾。当前第一版先覆盖 Mock MCU 已实现的公共信息：

- 板型、固件版本和 UUID 模板。
- 固件实际启用的能力。
- 对 Linux 公开的资源及其能力合同。
- 原生资源、扩展资源和板级内部占用关系。
- 可重复执行的基础故障场景。

当前描述不会修改 STM32 固件，也不会替代 `firmware/Kconfig`。F103/G431 的
芯片引脚复用、DMA、Flash 分区和安全电平尚未迁入该格式；等公共 schema 稳定
后，再让固件生成器和图形配置器读取同一描述包。

## 默认描述

默认 Mock 描述位于：

```text
boards/mock-generic-v1.json
```

它声明：

- 板型 `0x4d4f434b`，即 Mock Generic。
- GPIO、UART、PWM、定时位流、Bootloader 和 Motion 能力。
- 16 路原生 GPIO。
- UART 0～3 为原生串口。
- UART 4～7 为扩展串口。
- STEPGEN 0～2 是默认描述的三路示例轴；自定义描述可以按板卡性能声明更多或更少。
- PWM 0～1 是两路通用输出，定时位流 0 可用于 WS2812 等脉宽编码设备。
- 扩展串口组占用内部 SPI 1，因此 SPI 1 不会出现在远程资源目录中。
- 每项公开资源对应一份资源能力合同。

`board_type` 表示板型，同型号的多块板保持相同；Mock 的 `--instance` 会写入
UUID 最后一个字节，用于生成不同节点身份。UUID 才用于区分具体板卡。

## Schema v1

根对象必须包含以下字段，未知字段会被拒绝，避免拼写错误静默生效：

| 字段 | 含义 |
|---|---|
| `schema_version` | 当前固定为 1 |
| `name` | 板卡描述名称，1～64 字节 |
| `board_type` | 32 位板型编号 |
| `uuid` | 32 个十六进制字符；Mock 实例覆盖最后一个字节 |
| `firmware_version` | `[主版本, 次版本, 修订版本]` |
| `capabilities` | 本构建实际启用的能力数组 |
| `resource_groups` | 对 Linux 公开的连续资源组 |
| `reserved_resources` | 被板级功能内部占用、不公开调度的资源 |
| `motion_axes` | STEPGEN资源的步频、脉宽、最低低电平和方向建立时间 |

资源组使用 `first_instance`、`count` 和 `id_base` 生成连续资源，避免手工复制
大量同类条目。每组还包含：

- `type`：`gpio`、`uart`、`spi`、`i2c`、`adc`、`pwm`、`timer`、
  `storage`、`stepgen_axis` 或 `timed_bitstream`。
- `source`：`native` 或 `expanded`。
- RX/TX 缓冲容量。
- 读写、共享、独占和租约能力。
- 定时分辨率、最坏延迟、吞吐和队列保证值。

加载器执行严格校验：

- schema 版本、JSON 类型、整数范围和 UUID 格式必须正确。
- 未知字段、重复 JSON 键、重复能力和重复访问标志会被拒绝。
- 资源 ID 以及“类型/实例”必须唯一。
- 资源必须拥有对应的能力位。
- `lease_required` 必须同时声明 `lease_supported`。
- 同一底层资源不能既公开，又出现在 `reserved_resources` 中。
- UART、PWM和定时位流实例号必须位于协议允许范围，并且资源组不能越界。

能力合同中的零仍表示“不适用或尚未声明”，不表示无限能力。Mock 数值只用于
流程测试；实体板保证值必须来自 F103/G431 压力测试。

## 加载自定义板卡

Mock 默认加载仓库内置描述，也可以显式指定：

```sh
./build-wsl/mock_mcu/mock_mcu vcan0 classical \
    --board boards/mock-generic-v1.json \
    --instance 1
```

加载失败时进程在打开业务链路前退出并报告原因，不会带着部分资源目录运行。

## 故障场景

故障场景同样使用严格 JSON，当前版本为 1。事件必须按相对 Mock 启动时间
`at_ms` 非递减排列。第一版动作包括：

| `action` | 必要字段 | 效果 |
|---|---|---|
| `uart_failed` | `resource_id`、`value` | 锁存或恢复单路 UART 故障 |
| `gpio_input` | `resource_id`、`value` | 注入 GPIO 外部输入电平 |
| `node_online` | `value` | 模拟整节点离线或恢复 |
| `motion_limit` | `resource_id` | 在指定时刻触发本地运动限位并安全停机 |

示例文件：

```text
tests/data/mock_fault_scenario.json
```

运行：

```sh
./build-wsl/mock_mcu/mock_mcu vcan0 classical \
    --fault-scenario tests/data/mock_fault_scenario.json
```

单路 UART 故障只改变对应资源状态和操作结果；其他 UART、GPIO 及节点主循环继续
运行。`node_online=false` 时 Mock 停止心跳、响应和 UART 事件，但仍推进单调
时钟上的故障脚本；恢复后继续服务。GPIO 输入事件可以先于应用创建 GPIO 对象，
Mock 会保存物理输入电平，并在该引脚配置为输入时生效。

数字孪生使用 `steady_clock` 的启动后相对时间，不依赖系统墙上时间。单元测试
直接推进虚拟毫秒位置，验证同一脚本得到相同的状态转换顺序。

## 当前已验证内容

- 默认描述生成 16 GPIO、8 UART、3 STEPGEN、2 PWM、1定时位流和30份一一对应的能力合同；这不是
  框架固定轴数。
- 板型对多实例保持不变，UUID 对不同实例保持唯一。
- 扩展 UART 的内部 SPI 占用不会被公开枚举。
- 严格 schema、未知字段和乱序故障事件拒绝。
- UART 1 故障时 UART 0 仍可完成原子发送。
- UART 故障恢复、预注入 GPIO 输入、节点离线和恢复。
- 可变轴数共同时间线、正反向位置、最终段收尾、队列欠载和限位停机。
- PWM创建/更新/停止，以及定时位流写入、取消和标准WS2812逻辑像素解码。
- 描述接入后 Classical CAN、CAN-FD 和双节点端到端测试保持通过。

## 后续扩展

后续在不破坏 v1 文件的前提下通过新 schema 版本加入：

- 芯片、封装、引脚复用、定时器通道、DMA 和 ADC 映射。
- SWD、晶振、CAN、USB、Bootloader 和硬件 ID 保留项。
- Flash/RAM、A/B 配置槽、擦除粒度和安全默认电平。
- TMC2209 专用单线 UART、TMC SPI 事务、输入采样和更完整的资源依赖图。
- 晶振漂移、CAN 延迟/丢包、队列欠载、重启与会话确定性重放。
- 从同一描述生成固件静态检查、GUI 选项、接线表和资源占用报告。

## GPIO 与板级接口约束

实体板不能把“封装上存在的 GPIO”直接等同于“用户可任意配置的接口”。统一描述
后续按三层表达数字 IO：

1. 固定占用：SWD、CAN、晶振、Bootloader 恢复口、硬件 ID 等，GUI 不允许分配。
2. 板级定向接口：已经连接到按键、限位、继电器、MOS 管或收发器的引脚。描述中
   保存接口名称、固定或允许的方向、外部上下拉、有效电平和安全状态；GUI 默认按
   接口名称展示，并锁定原理图决定的属性。
3. 通用引脚：核心板排针等未被外围电路限定的 GPIO，GUI 才允许选择输入/输出、
   内部上拉/下拉、有效电平、输出安全初值、输入消抖和事件触发方式。

运行时资源清单引用板级接口或通用引脚，并为每个逻辑 GPIO 保存稳定名称。板卡
描述决定“硬件允许什么”，资源清单决定“这台设备实际拿它做什么”；两者都不会
把编译期 `menuconfig` 重新变成接线真相。所有输出必须具有明确的上电和故障安全
电平，输入的内部上下拉必须与板上外部电路兼容。
