# STM32 固件编译与烧录

## 当前可运行范围

当前固件已经为以下目标生成可烧录文件：

| 目标 | 总线模式 | 已接通的远程功能 |
|---|---|---|
| STM32F103CBT6 / WeAct BluePill Plus | Classical CAN、GPIO、USART1/2/3、双模式Katapult；三路115200全双工并发实板验证通过；PA6 PWM、PA8 DMA定时位流和五轴/TMC后端已交叉编译 |
| STM32F072RBT6 / Mellow FLY-D5 | Classical CAN 1 Mbit/s、GPIO、五轴运动与五路TMC2209已实板验证；PA6 PWM和PA8 DMA定时位流已交叉编译；双模式Katapult切换待验收 |
| STM32G431CBU6 / WeAct STM32G431CBU6 Core | CAN-FD及USART1/2/3三路115200全双工并发已实测；另有互斥的USB Vendor Bulk APP；PC6 PWM、PA8 DMA定时位流和PC13 GPIO |

STM32F072的硬件UART 0接到USART1；STM32F103与STM32G431依次提供USART1/2/3三路硬件UART。
所有端口均使用各自独立的中断驱动RX/TX环形缓冲。Studio/Kconfig可选择USART1
的PA9/PA10或PB6/PB7端点；F103的USART2固定PA2/PA3，USART3固定PB10/PB11。
RS-485方向引脚暂未接入实体后端。
FLY-D5未默认公开通用硬件UART。G431普通板卡预设默认公开三路硬件UART；单轴
TMC2209验收预设为避免复用既有PA2/PA3接线，仍只报告TMC专用单线逻辑UART。
F103 的 Katapult 跳转、CAN 在线升级和 USB Bootloader 枚举已经分别通过实体板
基线验证。当前 F103/G431 已统一生成双模式 Katapult：APP 命令进入 CAN；
BluePill 复位时按住 PA0、WeAct G431 复位时按住 PC13 进入 USB。双模式镜像
已交叉编译，G431 的接口切换仍待实板验证。
正式 APP 默认只链接 CAN/CAN-FD，不提供 USB CDC 调试接口。G431 另提供主链路
互斥的 USB Vendor Bulk APP，已交叉编译但待实体枚举与压力测试；Katapult USB
仍由独立 Bootloader 在应急升级阶段初始化，并使用不同 PID。

通用波形模块由`CONFIG_REMOTEBSP_PWM`和`CONFIG_REMOTEBSP_TIMED_BITSTREAM`
独立裁剪。F072/F103基础预设使用PA6/TIM3_CH1和PA8/TIM1_CH1+DMA1_Channel2；
G431使用PC6/TIM3_CH1和PA8/TIM1_CH1+DMA1_Channel1/DMAMUX。引脚选择会过滤
已被运动、CAN、SWD、USB或板载固定功能占用的IO。三种后端均已交叉编译，
但新远程PWM命令、DMA位流和WS2812实体波形仍待验收。BluePill Plus的PB2软件
呼吸灯保留为独立板级自检功能，不等同于通用PWM资源。

BluePill Plus专用配置`stm32f103_weact_bluepill_plus_defconfig`把CAN重映射到
PB8/PB9，避开板载USB对PA11/PA12的占用；默认启用UART 0～2三路端口。

## Ubuntu 依赖

在名为 `Ubuntu` 的 WSL 发行版中安装：

```sh
sudo apt update
sudo apt install -y \
    cmake ninja-build git python3 python3-pip \
    gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
python3 -m pip install --user kconfiglib
```

准备 ST 官方依赖：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh
```

脚本下载固定版本的 CMSIS Core、对应 MCU 的 CMSIS Device、HAL 和 G431 USB APP
需要的 ST USB Device 中间件，不下载板级示例工程。Katapult 依赖由独立脚本管理。

## 使用默认配置编译

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/build_firmware.sh all
```

构建脚本默认使用32个并行任务。需要临时调整时使用
`RBSP_BUILD_JOBS=16 bash scripts/build_firmware.sh all`。

