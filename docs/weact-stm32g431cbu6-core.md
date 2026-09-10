# STM32G431CBU6 / WeAct STM32G431CBU6 Core 支持说明

## 硬件基线

- MCU：STM32G431CBU6（QFN48），170 MHz，128 KiB Flash，32 KiB SRAM；
- 时钟：PF0/PF1 连接板载 8 MHz HSE，PC14/PC15 连接 32.768 kHz LSE；
- RemoteBSP 总线：FDCAN，PB8=FDCAN_RX、PB9=FDCAN_TX；
- 默认 CAN-FD：500 kbit/s 仲裁段、1 Mbit/s 数据段、BRS 开启；
- USB：PA11=USB_DM、PA12=USB_DP；默认供 Katapult 恢复，也可构建互斥的 Vendor Bulk APP；
- 硬件UART：USART1（PA9/PA10）、USART2（PA2/PA3）、USART3（PB10/PB11）；
- SWD：PA13=SWDIO、PA14=SWCLK，建议同时接 NRST；
- 板卡类型编号：`0x0431CB`。

本板是通用 `STM32G431CBU6` MCU 支持之上的板型选项。启用
`CONFIG_BOARD_WEACT_G431_CORE_V10` 后，固件会使用板载时钟、按键、LED 和
PB8/BOOT0 约束；其他 G431 板卡不继承这些约束。

## 板载功能与引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| FDCAN RX / TX | PB8 / PB9 | PB8 同时为 BOOT0，使用时必须配置 Option Bytes 忽略 PB8 启动电平 |
| 用户按键 | PC13 | 高电平按下，无外部上下拉；固件启用内部下拉 |
| 指示灯 / 通用 PWM | PC6 / TIM3_CH1 | 高电平点亮；基础 APP 将其作为远程 PWM 资源 |
| 通用定时位流 | PA8 / TIM1_CH1 + DMA1_Channel1/DMAMUX | 可驱动 WS2812 等脉宽编码设备；已交叉编译，待实板验收 |
| USART1 TX / RX | PA9 / PA10 | RemoteBSP UART对象0；可选PB6/PB7端点 |
| USART2 TX / RX | PA2 / PA3 | RemoteBSP UART对象1 |
| USART3 TX / RX | PB10 / PB11 | RemoteBSP UART对象2 |
| USB D- / D+ | PA11 / PA12 | Katapult 应急恢复，或 USB Vendor Bulk APP 主链路 |
| HSE | PF0 / PF1 | 8 MHz 外部晶振 |
| LSE | PC14 / PC15 | 32.768 kHz 外部晶振 |
| SWDIO / SWCLK | PA13 / PA14 | DAPLink 或 ST-Link |

板型预设默认选择 8 MHz HSE，170 MHz SYSCLK、FDCAN 和运动定时均以它为基准。
32.768 kHz LSE 不参与高速系统时钟；当前 APP 只声明并保留 PC14/PC15，等未来
RTC/跨板时间同步保持模块启用后，再由该模块启动、检测并使用 LSE。

PB8 有 10 kΩ 外部下拉且也是 BOOT0。连接 CAN 收发器后，RXD 的空闲高电平可能
影响启动选择；量产烧录应把 Option Bytes 配为 `nBOOT0=1`，使 BOOT0 选择忽略
PB8 电平。应急恢复仍由 PC13 或 `BOOTLOADER_ENTER_USB` 命令完成。

## 当前能力与边界

实体板已经验证：8 MHz HSE、CAN-FD 节点发现、分配、心跳、PING、信息/能力查询、
PC6 PWM 呼吸灯与 PC13 GPIO。实测使用 TJA1051/3 和刷入 CANable2.5
Candlelight/`gs_usb` 固件的 CANable2；500 kbit/s 仲裁段、1 Mbit/s 数据段和
BRS 下 TEC/REC 为 0。

此前板载呼吸灯验证的是 PC6/TIM3_CH1 硬件通道；现在基础 APP 已把同一通道接入
通用远程 PWM 对象，并增加 PA8/TIM1_CH1、DMA1_Channel1/DMAMUX 定时位流后端。
Studio 专用固件已完成 PC6 PWM 对象创建、占空比写入和停止命令验收；实体 WS2812
灯带未连接，因此 PA8 后端当前只有完整组合交叉编译结果，不能视为波形实测。

