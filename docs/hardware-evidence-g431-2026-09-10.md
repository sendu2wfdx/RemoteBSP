# 2026-09-10 STM32G431 CAN-FD 实体验收记录

本文记录一轮 WeAct STM32G431CBU6 Core、CANable2.5 和 ST-Link 的实体联调结果。
记录只覆盖本轮实际观察到的板卡、链路与恢复行为。自动测试、交叉编译和逻辑分析仪
采集分别列出，不能互相替代。

为避免公开可关联的设备身份，本文不记录 ST-Link 序列号和完整 RemoteBSP 节点 UUID。
原厂 Flash 备份保存在 Git 忽略目录中，仓库只记录其摘要。

## 1. 环境与设备身份

| 项目 | 本轮记录 |
|---|---|
| 目标板 | WeAct STM32G431CBU6 Core |
| MCU 探测 | `chipid=0x468`，128 KiB Flash，32 KiB SRAM |
| 下载器 | ST-Link，固件版本 `V2J46S32`；公开记录省略序列号 |
| 目标电压 | 约 3.225 V |
| CAN 适配器 | CANable2.5 Candlelight/`gs_usb`，USB VID:PID `1d50:606f` |
| APP 链路 | CAN-FD，500 kbit/s 仲裁段、1 Mbit/s 数据段、BRS |
| 逻辑分析仪 | ALIENTEK DL16，工具识别结果为 `dl16` |

CAN 收发器和主机已经完成有效通信，但测试结束时仍未取得用户对逻辑分析仪、目标板和
激励侧公共 GND 接法的口头确认。因此，本轮不能仅凭通信成功推断 DL16 采集参考地已经
正确连接；该项必须在下一次波形采集前现场复核。

## 2. 原厂状态保护与启动配置

覆盖前已对整片原厂 Flash 做只读备份。备份文件位于 Git 忽略目录，SHA-256 为：

```text
11fff4a1abfebc6d9d485c052a5d1cba2fb3cac6aaabd13d1c1a0b0885459dbd
```

离线识别结果与 WeAct 出厂示例程序相符。该判断只用于说明覆盖前镜像的来源类别，不把
原厂示例当作 RemoteBSP 验收固件，也不公开备份路径。

Option Bytes 读取结果：

- `OPTR = 0xFBEFF8AA`；
- `nSWBOOT0 = 0`；
- `nBOOT0 = 1`。

本轮未改写上述 Option Bytes。PB8 同时承担 BOOT0/FDCAN_RX 复用风险，后续量产流程仍应
把 Option Bytes 读取值纳入每块板的烧录记录，不能只依赖板型默认假设。

## 3. 板卡 APP、发现与基础命令

ST-Link 已把板卡专用 CAN-FD APP 写入目标板，下载器写后校验结果为 `verified OK`。
复位后 `toolbusd` 将目标分配为本次会话的 node 1，节点进入 ready；以下命令路径均成功：

- `PING`；
- 节点信息读取；
- 能力查询。

node 1 是本次会话中的临时数字路由号，不是持久设备身份。本记录不公开完整节点 UUID。

## 4. CAN-FD 压力与恢复

基础及压力结果：

| 项目 | 结果 |
|---|---|
| 顺序压力 | 200/200 成功 |
| 并发压力 | 4 个客户端各 50 次，共 200/200 成功 |
| 普通请求往返 | 最小 7 ms、平均 8 ms、p95 10 ms、最大 10 ms |
| 最大 PING 载荷 | 2023 字节，分片往返 56 ms |
| 测试结束总线状态 | `ERROR-ACTIVE` |
| 错误与丢失 | TEC=0、REC=0、丢包=0、bus-off=0 |

恢复动作也在同一实体链路上执行：

- 重启 `toolbusd` 后节点重新进入可用状态；
- MCU 复位后约 4 秒恢复到主机可用状态；
- 将 `can0` down 后再 up，链路和节点恢复。

这些结果证明当前单节点、当前线束和当前速率下的发现、分片、并发请求及三种恢复路径。
它们不覆盖多节点不受影响性、总线接近饱和时的优先级隔离、物理断线时的安全输出电平，
也不提供最坏恢复时延统计。`partial_failure_recovery` 因此继续保持开放。

