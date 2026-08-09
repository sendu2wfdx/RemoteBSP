# STM32G431CBU6 / WeAct STM32G431CBU6 Core 支持说明

## 硬件基线

- MCU：STM32G431CBU6（QFN48），170 MHz，128 KiB Flash，32 KiB SRAM；
- 时钟：PF0/PF1 连接板载 8 MHz HSE，PC14/PC15 连接 32.768 kHz LSE；
- RemoteBSP 总线：FDCAN，PB8=FDCAN_RX、PB9=FDCAN_TX；
- 默认 CAN-FD：500 kbit/s 仲裁段、1 Mbit/s 数据段、BRS 开启；
- USB：PA11=USB_DM、PA12=USB_DP，仅用于 Katapult 应急恢复；
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
| USB D- / D+ | PA11 / PA12 | Katapult 应急恢复，不由 APP 初始化 |
| HSE | PF0 / PF1 | 8 MHz 外部晶振 |
| LSE | PC14 / PC15 | 32.768 kHz 外部晶振 |
| SWDIO / SWCLK | PA13 / PA14 | DAPLink 或 ST-Link |

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
新对象协议和交叉编译已经通过，但动态 PWM 命令与实体 WS2812 波形仍待上板验收。

2 Mbit/s 数据相位在当前飞线条件下曾触发 Bus-Off，因此它保留为 menuconfig
可选高速档，必须在更短支线和更好信号完整性条件下重新验收。Katapult CAN/USB
切换和 UART 外设后端仍待继续验收；APP 不再提供 USB CDC。

PA0=STEP、PA1=DIR、PA2=EN、PA3=TMC UART 的单轴 TMC2209 接线已完成实板转动
验证。它只作为板名明确的验收预设；核心板本身不假定固定电机插槽，正式接线
仍应由具体扩展板或后续运行时资源清单确定。该预设没有启用硬件 USART，因此
TMC 单线端口保持为逻辑 UART 对象 0。

## 构建

在名为 `Ubuntu` 的 WSL 中执行：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh

# CAN-FD 独立 APP，从 0x08000000 启动
bash scripts/build_firmware.sh weact-stm32g431cbu6-core

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