2 Mbit/s 数据相位在当前飞线条件下曾触发 Bus-Off，因此它保留为 menuconfig
可选高速档，必须在更短支线和更好信号完整性条件下重新验收。Katapult CAN/USB
切换仍待继续验收。APP 不提供 USB CDC；新增 Vendor Bulk APP
已经交叉编译，实体枚举、重连和并发压力仍待验收。

PA0=STEP、PA1=DIR、PA2=EN、PA3=TMC UART 的单轴 TMC2209 接线已完成实板转动
验证。它只作为板名明确的验收预设；核心板本身不假定固定电机插槽，正式接线
仍应由具体扩展板或Studio静态工程确定。该运动验收预设没有启用硬件 USART，因此
TMC 单线端口保持为逻辑 UART 对象 0。

另一次 Studio 全资源工程使用 `PA0=STEP、PA1=DIR、PB0=EN、PB1=TMC UART`，以便
同时保留 PA2/PA3 的 USART2。该固件在未连接电机时完成100 STEP调度，最终位置与
发出步数均为100，状态回到空闲且无故障；这验证了配置生成、合同和compare执行
路径，但不能替代负载下电机、DIR/EN电平和示波器波形验收。

普通CAN-FD板卡预设默认启用三路硬件UART。每路拥有独立的1024字节RX和1024字节
TX环形存储，使用字节中断搬运；一路溢出或重配不会清空另外两路。推荐测试接线：

| USB转串口侧 | G431侧 | 方向 |
|---|---|---|
| ST-Link VCP TX | PA10 / USART1_RX | 转换器到MCU |
| ST-Link VCP RX | PA9 / USART1_TX | MCU到转换器 |
| CH348 A TX | PA3 / USART2_RX | 转换器到MCU |
| CH348 A RX | PA2 / USART2_TX | MCU到转换器 |
| CH348 B TX | PB11 / USART3_RX | 转换器到MCU |
| CH348 B RX | PB10 / USART3_TX | MCU到转换器 |

三者必须共地，只连接3.3V TTL信号，不连接USB转串口模块的5V或3.3V供电脚。

2026-08-17实体板验收结果：三路均以115200、8N1创建为流式对象，USART1使用
ST-Link VCP（Windows COM3），USART2/3使用CH348（本次枚举为COM9/COM7）。
三路同时全双工运行，每个方向、每路发送1024字节的独立递增/递减序列；总计
6144字节逐字节一致，`dropped_bytes=0`、`lost_events=0`。CAN-FD链路在测试后
保持ERROR-ACTIVE，TEC/REC均为0；toolbusd没有拒绝流量包。

## 2026-09-10 CAN-FD 压力、恢复与资源枚举复验

本轮使用枚举为 `1d50:606f` 的 CANable2.5 Candlelight/`gs_usb`、固件版本
`V2J46S32` 的 ST-Link 和同类 G431 核心板。目标探测为 `chipid=0x468`、128 KiB
Flash、32 KiB SRAM，目标电压约 3.225 V。公开文档不记录下载器序列号和完整节点 UUID。

覆盖前已只读备份原厂 Flash；备份 SHA-256 为
`11fff4a1abfebc6d9d485c052a5d1cba2fb3cac6aaabd13d1c1a0b0885459dbd`，离线识别与
WeAct 示例程序相符。读取到 `OPTR=0xFBEFF8AA`、`nSWBOOT0=0`、`nBOOT0=1`。
板卡专用 CAN-FD APP 经 ST-Link 写入后校验 `verified OK`，节点进入 ready，PING、
信息和能力查询成功。

实体压力 200/200 成功，4 个客户端各 50 次并发全部成功；普通请求最小 7 ms、平均
8 ms、p95/最大 10 ms，2023 字节最大 PING 载荷分片往返 56 ms。测试结束保持
ERROR-ACTIVE，TEC/REC、丢包和 bus-off 均为 0。重启 daemon、复位 MCU 以及将
`can0` down/up 后均恢复，其中 MCU 复位后约 4 秒恢复可用。

首次真机联调暴露 STM32 `ResourceEnum` 未连接静态资源表。本轮补齐
Enum/Describe/Status/Contract 后重刷，实体节点枚举出 USART1/2/3、PWM0 和
TimedBitstream0 共 5 项；`RuntimeSnapshot v2` 返回 1 节点、5 资源。配套 72/72
主机测试和 F072、F103、F103/BluePill Plus、F072/FLY-D5、G431、G431/WeAct Core
六目标交叉编译均通过，但它们仍属于软件证据。