## 5. 实体发现暴露的资源枚举缺口

首次刷入板卡专用 APP 后，节点基础命令正常，但真机发现 `ResourceEnum` 路径没有返回
静态板卡资源。这是实体环境发现的软件缺口，不能记录成测试接线问题。

本轮已在 STM32 公共 Remote Core 补齐以下只读资源命令与静态表连接：

- `ResourceEnum`；
- `ResourceDescribe`；
- `ResourceStatus`；
- `ResourceContract`。

后续同轮迭代又把 `ResourceStatus` 的 G431 UART 字段接到真实 RX/TX 环形缓冲水位和
溢出计数，并为 PWM、TimedBitstream 接入对象/后端忙状态与后端失败锁存。PWM 的
`Busy -> Normal` 生命周期已在本板复验；其余失败分支主要仍是代码和自动测试证据。
这些资源级基础状态不是 CPU/空闲率、ISR 最大耗时、栈水位、运动队列或硬件时间戳等
完整 MCU 遥测。`ResourceContract` 未声明的时延、吞吐和操作队列指标仍保持为 `0`，
不能解释为无限能力或性能保证。

修复后完成主机全量 72/72 测试，并交叉编译以下六个正式目标：F072、F103、
F103/BluePill Plus、F072/FLY-D5、G431、G431/WeAct Core。两类结果属于自动测试和
交叉编译证据，不是实体板证据。

重新烧录同一 G431 后，实体枚举返回 5 项静态资源：

- 3 个 UART；
- `PWM0`；
- `TimedBitstream0`。

随后 `RuntimeSnapshot v2` 返回 `nodes=1`、`resources=5`。这证明 G431 当前正式配置的
静态表、Remote Core 四个只读资源命令、`toolbusd` 和 Runtime 快照已经在实体 CAN-FD
链路上贯通。它不证明 UART 电气收发、PWM/TimedBitstream 波形、I2C/SPI、ADC、Timer、
Storage 或高速 Stream 已完成实体验收。

本轮继续操作 PWM 时发现一个资源生命周期缺口：首次创建得到对象 1，`pwm-stop`
虽然停止了后端输出，却没有释放 Remote Core 对象槽；同一资源再次创建因此返回状态 8
（`ResourceBusy`）。修复为仅在后端停止成功后释放对象后，实体板验证了：

- PWM 运行期间 `ResourceStatus` 为 `Busy`；
- `pwm-stop` 后同一资源状态回到 `Normal`；
- 不复位 MCU 即可在同一通道把对象 1 停止并重新创建为对象 2。

这证明当前 G431 PWM 单对象停止/重建路径已经闭合，但不等于所有资源类型、会话清理、
后端失败恢复或节点复位矩阵都已完成。

## 6. DL16 识别、通道映射与采集结果

采集工具将设备识别为真正的 `dl16`，本轮输入通道映射为：

| DL16 通道 | G431 引脚 |
|---|---|
| D0 | PC13 |
| D1 | PC15 |
| D2 | PA0 |
| D3 | PA2 |
| D4 | PA4 |
| D5 | PA6 |
| D6 | PC4 |
| D7 | PB1 |

激励输出规划为 `PWM0 -> PB10`、`PWM1 -> PB11`，但本轮没有启用这两个激励输出。
DL16 在 5 MHz、20 ms 窗口下完成全低电平基线采集。此前 `incomplete` 现象只在 PA0、
PA4 两个被测通道出现，不能再概括成“任一 MCU 通道为高电平都会失败”；当时设备侧进度
显示完成，但结果中没有 channel bytes。这两个通道仍需复测。

关闭 DL16 官方上位机、释放设备后，`atk-logic` 已成功采集 D5/PA6 的测试 PWM。
首轮 1 kHz、50% 基线为：

| 采集项 | 结果 |
|---|---:|
| 采样率 | 10 MHz |
| 采样窗口 | 20 ms |
| 样本数 | 200000 |
| 总边沿 | 40 |
| 上升沿 / 下降沿 | 20 / 20 |
| 频率 | 1000 Hz |
| 占空比 | 50.00% |
| 最短脉宽 | 499.9 µs |
| 毛刺 | 0 |

