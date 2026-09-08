# STM32F103CBT6 / WeAct BluePill Plus 支持说明

## 硬件基线

- MCU：STM32F103CBT6，外部 8 MHz HSE 经 PLL 到 72 MHz，128 KiB Flash，20 KiB SRAM；
- 低速时钟：PC14/PC15 连接 32.768 kHz LSE；当前 APP 保留该资源，待 RTC/掉线守时模块使用；
- RemoteBSP 总线：Classical CAN，PB8=CAN_RX、PB9=CAN_TX，实测 1 Mbit/s；
- 主机适配器：CANable2 硬件刷入 CANable2.5 Candlelight/`gs_usb` 固件；
- USB：PA11=USB_DM、PA12=USB_DP，仅用于 Katapult 应急升级；
- 硬件UART：USART1（PA9/PA10）、USART2（PA2/PA3）、USART3（PB10/PB11）；
- SWD：PA13=SWDIO、PA14=SWCLK，建议同时接 NRST；
- 板卡类型编号：`0x0103CB`。

本板是通用 `STM32F103CBT6` MCU 支持之上的板型选项。启用
`CONFIG_BOARD_WEACT_BLUEPILL_PLUS` 后，固件会应用 PA0 按键、PB2 LED、USB
引脚和板级保留资源约束；其他 F103 板卡不继承这些约束。

## 板载功能与引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| CAN RX / TX | PB8 / PB9 | bxCAN 重映射，使用 TJA1051/3 等 3.3 V 逻辑收发器 |
| USART1 TX / RX | PA9 / PA10 | RemoteBSP端口0，可选重映射到PB6/PB7 |
| USART2 TX / RX | PA2 / PA3 | RemoteBSP端口1 |
| USART3 TX / RX | PB10 / PB11 | RemoteBSP端口2 |
| 用户按键 | PA0 | 高电平按下，固件启用内部下拉 |
| 指示灯 | PB2 | 高电平点亮；可选约 4 秒周期的软件 PWM 呼吸灯，生产默认关闭 |
| 通用 PWM | PA6 / TIM3_CH1 | 远程配置频率、占空比和极性；已交叉编译，待实板验收 |
| 通用定时位流 | PA8 / TIM1_CH1 + DMA1_Channel2 | 可驱动 WS2812 等脉宽编码设备；已交叉编译，待实板验收 |
| USB D- / D+ | PA11 / PA12 | Katapult USB 恢复，不作为 APP 业务传输 |
| HSE OSC_IN / OSC_OUT | PD0 / PD1 | 8 MHz 外部晶振；启用 HSE 时不会出现在 GPIO/运动引脚选择中 |
| LSE OSC32_IN / OSC32_OUT | PC14 / PC15 | 32.768 kHz 外部晶振；不会作为普通 GPIO 分配 |
| SWDIO / SWCLK | PA13 / PA14 | DAPLink 或 ST-Link |

STM32F103 的 USB 与 bxCAN 共用专用 SRAM。因此 RemoteBSP APP 只运行 CAN，
不启用 USB CDC；双模式 Katapult 根据进入原因在 CAN 与 USB 中二选一初始化。

板型预设默认选择 HSE，使系统、CAN 和运动定时器都以外部晶振为基准。通用
STM32F103 配置可在 menuconfig 中退回 HSI8（64 MHz），但内部 RC 的精度和温漂
更差，只适合没有 HSE 的板卡和较宽松的 CAN 场景。LSE 不参与 72 MHz 主时钟；
在 RTC/时间同步保持功能落地前，固件只保留 PC14/PC15，不会无意义地启动振荡器。

## 当前能力与边界

实体板已经验证：节点发现、分配、心跳、PING、信息/能力查询、PB2 GPIO、USART1
收发、CAN Katapult、USB Katapult 以及 CAN/USB 双路径升级恢复。1 Mbit/s 下短
PING 实测约 12～16 ms；2023 字节最大 PING 载荷实测通过。