也可以只编译一个目标：

```sh
bash scripts/build_firmware.sh f103
bash scripts/build_firmware.sh weact-bluepill-plus
bash scripts/build_firmware.sh f072
bash scripts/build_firmware.sh mellow-fly-d5
bash scripts/build_firmware.sh g431
bash scripts/build_firmware.sh weact-stm32g431cbu6-core
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-usb
bash scripts/build_firmware.sh weact-bluepill-plus-motion
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-motion
```

带`motion`的目标使用`firmware/tests/configs`中的临时验收配置，不是正式出厂预设。
Studio生成配置的测试会另外对三块正式板卡执行真实交叉编译。

输出位于 `firmware/out`，包括 ELF、Intel HEX、裸 BIN 和链接 MAP。

带 Katapult 的编译、首次量产镜像及 CAN/USB 升级方法见
[Katapult 双模式升级与应急恢复](bootloader-upgrade.md)。

## 使用 menuconfig

先选择一个完整的初始配置，再进入菜单：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
cp configs/stm32f103cbt6_defconfig .config
python3 scripts/menuconfig.py
```

FLY-D5 或 WeAct G431 Core 将第一条命令中的文件分别改为
`configs/stm32f072_mellow_fly_d5_defconfig` 或
`configs/stm32g431_weact_core_defconfig`。保存后编译：

```sh
cmake -S . -B build-custom -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DRBSP_CONFIG="$PWD/.config"
cmake --build build-custom --parallel 32
```

菜单按职责分为“目标硬件”“CAN/CAN-FD”“远程协议与内存预算”“通用硬件资源”
“智能步进运动”和“启动与升级”。普通用户只需选择 MCU/板型、晶振、CAN、功能
模块和静态容量。“远程协议与内存预算”不再是空菜单：可在菜单内单独开启编辑，
也可打开顶层“显示专家级容量与时序设置”，查看协议缓存、运动步频和 compare
迟到保护等需要实板验证的参数。系统时钟、板卡类型和 APP Flash 偏移均由其他
选择自动派生，不能手工填写。

兼容期运动槽的 STEP/DIR/EN/TMC/限位已经由整数编码改为 `choice` 选择项。名称
按 `GPIOC / PC13` 形式分组标注；SWD、当前 CAN/UART/Katapult USB、板载呼吸灯
以及前面字段已经选过的引脚不会出现在后续列表中。生成文件
`Kconfig.motion_pins.generated` 与 GUI 引脚目录来自同一脚本，修改引脚源后运行：

```sh
python3 scripts/generate_pin_choices.py
```

每个槽的“EN 来源”可选择独立引脚，或与任意前置槽共用 EN。共用时不再显示独立
EN 引脚和极性选项，而是自动继承来源槽。固件不是简单重复写同一 GPIO，而是保存
各逻辑轴的使能请求并做组内 OR：任一轴仍需要使能时保持物理 EN，最后一个轴释放
后才关闭。共享 EN 无法让组内空闲电机单独断电，这是硬件共线本身的限制。

### 上电IO安全状态

`menuconfig`中的“上电 IO 安全状态”可以填写五类引脚列表：输出低、输出高、
浮空输入、上拉输入和下拉输入。列表使用`PA0,PB2,PC13`格式，空列表表示不主动
修改该类引脚。

固件在系统时钟建立后、CAN/UART和Remote Core启动前应用这些状态。输出引脚会
先写GPIO输出锁存器，再切换为推挽输出，避免使能、阀门或片选线上出现短毛刺。
同一引脚重复配置、引脚格式错误、封装上不存在的引脚，或与SWD/CAN/USB/UART
保留引脚冲突，都会让固件停在安全故障状态。

这些选项用于不可晚于通信初始化的板级安全默认值。步进轴、TMC和普通远程资源
的可修改接线仍由后续NVM资源清单管理；两者冲突时配置器必须在下发前拒绝。

目前的位时序是：

- F072通用配置：HSI48/PCLK 48 MHz，Classical CAN 500 kbit/s，16 TQ，
  分频系数6，采样点87.5%；默认使用PA11/PA12。
- FLY-D5：板载 8MHz HSE 经 PLL 到 PCLK 48 MHz，Classical CAN 1 Mbit/s，16 TQ，
  分频系数 3，采样点 87.5%。
- F103 通用预设：外部 8 MHz HSE 经 PLL 到 72 MHz，CAN 外设时钟 36 MHz，
  500 kbit/s，18 TQ，采样点约 88.9%。menuconfig 也可选择内部 HSI8，届时
  SYSCLK 为 64 MHz、CAN 时钟为 32 MHz，并使用 16 TQ；内部 RC 只建议作为
  没有 HSE 时的退路。
- F103 Bluepill 实机配置：CAN 外设时钟 36 MHz，1 Mbit/s，18 TQ，
  分频系数 2，采样点约 88.9%。
- G431：FDCAN 内核时钟 170 MHz，仲裁段 500 kbit/s，默认数据段 1 Mbit/s，
  两段采样点均约 82.4%。

WeAct BluePill Plus 和 WeAct G431 Core 的 32.768 kHz LSE 是低速守时资源，不是
系统主时钟。当前 APP 会在 menuconfig、GUI 和运行时引脚检查中保留 PC14/PC15，
但在 RTC/掉线守时模块实现前不会启动 LSE。FLY-D5 只声明板载 8 MHz HSE。

配置成其他速率时，编译期会检查是否能被当前时序精确生成；不能精确生成就会
停止编译，而不是悄悄使用错误速率。

`STM32F072/F103 UART 0（USART1）引脚`菜单可以在以下两组引脚间选择：

- PA9(TX)/PA10(RX)，默认值。
- PB6(TX)/PB7(RX)，启用 USART1 重映射。

STM32F072RBT6 MCU 层同样提供这两组 USART1 引脚：PA9/PA10 使用 AF1，PB6/PB7 使用 AF0。
F072还允许CAN在PA11/PA12与PB8/PB9之间选择。数据手册列出的第三组
PD0/PD1不在RBT6的LQFP64封装上，因此不会显示为可选项。FLY-D5板型固定选择
PB8/PB9。由于尚未确认 FLY-D5 对外安全公开的硬件 USART 引脚，该板预设把硬件
UART容量设为0，五个TMC端口继续使用逻辑UART对象0～4。F103的UART 1固定为
USART2 PA2/PA3，UART 2固定为USART3 PB10/PB11；LQFP48没有引出这些外设的有效
重映射组合，因此GUI不会提供无效候选。通用F072和F103可以分别编译硬件UART与
TMC后端；F103五轴测试预设为节省引脚只保留USART1对象0，TMC为
对象 1～5。TMC 端口只执行固定 40000 bit/s 原始字节事务，不能作为 Modbus 或
通用软串口使用。

被 UART 选中的引脚会从通用远程 GPIO 池中保留，不能再通过 `GPIO_CREATE`
重新配置。

## 基础配置与板卡配置

三种 MCU 都采用“MCU 通用层 + 板型覆盖层”，没有为单个试验接线复制 `main.c`。
当前保留的预设分为两类：

- MCU 基础：`stm32f072rbt6_defconfig`、`stm32f103cbt6_defconfig`、
  `stm32g431cbu6_defconfig`；只假定芯片本身，不启用板载 LED、按键或运动接线。
- 具体板卡：文件名包含 `mellow_fly_d5`、`weact_bluepill_plus` 或
  `weact_core`；可再带 `_motion_...` 或 `_katapult` 后缀说明用途。

接入同 MCU 的另一块板卡时：

1. 选择对应的 `CONFIG_BOARD_STM32...=y`，不要复制 Remote Core 或 HAL BSP。
2. 如果现有CAN/UART引脚组合适用，只需新增`configs/<板名>_defconfig`。
3. 板载 LED、按键、收发器 STB、USB 恢复口占用或禁止复用引脚通过板型选项描述。
4. 在`boards/`增加机器可读资源描述，记录公开资源和内部占用关系。
5. 如果需要Katapult，再增加与该板恢复方式匹配的独立Bootloader配置。

删除的 PB8/PB9、Classical CAN、二轴/五轴等早期试验预设都可以通过复制最接近
的基础或板卡配置，再在 `menuconfig` 中设置得到；不再为每次临时接线永久增加
仓库文件。

`智能步进运动（可选）`菜单当前提供：

- `CONFIG_REMOTEBSP_MOTION`：是否把运动队列核心编译进固件，默认关闭。
- `CONFIG_REMOTEBSP_TMC2209_UART`：是否编译固定40000 bit/s的TMC2209单线
  事务后端，可与F072/F103已裁剪启用的普通硬件UART同时存在。
- `CONFIG_TMC2209_UART_PORT_CAPACITY`：TMC 单线端口的编译期静态容量；配置生成器
  会拒绝槽号超出容量的组合。
- `CONFIG_MOTION_MAX_AXES`：本板最大轴数，F103 默认保守设为2、G431默认5；两者均可显式配置到当前板级后端的五槽上限。
- `CONFIG_MOTION_QUEUE_DEPTH`：固定容量运动段队列，默认F103为8、G431为32。
- `CONFIG_MOTION_MIN_LEAD_TIME_US`：最小排程提前量。
- `CONFIG_MOTION_MAX_STEP_RATE_HZ`与`CONFIG_MOTION_MAX_TOTAL_STEP_RATE_HZ`：
  单轴和整板总STEP频率准入预算。
- `CONFIG_MOTION_STEP_PULSE_WIDTH_US`、`CONFIG_MOTION_MIN_STEP_LOW_US`和
  `CONFIG_MOTION_DIRECTION_SETUP_US`：驱动器时序约束。
- `CONFIG_MOTION_COMPARE_MIN_LEAD_US`与`CONFIG_MOTION_MAX_COMPARE_LATENESS_US`：
  定时器compare写入余量和迟到安全停机预算。

除功能开关、最大轴容量和 TMC 端口容量外，运动队列、步频和定时器保护预算默认
隐藏在专家模式中。五组 `MOTION_SLOT...` 由Studio工程生成到Kconfig，用于构建
板卡专用固件；改变槽位接线必须重新构建、烧录并重启。

启用`CONFIG_REMOTEBSP_DEVICE_PARAMS`时，F103在Flash末尾保留2 KiB，F072/G431
保留4 KiB，用于SN、UUID、制造信息和ADC校准。链接器和Katapult APP写入上限都会
避开该区域，因此正常在线升级不会清除参数。参数区不保存运动或IO映射。

只有启用运动模块时，`motion.c`和`rbsp_core_t`中的轴/队列存储才参与编译。
因此普通GPIO/UART工具板不会承担运动功能的Flash和RAM成本。上述默认值只是
初始预算，最终可选上限必须经过实体板持续步频和最坏中断延迟测试。当前C队列、
远程运动命令和F072/F103/G431的TIM2_CH1 compare STEP后端均已接入并交叉编译；
F072旧固定tick版本有实板运动记录，新compare版本及F103/G431仍需实体压力验收。

## 使用 ST-Link 烧录

如果已安装 STM32CubeProgrammer，F103 和 G431 使用相同命令：

```powershell
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32f103cbt6.hex -v -rst
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32f103-bluepill-pb8-pb9.hex -v -rst
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32f072-fly-d5.hex -v -rst
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32g431cbu6.hex -v -rst
```

烧录前连接 `SWDIO/PA13`、`SWCLK/PA14`、`NRST`、`GND` 和目标电平
`VTref/3.3V`。

## 使用 DAPLink/CMSIS-DAP 烧录

OpenOCD 示例：

```sh
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg \
  -c "program firmware/out/remotebsp-stm32f103cbt6.elf verify reset exit"

