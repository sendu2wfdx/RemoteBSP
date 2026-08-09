# Mellow FLY-D5（STM32F072RBT6）支持说明

## 硬件基线

本板按 Mellow 官方资料配置：

- MCU：STM32F072RBT6，板载 8MHz HSE 经 PLL 到 48 MHz，128 KiB Flash，16 KiB SRAM；
- RemoteBSP 总线：Classical CAN，PB8=CAN_RX、PB9=CAN_TX；
- USB：PA11=USB_DM、PA12=USB_DP，只用于 Katapult 应急升级；
- SWD：PA13=SWDIO、PA14=SWCLK；
- 默认 CAN 仲裁段速率：1 Mbit/s；
- 板卡类型编号：`0x00F072D5`。

FLY-D5 是通用 `STM32F072RBT6` MCU 支持之上的一个板型配置，不是独立复制的
固件实现。它通过 `CONFIG_BOARD_MELLOW_FLY_D5` 增加板载 USB、固定 CAN
引脚和五轴资源约束；其他 F072 板卡不会继承这些限制。

FLY-D5 的 APP 直接使用板载 CAN 收发器接入 RemoteBSP 总线，不实现 Klipper
的“USB 转 CAN 桥”。协议层仍然不知道 USB、bxCAN 或 SocketCAN 的具体类型。

当前未从可静态核对的官方资料中确认一个独立、可安全公开的硬件通用 UART 端口。
默认配置中的五路通讯均是驱动插槽专用的 TMC2209 单线 UART，只能作为
原始事务通道使用，不能冒充面向外设的全双工通用串口。

## 板载功能引脚

以下引脚来自 Mellow 官方 Klipper 参考配置，作为后续运行时资源清单和智能运动
执行器的基线：

| 插槽 | STEP | DIR | EN | TMC UART/CS | 限位 |
|---|---|---|---|---|---|
| DRIVER0 | PC15 | PC14 | PC2，低有效 | PC13 | PB4 |
| DRIVER1 | PA1 | PA0 | PA2，低有效 | PC3 | PB3 |
| DRIVER2 | PA5 | PA4 | PA6，低有效 | PA3 | PD2 |
| DRIVER3 | PB10 | PB2 | PB11，低有效 | PB1 | — |
| DRIVER4 | PC5 | PC4 | PB0，低有效 | PA7 | — |

其他已知板载信号：

| 功能 | 引脚 |
|---|---|
| 热端加热 / 热敏 | PC6 / PC1 |
| 热床加热 / 热敏 | PC7 / PC0 |
| 可控风扇 | PC9、PC8 |
| 探针 / 舵机 | PB5 / PA8 |

通用波形基础预设还提供 PA6/TIM3_CH1 PWM 和 PA8/TIM1_CH1+DMA 定时位流。
PA6 同时是 DRIVER2 的 EN，PA8 同时是探针/舵机接口，因此它们只是不同固件用途
下的候选映射，不能与对应运动/板载功能同时启用；引脚校验器和 GUI 会拒绝冲突。

当前已核对的 FLY-D5 板级资料未给出一个可安全作为 MCU 默认指示灯使用的独立 GPIO。
因此固件不会为了呼吸灯占用上述加热、风扇、步进或探针资源；后续由运行时资源清单
确认实际指示灯引脚后，再将其配置为可选呼吸灯输出。

当前 APP 已在实体板验证发现、节点分配、心跳、PING、信息查询、能力查询和原子 GPIO。
五路单线 UART（PC13、PC3、PA3、PB1、PA7）均已读取 TMC2209
`IOIN=0x21000041`，并完成五电机转动验证；Linux 侧负责 TMC 数据报和 CRC，MCU
只执行原始字节收发。

通用 PWM、定时位流协议和板级后端已加入基础 APP 并交叉编译。PWM 使用
PA6/TIM3_CH1；WS2812 等位流使用 PA8/TIM1_CH1 和 DMA1_Channel2。它们尚未在
FLY-D5 上使用示波器或灯带验证，不属于已有五轴实测结论。

