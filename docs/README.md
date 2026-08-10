# Remote BSP 文档导航与项目状态

> 最后整理：2026-08-10
>
> 本文是项目状态入口。功能状态以“已实现、已编译、已实测、仅设计”区分。

## 文档导航

- [系统分层与通信设计](architecture.md)
- [智能运动、资源模型与路线图](intelligent-motion-resources.md)
- [板卡资源清单与数字孪生](board-manifest-and-digital-twin.md)
- [STM32 构建、烧录与验证](stm32-build-and-flash.md)
- [Katapult 双模式升级与应急恢复](bootloader-upgrade.md)
- [USB Vendor Bulk 传输](usb-transport.md)
- [Mellow FLY-D5 板卡说明](mellow-fly-d5.md)
- [WeAct BluePill Plus 板卡说明](weact-bluepill-plus.md)
- [WeAct STM32G431CBU6 Core 板卡说明](weact-stm32g431cbu6-core.md)
- [客户端 API](libremotebsp-api.md)
- [使用场景与需求](use-cases-and-requirements.md)

## 一句话说明

Remote BSP 让 Linux 主机通过 CAN/CAN-FD 统一管理多块不同 MCU、不同资源的
工具板，并像使用本地资源一样操作远端 GPIO、UART、PWM、定时位流和智能运动资源。
Linux 处理设备协议与运动规划，MCU 只执行通用、确定性的硬件操作。

## 系统使用场景

- 主机：Linux 上位机加本地工具板。
- 从机：一块或多块远端工具板。
- 工具板可以使用不同 MCU、资源数量和板级扩展芯片。
- 所有节点通过 CAN 或 CAN-FD 接入。
- 单个节点、UART 或其他资源故障不能阻塞无关节点和资源。
- Linux 应用不直接访问 SocketCAN，而是通过 `libremotebsp` 和 `toolbusd`。

典型数据路径：

```mermaid
flowchart LR
    A["应用 / remote-cli"] --> B["libremotebsp"]
    B --> C["Unix Domain Socket"]
    C --> D["toolbusd"]
    D --> E["协议、请求、分片"]
    E --> F["LinkTransport"]
    F --> G["SocketCAN / libusb"]
    G --> G2["CAN / CAN-FD / USB Bulk"]
    G2 --> H["Mock MCU / STM32 Remote Core"]
    H --> I["GPIO / UART / PWM / 定时位流 / 智能资源"]
```

## 设计边界

MCU 固件允许包含：

- GPIO、UART、SPI、定时器等 MCU 外设适配器。
- SPI 转 UART 等板级资源适配器。
- 定时 STEP 执行、本地限位联锁和通用事务执行。

MCU 固件禁止包含：

- Modbus、GPS、传感器、阀门和厂商业务协议。
- TMC 型号相关寄存器语义和运动学规划。
- Linux 应用逻辑。

协议层不知道 CAN、CAN-FD 或 SocketCAN；传输层不知道 GPIO、UART 和运动业务。

## 当前实现状态

| 模块 | 状态 | 说明 |
|---|---|---|
| 远程包协议 v1 | 已实现、已测试 | 24字节固定头、小端线序、CRC-32、严格长度和版本检查 |
| 分片与重组 | 已实现、已测试 | Classical CAN 8字节、CAN-FD 64字节、最大包2048字节 |
| 通用链路传输 | 已实现、已测试 | `LinkTransport`统一逻辑帧；SocketCAN和Mock USB完整端到端 |
| USB Vendor Bulk | 第一阶段已实现、已测试 | libusb、Mock全链路和G431 Device后端已完成；实体G431待验收 |
| 节点发现 | 已实现、已测试 | UUID发现、节点分配、协议版本协商 |
| 心跳与离线 | 已实现、已测试 | 500 ms心跳、2 s离线判断 |
| 请求管理 | 已实现、已测试 | 请求ID、750 ms默认超时、重试、响应匹配 |
| CAN流量准入 | 第一阶段已实现、已测试 | 六类业务、Classical/CAN-FD线时间估算、BRS、令牌桶和统计 |
| 副作用去重 | 已实现、已测试 | 重试沿用请求ID，MCU返回缓存响应，不重复执行写操作 |
| 本地IPC | 已实现、已测试 | Unix Domain Socket，多客户端并发 |
| C++客户端 | 已实现、已测试 | 节点、GPIO、UART、PWM、定时位流/WS2812、运动、资源合同/租约、事件及Bootloader API |
| `remote-cli` | 已实现、已测试 | 信息、资源、合同/租约、GPIO、UART、PWM、定时位流/WS2812和升级入口命令 |
| Mock MCU | 已实现、已测试 | 版本化板卡描述、Classical CAN/CAN-FD、多节点、GPIO、8路UART、2路PWM、1路定时位流、故障注入和租约安全释放 |
| STM32F103 | 已实现、已实测 | Classical CAN、GPIO、USART1、CAN/USB Katapult |
| STM32G431 | CAN-FD APP 已实测，USB APP 已交叉编译 | CAN-FD基础功能已通过；USB Vendor Bulk及新通用波形待实体验收 |
| 通用PWM与定时位流 | 第一阶段已实现、已测试 | 协议、Linux API/CLI、Mock、GUI草案和F072/F103/G431后端；实体波形待验收 |
| SPI/I2C/ADC/Timer/Storage | 尚未实现 | 仅保留资源类型和后续设计位置 |
| 智能运动资源 | 第一阶段已实现、已测试 | Mock 多轴、STM32 TIM2 compare STEP 调度、队列、限位安全停机和状态遥测；跨板同步与运行时配置待实现 |
| 图形资源配置器 | 第一版草案已实现 | 引脚冲突检查、GPIO/运动/PWM/WS2812配置草案、导出和Mock数字孪生；部署闭环待实现 |