openocd -f interface/cmsis-dap.cfg -f target/stm32g4x.cfg \
  -c "program firmware/out/remotebsp-stm32g431cbu6.elf verify reset exit"
```

## GPIO 编号

远程 GPIO 使用 `端口序号 * 16 + 引脚号`：

| GPIO | 编号 | GPIO | 编号 |
|---|---:|---|---:|
| PA0 | 0 | PA15 | 15 |
| PB0 | 16 | PB15 | 31 |
| PC13 | 45 | PD0 | 48 |

## STM32F103实体UART

三路端口分别使用256字节RX和256字节TX存储区。环形缓冲保留一个空槽，
所以每次`UART_WRITE`最多原子接收255字节；空间不足时整段返回
`RESOURCE_FAILED`，不会发送半条报文。`UART_READ` 为保证响应可去重重放，
单次最多取 231 字节，更长数据可以连续读取。

| RemoteBSP端口 | STM32外设 | TX | RX | 最高配置波特率 |
|---:|---|---|---|---:|
| 0 | USART1 | PA9（可选PB6重映射） | PA10（可选PB7重映射） | 4,500,000 |
| 1 | USART2 | PA2 | PA3 | 2,250,000 |
| 2 | USART3 | PB10 | PB11 | 2,250,000 |

ST-Link 虚拟串口接线：

| ST-Link VCP | STM32F103 |
|---|---|
| TX | PA10 / USART1_RX |
| RX | PA9 / USART1_TX |
| GND | GND |

创建并测试 UART：

```sh
./build-wsl/remote-cli --node 1 get-capability
./build-wsl/remote-cli --node 1 uart-create 0 115200 8 none 1
./build-wsl/remote-cli --node 1 uart-create 1 115200 8 none 1
./build-wsl/remote-cli --node 1 uart-create 2 115200 8 none 1
./build-wsl/remote-cli --node 1 uart-write 1 hello-uart1
./build-wsl/remote-cli --node 1 uart-write 2 hello-uart2
./build-wsl/remote-cli --node 1 uart-write 3 hello-uart3
./build-wsl/remote-cli --node 1 uart-write-hex 1 010300000002c40b
./build-wsl/remote-cli --node 1 uart-read 1 231
```

2026-07-28 使用 ST-Link 的 COM18 实测：

- CAN→USART1 和 USART1→CAN 文本双向一致。
- 包含 `00` 的 `00..FE` 共 255 字节二进制序列双向逐字节一致。
- 256 字节 TX 写入被完整拒绝，COM18 收到 0 字节，节点和 CAN 心跳保持正常。

2026-08-17三路固件实测：UART对象0/1/2同时创建成功；USART1使用ST-Link VCP，
USART2/3使用CH348的两路接口。115200 8N1下三路同时全双工，每个方向、每路
1024字节，共6144字节逐字节一致，`dropped_bytes=0`、`lost_events=0`，CAN保持
ERROR-ACTIVE且收发错误、丢包和Bus-Off均为0。过量并发启动独立CLI请求时，
`toolbusd`会按预算主动拒绝部分交互请求；顺序限速后全部通过，这是流量准入保护，
不是UART数据丢失。

CH348通道刚由Windows打开并设置波特率后立即首发时，曾在A/B两路各观察到一个
首字节缺失；端口打开后等待500 ms再进行相位对齐递增序列测试，三路双向全部
一致。因此该等待属于测试适配器初始化要求，不计为STM32 UART持续运行丢字。

固件会拒绝 CAN/FDCAN 引脚和 SWD 引脚。F103 还会按照 LQFP48 的实际键合
管脚拒绝不存在的 GPIO；FLY-D5 还会保留 PA11/PA12 USB，并按照 LQFP64
约束仅允许 PD2；WeAct G431 QFN48 开放实际引出的 GPIOA/GPIOB 以及
PC4/PC6/PC10/PC11/PC13，保留晶振使用的 PF0/PF1 和 PC14/PC15。

FLY-D5 的板级引脚、双模式升级和数字孪生资源说明见
[Mellow FLY-D5 支持说明](mellow-fly-d5.md)。

## 首次上板检查顺序

1. 只连接 SWD，确认能够读出芯片 ID、烧录并复位。
2. 不连接总线时检查 TXD 静态电平和 STB 是否为低。
3. 接一块 USB-CAN，先只接一个节点并在总线两端各放 120Ω。
4. F103 通用配置使用 Classical CAN 500 kbit/s；已验证的 Bluepill 配置使用
   1 Mbit/s。G431 配置 CAN-FD、仲裁段 500 kbit/s、默认数据段 1 Mbit/s，并启用
   BRS。
5. 启动 `toolbusd`，确认节点在 2 秒内被发现且每 500 ms 更新心跳。
6. 执行 `ping`、`get-info`，最后再创建和读写一个未保留的 GPIO。

2026-08-01 的 WeAct STM32G431CBU6 + TJA1051/3 + CANable2.5 实测中，
PB8/PB9 上的 CAN-FD 以 500 kbit/s 仲裁段、1 Mbit/s 数据段和 BRS 完成节点
发现、分配、持续心跳、PING、GET_INFO、GET_CAPABILITY 及 PA5 GPIO 高低电平
读写，双方实时 TEC/REC 均为 0。相同飞线环境下 2 Mbit/s 数据相位会触发
Bus-Off，因此 2 Mbit/s 保留为 menuconfig 可选高速档，需在更短支线和更好
信号完整性条件下重新验收。

G431 的 PB8 同时是 FDCAN_RX 和 BOOT0。使用 PB8/PB9 CAN 时，收发器的空闲
高电平可能让 MCU 复位进入系统 ROM；量产烧录必须把 Option Bytes 配置为
BOOT0 来自 nBOOT0 选项且 nBOOT0=1（忽略 PB8 启动电平）。应急 USB 升级由
双模式 Katapult 的 PC13/命令入口承担。

## CANable2.5 Candlelight/gs_usb 固件（当前推荐）

当前使用的主机适配器是 **CANable2 硬件刷入 CANable2.5 的
Candlelight/`gs_usb` 固件**。该固件由 Linux `gs_usb` 驱动直接注册为
SocketCAN `canX` 接口，不经过 USB CDC、`slcand` 或 LAWICEL ASCII 封装；因此
它是本项目 Classical CAN 与 CAN-FD 实机测试的推荐路径。

先确认接口名称和控制器能力：

```sh
ip -details link show
```

Classical CAN 1 Mbit/s 的典型启动方式为：

```sh
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 1000000 sample-point 0.875 berr-reporting on
sudo ip link set can0 txqueuelen 1024
sudo ip link set can0 up
```

若 `ip -details link show can0` 显示控制器支持 FD，再以 G431 已验证的
500 kbit/s 仲裁段、1 Mbit/s 数据段和 BRS 启动：

```sh
sudo ip link set can0 down
sudo ip link set can0 type can bitrate 500000 dbitrate 1000000 fd on \
  sample-point 0.875 dsample-point 0.800 berr-reporting on
