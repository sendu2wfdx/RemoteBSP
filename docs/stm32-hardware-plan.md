# STM32F103CBT6 与 STM32G431CBU6 上板规划

## 默认引脚

两块核心板默认统一采用：

| 功能 | 引脚 | 说明 |
|---|---|---|
| CAN/FDCAN RX | PA11 | 接 TI 收发器 RXD |
| CAN/FDCAN TX | PA12 | 接 TI 收发器 TXD |
| SWDIO | PA13 | DAPLink/ST-Link |
| SWCLK | PA14 | DAPLink/ST-Link |
| NRST | NRST | 建议下载器同时连接 |
| GND | GND | 下载器、MCU、收发器共地 |

PA11/PA12 在 F103 上是 bxCAN 默认引脚，在 G431 上可配置为 FDCAN1
复用功能；不会占用 SWD。PB8/PB9 作为 menuconfig 备选，F103 使用该组时需要
打开 AFIO CAN 重映射，G431 使用 AF9。

PA11/PA12 会占用 USB D-/D+，当前工具板固件不实现 USB，因此优先保证两种 MCU
引脚统一。如果将来需要 USB，可以切换到 PB8/PB9。

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

- F103：500 kbit/s Classical CAN。
- G431：500 kbit/s 仲裁段、2 Mbit/s 数据段 CAN-FD。

速率全部由 menuconfig 设置。实际布线较长、节点较多或隔离器传播延迟较大时，
需要降低数据段速率并重新计算采样点。

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