2026-08-17新增三路硬件UART后端并通过实体板升级：USART1使用ST-Link VCP，
USART2/3使用CH348，在115200 8N1下同时全双工运行。每个方向、每路1024字节，
共6144字节逐字节一致，MCU与Linux事件缓冲均报告0丢失；测试后CAN仍为
ERROR-ACTIVE，错误计数、丢包和Bus-Off均为0。

本次Windows映射为USART2=`COM9`、USART3=`COM7`，编号只对当前电脑枚举有效。
自编译WSL 6.18内核能识别CH348的USB设备`1a86:55d9`，但未包含CH348串口驱动，
因此测试让CH348保留在Windows侧，CAN与`remote-cli`继续运行于Ubuntu WSL。
Windows打开CH348通道并设置波特率后需等待约500 ms再首发；否则A/B通道可能
各丢第一个测试字节。等待后相位对齐递增序列三路双向全部一致。

可选五轴 TMC2209 构建已通过交叉编译，但尚待本板实体运动与 TMC2209 并发验收。
该预设同时保留 USART1：逻辑 UART 对象 0 是 PA9/PA10 硬件串口，对象 1～5 是
五路固定 40000 bit/s 的 TMC2209 单线后端。TMC 端口不是通用软串口或 Modbus 端口。

基础 APP 已同时链接可选通用 PWM 与定时位流后端。前者使用 PA6/TIM3_CH1，
后者使用 PA8/TIM1_CH1 和 DMA1_Channel2；协议、Mock 和交叉编译已经通过，但
真实频率、占空比、WS2812 时序和运动/CAN 并发仍需实板测量。

## 构建

在名为 `Ubuntu` 的 WSL 中执行：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh

# 独立 APP，从 0x08000000 启动
bash scripts/build_firmware.sh weact-bluepill-plus

# 五轴 TMC2209 编译配置
bash scripts/build_firmware.sh weact-bluepill-plus-motion

# 8 KiB Katapult 布局的 APP、Bootloader 和工厂镜像
bash scripts/build_firmware.sh weact-bluepill-plus-katapult
bash scripts/build_bootloader.sh stm32f103_weact_bluepill_plus_dual
bash scripts/build_factory_images.sh weact-bluepill-plus
```

主要输出：

- `out/remotebsp-stm32f103-bluepill-pb8-pb9.hex`：独立 APP；
- `out/remotebsp-stm32f103-bluepill-motion-5axis-tmc2209.hex`：五轴 TMC2209 APP；
- `out/remotebsp-stm32f103-bluepill-katapult.bin`：在线升级 APP；
- `out/katapult-stm32f103_weact_bluepill_plus_dual.bin`：CAN/USB 双模式 Bootloader；
- `out/remotebsp-stm32f103-bluepill-katapult-dual-factory.bin`：首次整片镜像。

## 首次烧录与总线接线

使用 ST-Link 或 DAPLink 连接 `SWDIO`、`SWCLK`、`NRST`、`GND` 和 `3.3V/VTref`。
首次烧录双模式工厂镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32f103-bluepill-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

CAN 总线连接 `CANH`、`CANL` 和参考地。物理干线两端各放一个 120 Ω 终端，
断电测量 CANH-CANL 应约为 60 Ω。主机 `can0` 与此固件均使用 Classical CAN
1 Mbit/s。

## 升级入口

- 正常升级：`remote-cli --node N bootloader-enter`，进入 CAN Katapult；
- CAN 可用但需要 USB：`remote-cli --node N bootloader-enter-usb`；
- CAN 完全失效：按住 PA0 后复位，进入 USB Katapult；
- 正常单次复位：启动 RemoteBSP APP。

单节点总线进入 USB Katapult 后若没有其他节点确认发现帧，CANable2.5/`gs_usb`
可能进入 ERROR-PASSIVE。升级完成后执行 `ip link set can0 down`、`ip link set
can0 up` 即可恢复接口。

## 相关文档

- [STM32 构建、烧录与总线适配器说明](stm32-build-and-flash.md)
- [Katapult 双模式升级与应急恢复](bootloader-upgrade.md)
- [三块 STM32 实体工具板上板规划](stm32-hardware-plan.md)