sudo ip link set can0 txqueuelen 1024
sudo ip link set can0 up
```

`toolbusd` 直接打开该 `can0`；CAN-FD 使用 `fd` 模式：

```sh
./build-wsl/toolbusd/toolbusd can0 fd /tmp/toolbusd.sock \
  --arbitration-bitrate 500000 --data-bitrate 1000000 \
  --runtime-operation-ledger-dir /var/lib/remotebsp/operation-ledger
```

`--runtime-operation-ledger-dir` 为当前 `toolbusd` 强制参数。正式目录应由运行 daemon 的
专用用户独占且跨重启保留；目录属主、类型或权限不安全会拒绝 daemon 启动，安全目录内的
账本内容损坏则保持只读诊断并让 Runtime 写入口失败关闭。该主机侧要求不改变任何 STM32
烧录布局。

单节点进入 USB Katapult 后，总线没有其他节点确认周期发现帧时，`gs_usb`
适配器可能因持续 ACK 错误进入 ERROR-PASSIVE。USB 升级结束后重新启动接口即可：

```sh
sudo ip link set can0 down
sudo ip link set can0 up
```

## CANable2 原厂 SLCAN 固件（旧固件兼容）

CANable2 重新连接后可能在 `ttyACM0`、`ttyACM1` 等编号之间变化，因此实机
测试使用 `/dev/serial/by-id` 稳定路径。仓库脚本会自动查找唯一的 CANable2，
默认以 1 Mbit/s 建立 `can0`，把 SLCAN USB CDC 端口参数设为 2 Mbaud，并把
发送队列从默认的 10 调整为 1024：

```sh
cd /mnt/d/Documents/RemoteBSP
sudo bash tests/hardware/start_canable2_slcan.sh can0 8 1024 2000000
```

第二个参数是 LAWICEL 速率预设（`6=500 kbit/s`、`8=1 Mbit/s`），第四个参数
是 SLCAN 串口波特率。1 Mbit/s 对布线长度、支线长度和终端质量更敏感；现场
拓扑不能满足要求时应退回预设 6。

经典 CAN 的协议包会分成多个 8 字节帧。`NODE_ASSIGN` 当前需要 15 个分片；
如果保留 slcan 默认的 `txqueuelen=10`，SocketCAN 写入虽然可能返回成功，尾部
分片仍会在队列层被丢弃，节点会长期停在 `ready=0`。协议最大包为 2048 字节，
经典 CAN 最多需要约 683 个分片，因此实机脚本默认使用 1024 帧队列。
F103 固件也按完整的 2048 字节协议上限构建；当前 Bluepill 配置占用
10808 字节 SRAM（52.77%），仍保留约 9.4 KiB 给中断栈和后续外设缓冲。

启动守护进程并验证实体节点：

```sh
./build-wsl/toolbusd/toolbusd can0 classical /tmp/toolbusd.sock \
  --runtime-operation-ledger-dir /var/lib/remotebsp/operation-ledger