原始 CSV 与分析报告位于 Git 忽略目录：

```text
hardware-backups/round21/pa6-d5-pwm-1khz.csv
hardware-backups/round21/pa6-d5-pwm-1khz-report.md
```

同一 PA6/D5 通道又完成三组占空比与高频采集：

| 设定 | 采样率 / 窗口 | 样本数 | 边沿 | 占空比 | 最短脉宽 | 毛刺 |
|---|---|---:|---:|---:|---:|---:|
| 1 kHz / 12.34% | 10 MHz / 20 ms | 200000 | 40 | 12.34% | 123.3 µs | 0 |
| 100 kHz / 50% | 50 MHz / 2 ms | 100000 | 400 | 50.00% | 5.0 µs | 0 |
| 100 kHz / 25% | 50 MHz / 2 ms | 100000 | 400 | 25.00% | 2.5 µs | 0 |

三组原始 CSV 与分析报告位于 `hardware-backups/round22/`。修复上述 PWM 对象生命周期后，
最终固件再次完成 100 kHz / 25% 采集：50 MHz、2 ms、100000 样本、400 边沿、
25.00%、最短脉宽 2.5 µs、0 毛刺。对应忽略目录文件为：

```text
hardware-backups/round22/pa6-d5-pwm-1khz-12p34.csv
hardware-backups/round22/pa6-d5-pwm-1khz-12p34-report.md
hardware-backups/round22/pa6-d5-pwm-100khz-50.csv
hardware-backups/round22/pa6-d5-pwm-100khz-50-report.md
hardware-backups/round22/pa6-d5-pwm-100khz-25.csv
hardware-backups/round22/pa6-d5-pwm-100khz-25-report.md
hardware-backups/round22/pa6-d5-pwm-100khz-25-lifecycle-fixed.csv
hardware-backups/round22/pa6-d5-pwm-100khz-25-lifecycle-fixed-report.md
```

这些结果证明 DL16 与 `atk-logic` 的单路采集/解码链路可用，并验证 PA6 上 1 kHz 两种
占空比、100 kHz 两种占空比及生命周期修复后的重复高频输出。由于公共 GND 仍待用户
口头确认、PA0/PA4 尚未复测，且这些信号不是持续高负载 STEP/DIR/EN 或跨板同步边沿，
不能据此关闭确定性时序 blocker，也不能外推 TimedBitstream、最坏抖动、ISR 耗时或
安全停机时延。

## 7. 证据层级与仍开放的门槛

本轮可作为实体证据的内容：

- G431 型号、存储容量、目标电压、Option Bytes 和原厂镜像摘要；
- ST-Link 写入板卡专用 APP 并校验成功；
- 单节点 CAN-FD 发现、基础命令、200 次顺序与 200 次并发请求、2023 字节分片；
- daemon 重启、MCU 复位和 `can0` down/up 后恢复；
- 修复后的 5 项静态资源枚举和 RuntimeSnapshot 实体链路贯通；
- PWM 停止后 `Busy -> Normal`、对象 1 不复位重建为对象 2；
- PA6/D5 的 1 kHz/50%、1 kHz/12.34%、100 kHz/50%、100 kHz/25% 波形，
  以及生命周期修复固件上的 100 kHz/25% 重复采集。

以下内容不能提升为实体波形或整体成熟度证据：

- 72/72 测试和六目标交叉编译；
- PA0/PA4 返回 `incomplete` 的采集，以及尚未复测的其他运动/同步输出；
- 资源级 UART/PWM/TimedBitstream 基础状态对完整 MCU 健康遥测的外推；
- 尚未确认的公共 GND；
- 单节点结果对多节点同步、其他两类 MCU 或 RemoteBSP/Klipper 对照性能的外推。

本轮不关闭任何成熟度 blocker。尤其是 `determinism_hardware_timing`、
`multi_node_hardware_sync`、`incomplete_resource_families`、`partial_failure_recovery`、
`hardware_records_not_reproducible` 和 `hardware_evidence_incomplete` 仍需后续实体环境与原始
附件补齐。PA6 的原始 CSV/报告虽已保存，但完整命令输出、接线确认、其他板卡和其他信号
仍未形成统一记录。