FLY-D5 默认预设会启用 `CONFIG_REMOTEBSP_MOTION` 并填写已知接线：TIM2 提供
1MHz 单调时基，TIM2_CH1 compare按下一条STEP边沿时间调度中断，不再占用TIM3。
该预设不是功能开关；取消它后，
STM32F072RBT6 仍可手工启用“智能步进运动”及各槽引脚。只有启用
“智能步进运动”后，`menuconfig` 才会显示“兼容期静态步进槽映射”。每个槽先选择驱动器：
“通用 STEP/DIR”或“TMC2209（STEP/DIR + 单线 UART）”；选择后者才显示该槽的
TMC UART 引脚，并自动链接 TMC2209 专用单线通讯执行器。关闭运动模块时，运动队列、
定时器后端、轴设置和因轴驱动器选择而拉入的通讯后端均不链接。该后端只执行原始字节
事务，TMC 寄存器协议始终由 Linux 处理。引脚使用按 GPIO 端口标注的下拉选择；
固定占用和前面已经选择的引脚会被隐藏，启动时仍会二次拒绝重复、CAN/USB/SWD
保留或不存在的引脚。每个槽可独立启用；实际公开的逻辑轴按已启用槽的 D0→D4 顺序紧凑
编号。`MOTION_MAX_AXES` 是 RAM/队列的编译期容量上限，必须不小于已启用槽数量，
并不表示五个物理槽都必须使用。启用运动后，所选 STEP/DIR/EN 引脚会从普通 GPIO
资源中保留。

旧固定tick执行器已在实体板完成五轴同段各 3200 STEP、持续 6 秒、自动关闭 EN；运行中主动
`MOTION_ABORT` 与非最终段队列欠载也已实板验证会关闭全部 EN。D2 单轴已验证
4500 step/s、9000 步完整输出并自动禁用。当前固件已切换为compare调度，并保守
保留单轴5k step/s、整板25k step/s的入队预算；这些数值只是延续已知安全范围，
compare版本仍需用示波器和CAN/UART并发压力测试重新标定。

当前软件 UART 事务会短时关中断，因此运动已武装或运行时固件拒绝新的软 UART
写请求，避免破坏 STEP 时序；待实现非阻塞状态机后解除此限制。数字孪生描述
`boards/mellow-fly-d5-v1.json`仍按五轴能力建立。

## 构建

在名为 `Ubuntu` 的 WSL 中执行：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh

# 从 0x08000000 启动，适合直接使用 ST-Link
bash scripts/build_firmware.sh mellow-fly-d5

# 从 0x08002000 启动，适合配合 8 KiB Katapult
bash scripts/build_firmware.sh mellow-fly-d5-katapult
bash scripts/build_bootloader.sh stm32f072_mellow_fly_d5_dual
bash scripts/build_factory_images.sh mellow-fly-d5
```

主要输出：

- `out/remotebsp-stm32f072-fly-d5.hex`：独立 APP；
- `out/remotebsp-stm32f072-fly-d5-katapult.bin`：在线升级 APP；
- `out/katapult-stm32f072_mellow_fly_d5_dual.bin`：CAN/USB 双模式 Bootloader；
- `out/remotebsp-stm32f072-fly-d5-katapult-dual-factory.bin`：首次整片镜像。

F072 Cortex-M0 没有 VTOR。链接脚本保留 SRAM 前 192 字节，APP 启动时复制
48 个中断向量并把地址 0 重映射到 SRAM，因此 8 KiB 偏移 APP 的中断入口仍然
正确。

## 首次烧录与接线

使用 ST-Link 连接板上的 `SWDIO`、`SWCLK`、`NRST`、`GND` 和 `3.3V/VTref`，
不要由两个电源同时给板卡供电。首次烧录双模式完整镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32f072-fly-d5-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

CAN 总线连接 `CANH`、`CANL` 和参考地。总线只在物理干线两端各放一个 120 Ω
终端；断电测量 CANH-CANL 应约为 60 Ω。主机 SocketCAN 与本固件都配置为
1 Mbit/s。

## 升级入口

- 正常升级：`remote-cli --node N bootloader-enter`，进入 CAN Katapult；
- CAN 尚可但需要 USB：`bootloader-enter-usb`，进入 USB Katapult；
- CAN 完全失效：快速双击板上的 RESET，进入 USB Katapult；
- 正常单次复位：启动 RemoteBSP APP。

上述逻辑和镜像已经完成交叉编译、链接、大小及工厂打包检查。Classical CAN 1M
收发与 D0 TMC2209 单线 UART 已在实体 FLY-D5 验证；USB 枚举、双击复位和在线
写入仍须继续验收。

## 资料依据

- [STMicroelectronics STM32F072RB 产品页](https://www.st.com/en/microcontrollers-microprocessors/stm32f072rb.html)
- [Mellow FLY-D5 主板简介](https://mellow.klipper.cn/docs/ProductDoc/MainBoard/fly-d/fly-d5/)
- [Mellow FLY-D5 USB 桥接 CAN 固件配置](https://mellow.klipper.cn/docs/ProductDoc/MainBoard/fly-d/fly-d5/firmware/can/)
- [Mellow FLY-D5 Klipper 引脚参考](https://mellow.klipper.cn/docs/ProductDoc/MainBoard/fly-d/fly-d5/cfg/)