./build-wsl/remote-cli node-list
./build-wsl/remote-cli --node 1 ping hello
./build-wsl/remote-cli --node 1 get-info
./build-wsl/remote-cli --node 1 get-capability
```

实体链路压力测试可以同时覆盖顺序 PING、GPIO 往返、并发客户端和大包分片：

```sh
bash tests/hardware/physical_can_stress.sh \
  /tmp/toolbusd.sock 1 200 50 0 4 50 2023
```

最后一个参数是大载荷 PING 的字节数。协议包上限为 2048 字节，扣除 24 字节
协议头和 1 字节响应状态码后，PING 可成功回显的最大载荷为 2023 字节；
2024 字节会在主机 API 层立即拒绝，不会向总线发送一个必然无法构造响应的
请求。

2026-07-28 的 Bluepill + TJA1051/3 + CANable2 实测中，2023 字节载荷连续
10/10 次成功，单次往返约 425～451 ms；8 个并发客户端各执行 100 次 PING，
共 800 次请求无失败。测试结束后 can0 的发送错误、接收错误、丢包、仲裁丢失
和 bus-off 计数均为 0，普通 PING 仍正常。

如果需要绕过 SocketCAN 检查 CANable2 原厂固件、发送测试帧并读取其软件错误
寄存器，应先停止 `toolbusd` 和 `slcand`，再执行：

```sh
sudo bash tests/hardware/canable2_raw_probe.sh \
  /dev/serial/by-id/usb-Openlight_Labs_CANable2_* 32 8 2000000
```

2026-07-29 将同一套 Bluepill + TJA1051/3 + CANable2 提升到 1 Mbit/s，
SLCAN 端口参数提高到 2 Mbaud。顺序 PING、GPIO 往返、并发客户端和 2023
字节最大载荷均通过；短 PING 为 12～16 ms，2023 字节往返为 364 ms，
SocketCAN 统计保持 0 错误、0 丢包、0 bus-off。最大载荷提升没有达到线速
翻倍，主要限制来自原厂 SLCAN 的 ASCII 封装和 USB CDC 路径。
