# Remote BSP 项目概览

> 最后整理：2026-07-31
>
> 本文是项目状态入口。功能状态以“已实现、已编译、已实测、仅设计”区分。

## 一句话说明

Remote BSP 让 Linux 主机通过 CAN/CAN-FD 统一管理多块不同 MCU、不同资源的
工具板，并像使用本地资源一样操作远端 GPIO、UART 和后续智能运动资源。
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
    E --> F["SocketCAN"]
    F --> G["CAN / CAN-FD"]
    G --> H["Mock MCU / STM32 Remote Core"]
    H --> I["GPIO / UART / 后续智能资源"]
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
| SocketCAN传输 | 已实现、已测试 | `CanTransport`统一接口，支持`can_frame`和`canfd_frame` |
| 节点发现 | 已实现、已测试 | UUID发现、节点分配、协议版本协商 |
| 心跳与离线 | 已实现、已测试 | 500 ms心跳、2 s离线判断 |
| 请求管理 | 已实现、已测试 | 请求ID、750 ms默认超时、重试、响应匹配 |
| 副作用去重 | 已实现、已测试 | 重试沿用请求ID，MCU返回缓存响应，不重复执行写操作 |
| 本地IPC | 已实现、已测试 | Unix Domain Socket，多客户端并发 |
| C++客户端 | 已实现、已测试 | 节点、GPIO、UART、资源状态、事件及Bootloader API |
| `remote-cli` | 已实现、已测试 | 信息、资源、GPIO、UART和升级入口命令 |
| Mock MCU | 已实现、已测试 | Classical CAN/CAN-FD、多节点、GPIO、8路UART和RX事件 |
| STM32F103 | 已实现、已实测 | Classical CAN、GPIO、USART1、CAN/USB Katapult |
| STM32G431 | 已交叉编译 | CAN-FD、GPIO、USB CDC调试、双模式Katapult，等待实板 |
| SPI/I2C/ADC/PWM/Timer/Storage | 尚未实现 | 仅保留资源类型和后续设计位置 |
| 智能运动资源 | 仅设计 | 多轴STEP、限位、TMC、跨板同步、遥测尚未进入稳定协议 |
| 图形资源配置器 | 待办 | 类似CubeMX，管理运行时资源清单和在线监控 |

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
GPIO_CREATE / GPIO_READ / GPIO_WRITE
UART_CREATE / UART_READ / UART_WRITE / UART_RX_EVENT
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

Mock MCU暴露16路GPIO、8路UART；UART 0～3标记为原生，UART 4～7标记为
扩展。扩展UART背后的内部SPI不会重复暴露。

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

### STM32G431CBU6

| 功能 | 当前规划 |
|---|---|
| FDCAN RX/TX | PB8/PB9 |
| USB D-/D+ | PA11/PA12 |
| CAN-FD速率 | 500 kbit/s仲裁段、2 Mbit/s数据段、BRS |
| 系统时钟 | 170 MHz |
| 升级 | CAN/USB双模式Katapult |

G431固件和Bootloader已经编译、链接并生成镜像，但尚未进行实体CAN-FD、USB和
UART验收。

### 总线物理层

- 总线主干首尾各放一个120Ω终端，断电测量CANH与CANL约60Ω。
- 支线尽量短，中间节点不重复加终端。
- Classical CAN节点不能正确接收CAN-FD数据帧；若系统需要F103 Classical CAN
  和G431 CAN-FD同时发挥各自带宽，推荐使用两条物理总线和两个主机接口。
- CANable2类适配器已经测试过原厂SLCAN和CANable2.5/Candlelight `gs_usb`
  路径。

## Bootloader与升级

- Katapult占用Flash前8 KiB，APP从`0x08002000`启动。
- 正常情况下由APP命令进入CAN Katapult。
- CAN可用但希望走USB时，可用`BOOTLOADER_ENTER_USB`命令切换。
- CAN完全失效时，按住PA0复位进入USB恢复模式。
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

构建启用`vcan0`时共注册16项：