## 已实现协议能力

协议版本为1，完整包最大2048字节，头部字段包括：

- 协议版本、消息类型和命令。
- 会话ID、请求ID和对象ID。
- 载荷长度、标志和CRC。

当前命令族：

```text
DISCOVERY_REQUEST / DISCOVERY_RESPONSE / NODE_ASSIGN / HEARTBEAT
GET_INFO / GET_CAPABILITY / PING
RESOURCE_ENUM / RESOURCE_DESCRIBE / RESOURCE_STATUS / RESOURCE_RESET
RESOURCE_CONTRACT / RESOURCE_ACQUIRE / RESOURCE_RENEW
RESOURCE_RELEASE / RESOURCE_LEASE_STATUS
GPIO_CREATE / GPIO_READ / GPIO_WRITE
UART_CREATE / UART_READ / UART_WRITE / UART_RX_EVENT
PWM_CREATE / PWM_WRITE / PWM_STOP
TIMED_BITSTREAM_CREATE / TIMED_BITSTREAM_WRITE / TIMED_BITSTREAM_ABORT
BOOTLOADER_ENTER / BOOTLOADER_ENTER_USB
```

广播发现使用CAN ID `0x700`。节点N分配后：

- 请求：`0x600 + N`
- 响应：`0x580 + N`
- 事件与心跳：`0x500 + N`

Classical CAN每片只有3字节有效协议数据；CAN-FD每片最多59字节。分片层支持
超时、重复检测、乱序重组、冲突拒绝和CAN-FD DLC补零检查。

## 当前远程资源

### GPIO

- 创建输入或输出对象。
- 原子读写。
- 校验芯片实际引脚和保留资源。
- 重复`GPIO_WRITE`不会再次执行底层写操作。

GPIO编号采用`端口序号 × 16 + 引脚号`，例如PA0为0、PB2为18、PC13为45。

### UART

- 配置波特率、数据位、停止位和奇偶校验。
- 原始二进制读写，不解析Modbus、GPS或厂商协议。
- 轮询读取与异步`UART_RX_EVENT`。
- 每端口独立缓冲、健康状态、溢出计数和资源复位。
- F103 UART 0使用USART1中断和RX/TX环形缓冲。

Mock MCU从`boards/mock-generic-v1.json`加载16路GPIO和8路UART；UART 0～3
标记为原生，UART 4～7标记为扩展。扩展UART背后的内部SPI作为保留资源记录，
不会重复暴露。板型对同型号多实例保持不变，实例身份由UUID区分。

### PWM与通用定时位流

- PWM对象配置频率、0～10000万分比占空比和有效极性，支持运行中更新占空比与安全停止。
- 定时位流对象接收0/1高低电平时间、复位时间和打包位流，MCU不识别具体设备型号。
- Linux的WS2812包装器完成RGB→GRB和逐位打包；动画、亮度和像素布局不进入固件。
- Mock可显示PWM状态，并把标准WS2812时序解码为逻辑像素。
- F072/F103使用PA6/TIM3_CH1输出PWM、PA8/TIM1_CH1+DMA输出位流；G431使用
  PC6/TIM3_CH1与PA8/TIM1_CH1+DMA。当前均只完成交叉编译，新后端待示波器和灯带验收。

