# 三块 STM32 实体工具板上板规划

适用板卡：STM32F072RBT6 / Mellow FLY-D5、STM32F103CBT6 / WeAct BluePill Plus、
STM32G431CBU6 / WeAct STM32G431CBU6 Core。

## 默认引脚

通用 MCU 配置可以使用 PA11/PA12 CAN，但当前两块 WeAct 实板为了保留 USB，
均使用 PB8/PB9：

| 功能 | 引脚 | 说明 |
|---|---|---|
| CAN/FDCAN RX | PB8 | 接 TI 收发器 RXD |
| CAN/FDCAN TX | PB9 | 接 TI 收发器 TXD |
| SWDIO | PA13 | DAPLink/ST-Link |
| SWCLK | PA14 | DAPLink/ST-Link |
| NRST | NRST | 建议下载器同时连接 |
| GND | GND | 下载器、MCU、收发器共地 |

PA11/PA12 在 F103 上是 bxCAN 默认引脚，在 G431 上可配置为 FDCAN1
复用功能；不会占用 SWD。PB8/PB9 作为 menuconfig 备选，F103 使用该组时需要
打开 AFIO CAN 重映射，G431 使用 AF9。

PA11/PA12 同时是 USB D-/D+。不启用 USB 的通用配置可以把 CAN 放在
PA11/PA12。STM32F103 的 USB 与 bxCAN 还共用 512 字节专用 SRAM，即使把 CAN
改到 PB8/PB9，两者也不能同时运行；因此 F103 双模式 Katapult 根据进入原因
只初始化 CAN 或 USB，APP 只运行 CAN。STM32G431 的 APP 可以让 USB 使用
PA11/PA12、FDCAN 使用 PB8/PB9；其双模式 Katapult 为了统一升级工具链，也在
Bootloader 阶段只启动一个升级接口。Kconfig 会约束这些组合。

当前带 Katapult 的推荐引脚为：

| 功能 | 引脚 |
|---|---|
| CAN/FDCAN RX | PB8 |
| CAN/FDCAN TX | PB9 |
| USB D- | PA11 |
| USB D+ | PA12 |
| USB 恢复键 | WeAct BluePill Plus 为 PA0；WeAct STM32G431CBU6 Core 为 PC13；均为内部下拉、按下为高 |

WeAct STM32G431CBU6 Core 还固定占用 PF0/PF1 连接 8 MHz HSE、PC14/PC15
连接 32.768 kHz LSE。PC6 是高电平点亮的板载蓝色 LED。PB8/BOOT0 虽有
10 kΩ 外部下拉，但 CAN 收发器 RXD 的主动高电平会覆盖该下拉，因此使用
PB8 FDCAN 时必须通过 Option Bytes 让启动选择忽略 PB8 电平。

## 收发器建议

建议两块板统一使用支持 3.3V 逻辑和 CAN-FD 的 TI 收发器，例如 TCAN3413。
这样 F103 运行 Classical CAN、G431 运行 CAN-FD 时可以复用同一物理层设计。

- TXD 接 MCU CAN_TX。
- RXD 接 MCU CAN_RX。
- STB 低电平为正常收发模式。
- 如果 STB 不需要软件控制，可直接下拉到 GND。
- 如果需要低功耗/唤醒，通过 menuconfig 指定一个 GPIO 控制 STB。
- 总线两端各放置 120Ω 终端电阻，中间节点不要重复终端。

## 默认速率

- F103 通用配置：500 kbit/s Classical CAN。
- STM32F103CBT6 / WeAct BluePill Plus 实机配置：1 Mbit/s Classical CAN。
- G431：实测默认 500 kbit/s 仲裁段、1 Mbit/s 数据段 CAN-FD；2 Mbit/s
  保留为改善拓扑和信号完整性后的可选高速档。

速率全部由 menuconfig 设置。实际布线较长、节点较多或隔离器传播延迟较大时，
需要降低速率并重新计算采样点。1 Mbit/s Classical CAN 尤其需要控制主干和
支线长度，并保证总线只有两个 120Ω 终端。

## 下载连接

DAPLink 和 ST-Link 均使用 SWD：

```text
下载器 SWDIO -> PA13
下载器 SWCLK -> PA14
下载器 NRST  -> NRST
下载器 GND   -> GND
下载器 VTref -> 核心板 3.3V
```

VTref 用于检测目标逻辑电平，除非下载器明确支持给目标供电，否则不要把它当作
核心板电源。