- 协议、CRC、分片和CAN/CAN-FD帧。
- SocketCAN收发。
- Classical CAN和CAN-FD完整端到端链路。
- Classical CAN和CAN-FD双节点故障隔离。
- Remote Core、Mock Node、请求管理、发现和心跳。
- GPIO、UART、资源模型和嵌入式C Remote Core。

2026-07-31在指定的`Ubuntu` WSL复核：

- 11项不依赖SocketCAN接口的测试全部通过。
- 5项SocketCAN/vcan测试因当前未创建`vcan0`而不能启动。
- 创建`vcan0`需要本机sudo密码，本次没有修改系统网络状态。
- 最近一次具备`vcan0`的完整基线为16/16通过。

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

- F103/G431目标板、板型和系统时钟。
- CAN/FDCAN引脚及速率。
- USB CDC调试。
- 协议包、请求缓存、重组槽和UART缓冲等静态预算。
- UART 0引脚选择。
- 独立APP或保留8 KiB Katapult的Flash布局。

后续明确分为两类：

- `menuconfig`：MCU、晶振、CAN、USB、Bootloader，以及某功能是否编译链接。
- 运行时资源清单：STEP/DIR/EN、限位、按钮、TMC UART/SPI和资源依赖。

## 下一阶段设计

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

规划一个类似STM32CubeMX的中文图形化工具：

- 可视化配置引脚、定时器、总线、步进轴、限位和TMC。
- 在线读取、比较、部署、备份和回滚资源清单。
- 显示MCU/`toolbusd` CPU占用、STEP中断耗时、栈余量、队列水位、CAN负载和
  跨板同步误差。
- GUI、命令行、测试和生产工具共用同一套schema、校验器和配置编译器。

## 建议执行顺序

1. 整理并提交当前设计文档，保持主分支状态可追踪。
2. 为F103/G431划分Bootloader、APP和A/B配置Flash区域。
3. 冻结运行时资源schema、配置命令和冲突校验规则。
4. 在Mock MCU实现配置读取、暂存、校验、提交和回滚。
5. 实现单轴STEP执行器和定时输入采样。
6. 扩展为同板多轴同步、队列和本地安全联锁。
7. 实现节点时钟同步及跨板准备/提交协议。
8. 加入通用半双工UART/SPI和Linux TMC库。
9. 实现性能遥测。
10. 在公共配置核心稳定后开发图形化配置器。
11. G431到货后完成CAN-FD、USB、Bootloader和运动性能实体测试。

## 已知风险与未完成项

- `toolbusd`尚未自动监测和恢复SocketCAN error-passive/bus-off。
- 单节点切换到USB Bootloader后，总线上可能无人ACK发现帧，使部分`gs_usb`
  适配器进入error-passive。
- F103只有20 KiB SRAM，当前完整协议配置已使用约10.8 KiB；运动队列必须严格
  预算并实测最大轴数和步频。
- Classical CAN大包分片开销明显，原厂SLCAN还受到ASCII和USB CDC限制。
- G431尚未实体测试，不能把交叉编译结果视为硬件完成。
- 配置Flash区域尚未从链接脚本中正式预留。
- 跨板启动偏差、长期漂移和跨板限位停止距离尚无实体测量数据。
- Bootloader尚无镜像签名、防回滚和生产密钥体系。
- SPI、I2C、ADC、PWM、Timer、Storage和硬件ID仍未实现。

## 仓库状态

- GitHub：`https://github.com/sendu2wfdx/RemoteBSP`
- 当前分支：`codex/stm32-dual-katapult`
- 远端已推送提交：
  - `64833f8`：实现STM32双模式Katapult升级。
  - `f8637ca`：完善项目README。
- 当前智能运动、跨板同步、遥测、图形配置器和本概览仍为本地文档改动，尚未
  提交和推送。

## 文档入口

- [README](../README.md)
- [项目待办](../TODO.md)
- [总体架构](architecture.md)
- [使用场景与系统需求](use-cases-and-requirements.md)
- [智能实时资源与多轴运动控制设计](intelligent-motion-resources.md)
- [STM32硬件规划](stm32-hardware-plan.md)
- [STM32编译与烧录](stm32-build-and-flash.md)
- [Katapult与USB调试](bootloader-and-usb-debug.md)
- [libremotebsp客户端API](libremotebsp-api.md)