## 当前硬件基线

### STM32F103CBT6 / WeAct BluePill Plus

| 功能 | 当前推荐引脚或参数 |
|---|---|
| CAN RX/TX | PB8/PB9，Classical CAN 1 Mbit/s实测 |
| USB D-/D+ | PA11/PA12，仅用于USB Katapult |
| USART1 TX/RX | PA9/PA10 |
| 用户按键 | PA0，内部下拉、按下为高 |
| 指示灯 | PB2，高电平点亮 |
| SWD | PA13/PA14，建议同时连接NRST |
| 收发器 | 实测TJA1051/3 |

F103的USB与bxCAN共享专用SRAM，因此APP只运行CAN；Bootloader根据进入原因
二选一运行CAN或USB。

### STM32F072RBT6 / Mellow FLY-D5

| 功能 | 当前配置 |
|---|---|
| CAN RX/TX | PB8/PB9，Classical CAN 1 Mbit/s |
| USB D-/D+ | PA11/PA12，仅用于USB Katapult |
| 系统时钟 | 外部 8 MHz HSE，经 PLL 到 48 MHz |
| SWD | PA13/PA14，建议同时连接NRST |
| 板载运动接口 | 5路 STEP/DIR/EN 与 TMC2209 单线 UART；五轴转动和五路通讯已实板验证 |
| 升级 | CAN/USB双模式Katapult，双击RESET进入USB恢复 |

F072 APP、双模式Katapult和工厂镜像已经交叉编译；由于Cortex-M0没有VTOR，
APP使用SRAM向量表重映射。FLY-D5 的 CAN、五轴运动和五路 TMC2209 通讯已完成
实体板验收；USB恢复与 CAN 在线升级的双模式切换仍待验收。

### STM32G431CBU6 / WeAct STM32G431CBU6 Core

| 功能 | 当前规划 |
|---|---|
| FDCAN RX/TX | PB8/PB9 |
| USB D-/D+ | PA11/PA12 |
| 用户恢复键 | PC13，高电平有效、APP/Katapult 使用内部下拉 |
| 指示灯 | PC6，高电平点亮 |
| CAN-FD速率 | 500 kbit/s仲裁段、1 Mbit/s数据段、BRS；2 Mbit/s为待改善布线后复验的可选档 |
| 系统时钟 | 板载 8 MHz HSE，经 PLL 运行至 170 MHz；PC14/PC15 的 32.768 kHz LSE 预留 |
| 升级 | CAN/USB双模式Katapult |

G431固件和Bootloader已经编译、链接并生成镜像。实体 8 MHz HSE、CAN-FD
发现/心跳/PING、PC6 LED GPIO 和 PC13 内部下拉输入已经通过；USB、Bootloader
模式切换和UART仍待验收。

### 总线物理层

- 总线主干首尾各放一个120Ω终端，断电测量CANH与CANL约60Ω。
- 支线尽量短，中间节点不重复加终端。
- Classical CAN节点不能正确接收CAN-FD数据帧；若系统需要F103 Classical CAN
  和G431 CAN-FD同时发挥各自带宽，推荐使用两条物理总线和两个主机接口。
- 当前主机适配器基线是刷入 CANable2.5 Candlelight/`gs_usb` 固件的 CANable2：
  直接作为 SocketCAN `canX` 使用，不经过 SLCAN；原厂 SLCAN 仅保留为兼容路径。

## Bootloader与升级

- Katapult占用Flash前8 KiB，APP从`0x08002000`启动。
- 正常情况下由APP命令进入CAN Katapult。
- CAN可用但希望走USB时，可用`BOOTLOADER_ENTER_USB`命令切换。
- CAN完全失效时，WeAct BluePill Plus 按住 PA0、WeAct STM32G431CBU6 Core 按住 PC13，
  再复位进入USB恢复模式。
- 工厂镜像由Bootloader和APP合并，后续在线升级只写APP。

F103实板已验证：

- CAN Katapult UUID：`70fa76b1eae9`
- USB枚举：`1d50:6177`
- CAN升级、USB升级、SHA校验、复位和APP恢复均通过。
- USB升级后可以再次切换回CAN Katapult。