同轮继续把 G431 `ResourceStatus` 的 UART 字段接到真实 RX/TX 环形缓冲水位和溢出，
并为 PWM/TimedBitstream 接入对象/后端忙和后端失败基础状态。实板操作还暴露
`pwm-stop` 成功后未释放对象槽：对象 1 停止后再次创建返回状态 8（`ResourceBusy`）。
修复后，实体 `ResourceStatus` 从运行时 `Busy` 回到停止后的 `Normal`，且无需复位即可
在同一通道把对象 1 重建为对象 2。这里的资源级状态不是 CPU、ISR、栈、运动队列或
硬件时间戳等完整 MCU 遥测。

ALIENTEK DL16 已被采集工具识别为真正的 `dl16`。关闭官方上位机后，`atk-logic`
成功采集 D5/PA6 的 1 kHz、50.00% 测试 PWM：10 MHz、20 ms、200000 样本、20 个
上升沿和 20 个下降沿、最短脉宽 499.9 µs、0 毛刺。原始 CSV 和报告保存在 Git 忽略的
`hardware-backups/round21/`。之后同一通道完成 1 kHz/12.34%（10 MHz、20 ms、
40 边沿、最短 123.3 µs）、100 kHz/50%（50 MHz、2 ms、400 边沿、最短 5.0 µs）
和 100 kHz/25%（50 MHz、2 ms、400 边沿、最短 2.5 µs）采集，均为 0 毛刺；最终
生命周期修复固件再次通过 100 kHz/25%，原始附件位于忽略的 `round22/`。
此前 `incomplete` 只发生在 PA0/PA4，不能泛化成任一高电平通道失败；这两路与公共
GND 仍待确认/复测。单路 PA6 PWM 不能据此关闭 STEP、TimedBitstream 或跨板确定性
时序 blocker。通道映射、失败现象和完整证据分层见
[本轮实体验收记录](hardware-evidence-g431-2026-09-10.md)。

## 构建

在名为 `Ubuntu` 的 WSL 中执行：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh

# CAN-FD 独立 APP，从 0x08000000 启动
bash scripts/build_firmware.sh weact-stm32g431cbu6-core

# USB Vendor Bulk 独立 APP，从 0x08000000 启动，不初始化 FDCAN
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-usb

# Classical CAN 单轴 TMC2209 实板验收配置
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-motion

# 8 KiB Katapult 布局的 APP、Bootloader 和工厂镜像
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-katapult
bash scripts/build_bootloader.sh stm32g431_weact_core_dual
bash scripts/build_factory_images.sh weact-stm32g431cbu6-core
```

主要输出：

- `out/remotebsp-stm32g431-weact-core.hex`：CAN-FD 独立 APP；
- `out/remotebsp-stm32g431-weact-core-motion-1axis-tmc2209.hex`：单轴 TMC2209 验收 APP；
- `out/remotebsp-stm32g431-katapult.bin`：在线升级 APP；
- `out/katapult-stm32g431_weact_core_dual.bin`：CAN/USB 双模式 Bootloader；
- `out/remotebsp-stm32g431-katapult-dual-factory.bin`：首次整片镜像。

## 首次烧录与总线接线

使用 ST-Link 或 DAPLink 连接 `SWDIO`、`SWCLK`、`NRST`、`GND` 和 `3.3V/VTref`。
首次烧录双模式工厂镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32g431-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

CAN 收发器连接 PB8=RXD、PB9=TXD、CANH、CANL 和参考地。主机使用 CANable2.5
时，先在 `can0` 上配置 500 kbit/s 仲裁段、1 Mbit/s 数据段和 BRS；具体命令见
[STM32 构建、烧录与总线适配器说明](stm32-build-and-flash.md)。

## 升级入口

- 正常升级：`remote-cli --node N bootloader-enter`，进入 CAN Katapult；
- CAN 可用但需要 USB：`remote-cli --node N bootloader-enter-usb`；
- CAN 完全失效：按住 PC13 后复位，进入 USB Katapult；
- 正常单次复位：启动 RemoteBSP APP。

单节点总线进入 USB Katapult 后若没有其他节点确认发现帧，CANable2.5/`gs_usb`
可能进入 ERROR-PASSIVE。升级完成后重新下线再上线 `can0` 即可恢复接口。

## 相关文档

- [STM32 构建、烧录与总线适配器说明](stm32-build-and-flash.md)
- [Katapult 双模式升级与应急恢复](bootloader-upgrade.md)
- [三块 STM32 实体工具板上板规划](stm32-hardware-plan.md)