当前Bootloader是可靠升级通道，不是安全启动边界；镜像签名、防回滚和密钥管理
尚未实现。

## 测试与实测结果

### 自动测试

构建启用`vcan0`时共注册32项：

- 协议、CRC、分片和CAN/CAN-FD帧。
- SocketCAN收发。
- Classical CAN和CAN-FD完整端到端链路。
- Classical CAN和CAN-FD双节点故障隔离。
- Remote Core、Mock Node、请求管理、发现和心跳。
- GPIO、UART、PWM、定时位流/WS2812、资源模型和嵌入式C Remote Core。
- 资源合同、共享/独占租约、续租、释放、到期和安全清理。
- 严格板卡描述schema、内部资源占用和数字孪生故障隔离。
- CAN-FD BRS、六类业务、令牌桶准入和低预算端到端拒绝。
- 固件 menuconfig 的硬件 UART/TMC 端口容量派生和非法组合拒绝。

2026-08-10 在指定的 `Ubuntu` WSL 复核：完整 32/32 项测试通过，0项失败；
Classical CAN与CAN-FD单节点/双节点、资源租约、运动、流量准入、GUI/API、PWM
和WS2812端到端路径均包含在测试中。F072、F103、G431 三块命名板卡的基础 APP
配置均完成交叉编译；实体波形和高负载并发仍需后续实板验收。

### F103实体链路

BluePill Plus、TJA1051/3和CANable适配器已经完成：

- 节点发现、分配、心跳、PING、信息和能力查询。
- GPIO读写和PB2板载LED控制。
- USART1文本及二进制双向通信。
- 255字节UART原子写入；256字节空间不足时完整拒绝。
- 2023字节最大PING载荷连续10/10通过。
- 8个并发客户端各100次PING，共800次无失败。
- 1 Mbit/s下短PING约12～16 ms，最大载荷PING约364 ms。
- 测试结束SocketCAN错误、丢包和bus-off计数为0。
- CAN与USB两种Katapult升级闭环。

## 当前固件配置方式

`firmware/Kconfig`和`menuconfig`当前管理：

- MCU、板型、晶振和系统时钟。
- CAN/FDCAN 引脚、速率和收发器控制。
- 协议包、请求缓存、重组槽和 UART 缓冲等静态预算。
- UART 0 引脚选择、上电 IO 安全状态，以及可选PWM/定时位流后端和静态容量上限。
- 独立 APP 或保留 8 KiB Katapult 的 Flash 布局。
- 可选运动/TMC 后端、最大轴数、队列和时序硬上限；默认关闭。

后续明确分为两类：

- `menuconfig`：MCU、晶振、CAN、Bootloader 布局、是否链接运动/TMC/PWM/定时位流，
  以及各模块的静态最大容量。当前波形引脚只作为过渡期默认值。
- 运行时资源清单：STEP/DIR/EN、限位、按钮、TMC UART/SPI、PWM/定时位流引脚、
  频率/时序与资源依赖。

## 下一阶段设计

### 已在Mock开始实现的公共基础

- 资源能力合同已经完成协议、Mock、C++ API和CLI第一阶段。
- 资源共享读/独占租约已经支持冲突、续租、释放、到期和会话清理。
- 租约释放会删除关联对象，GPIO输出拉低，UART执行资源复位。
- 本地应用强所有权仍需持久IPC客户端身份或逐操作租约令牌。
- Mock板型、UUID模板、资源、能力合同和内部占用已改由严格JSON描述加载。
- 数字孪生支持确定性注入单路UART故障、GPIO输入和节点离线/恢复。
- `toolbusd`支持发送线时间估算、业务分类、带宽准入和本地统计查询。

### 智能多轴运动

- Linux执行运动学、加减速和TMC协议。
- MCU使用定时器输出STEP，GPIO输出DIR/EN。
- 主机提前下发压缩运动段，不逐个脉冲占用CAN。
- 同一节点多轴共享硬件时间基准。
- 限位、急停和TMC DIAG走MCU本地快速联锁。

### 跨板同步

- `toolbusd`维护64位全局运动时间。
- 分别估计每个MCU的时钟偏移和晶振漂移。
- 所有节点预装运动段并报告就绪。
- 全部就绪后提交同一个未来绝对开始时间。
- 时钟误差或队列余量越界时，在共同段边界停止。

跨板限位需要经过CAN，实时等级低于本地限位。安全关键限位应与STEP轴同板、
硬件并联到相关节点，或增加独立急停/使能关断线。

### 持久化资源清单

- 主机可读工程文件编译成版本化二进制TLV/资源图。
- MCU检查引脚、复用、定时器、总线、保留资源和安全电平冲突。
- F103/G431首版使用内部Flash仿EEPROM。
- 配置使用A/B双槽、CRC、代数、提交标记和断电回滚。
- Katapult升级必须保留配置区；当前链接脚本尚未预留该区域。

### 图形化配置与监控

第一版 `gui/` 本地中文工具已经落地：

- 三块已知板卡可切换，运动轴的 STEP/DIR/EN/TMC/DIAG 引脚使用分组下拉框。
- SWD、CAN、Katapult USB、板载演示功能和已经分配的引脚不会再次出现。
- 可以编辑GPIO、PWM和WS2812草案并导出资源清单JSON；所有资源共享引脚冲突检查。
- 可显示 Mock MCU 导出的 GPIO 电平、步进位置、运动队列、PWM和WS2812状态；
  未连接 Mock 时提供完整演示视图。

仍待完成的是在线读取、比较、部署、备份和回滚资源清单，以及 MCU/`toolbusd`
CPU 占用、STEP 中断耗时、栈余量、CAN 负载和跨板同步误差等正式遥测。GUI、
命令行、测试和生产工具最终必须共用同一套 schema、校验器和配置编译器。

## 当前执行顺序

1. 完善已落地的Mock多轴运动段、队列、安全停机和能力准入。
2. 实现节点时钟模型、跨板`PREPARE/READY/COMMIT`和组故障策略。
3. 扩展数字孪生：输入采样、TMC模型、时钟漂移、CAN故障和会话重放。
4. 完善运动队列、CPU/中断、同步精度、CAN负载和资源健康遥测。
5. 冻结图形配置器共用的资源 schema、校验器和工程文件，把现有 GUI 草案接入
   toolbusd IPC 与 EEPROM/Flash A/B 部署事务。
6. 为F103/G431划分配置区并实现实体定时器执行器，实测能力上限。
7. 完善 TMC2209 专用单线 UART、TMC SPI 事务和 Linux 协议库。
8. 完成PWM/定时位流实体波形和运动并发验收；在以上主线闭环后，再继续
   通用SPI/I2C/ADC/Timer/Storage。

## 已知风险与未完成项

- `toolbusd`尚未自动监测和恢复SocketCAN error-passive/bus-off。
- 单节点切换到USB Bootloader后，总线上可能无人ACK发现帧，使部分`gs_usb`
  适配器进入error-passive。
- F103只有20 KiB SRAM，当前完整协议配置已使用约10.8 KiB；运动队列必须严格
  预算并实测最大轴数和步频。
- Classical CAN大包分片开销明显，原厂SLCAN还受到ASCII和USB CDC限制。
- G431 的 HSE、CAN-FD APP、板载 GPIO/PWM 和单轴运动已实体测试；双模式
  Bootloader 切换、UART 与新版 compare 调度压力上限仍不能视为硬件完成。
- 配置Flash区域尚未从链接脚本中正式预留。
- 当前资源租约只能隔离远端会话，本地应用仍共享`toolbusd`会话身份。
- 跨板启动偏差、长期漂移和跨板限位停止距离尚无实体测量数据。
- Bootloader尚无镜像签名、防回滚和生产密钥体系。
- PWM与定时位流的新通用STM32后端尚未实体波形验收；F072静态位流缓冲还需压缩。
- SPI、I2C、ADC、Timer、Storage和硬件ID仍未实现。

## 仓库状态

- GitHub：`https://github.com/sendu2wfdx/RemoteBSP`
- 主分支：`main`
- 自动验证基线：32 项主机/VCAN/配置测试，以及 F072、F103、G431 三块命名板卡
  基础 APP 的交叉编译；其他兼容/Bootloader预设按改动范围抽查。

## 文档入口

- [README](../README.md)
- [项目待办](../TODO.md)
- [总体架构](architecture.md)
- [使用场景与系统需求](use-cases-and-requirements.md)
- [智能实时资源与多轴运动控制设计](intelligent-motion-resources.md)
- [STM32硬件规划](stm32-hardware-plan.md)
- [STM32编译与烧录](stm32-build-and-flash.md)
- [Katapult双模式升级与应急恢复](bootloader-upgrade.md)
- [libremotebsp客户端API](libremotebsp-api.md)
