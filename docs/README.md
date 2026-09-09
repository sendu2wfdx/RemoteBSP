# Remote BSP 文档导航与项目状态

> 最后整理：2026-09-09

## 文档导航

- [系统分层与通信设计](architecture.md)
- [固件配置与 RemoteBSP Studio](configuration-and-studio.md)
- [智能运动与资源模型](intelligent-motion-resources.md)
- [板卡描述与数字孪生](board-manifest-and-digital-twin.md)
- [STM32 构建、烧录与验证](stm32-build-and-flash.md)
- [Katapult 双模式升级与应急恢复](bootloader-upgrade.md)
- [USB Vendor Bulk 传输](usb-transport.md)
- [Mellow FLY-D5](mellow-fly-d5.md)
- [WeAct BluePill Plus](weact-bluepill-plus.md)
- [WeAct STM32G431CBU6 Core](weact-stm32g431cbu6-core.md)
- [客户端 API](libremotebsp-api.md)
- [使用场景与需求](use-cases-and-requirements.md)
- [产品定位与上位机配套架构](product-positioning-and-host-stack.md)
- [Studio 与上位机运行时边界](studio-runtime-design.md)
- [总线设备与高速流资源设计](bus-and-stream-resources.md)
- [运动可靠性与跨板同步验证计划](motion-reliability-plan.md)
- [主机时钟同步模型](clock-synchronization.md)
- [只读 Runtime API](runtime-api.md)

## 一句话说明

Remote BSP 让 Linux 主机通过 CAN/CAN-FD 或可选 USB 链路统一管理多块 MCU 工具板，
像本地资源一样操作远端 GPIO、UART、PWM、WS2812 和智能运动。Linux 处理设备协议
与运动规划，MCU 只执行原子、确定性的硬件操作和必要的本地安全机制。

## 配置模型

```mermaid
flowchart LR
    A["Studio 工程"] --> B["生成 Kconfig .config"]
    B --> C["构建板卡专用固件"]
    C --> D["烧录并重启"]
    D --> E["固定资源映射"]
    F["SN / UUID / 制造 / ADC校准"] --> G["EEPROM或Flash仿EEPROM"]
```

- Kconfig 同时负责硬件基础参数、功能裁剪、容量预算和具体资源映射；
- Studio 是普通用户入口，`menuconfig` 是开发备用入口；
- 运行中不动态申请 IO 或改变复用关系；
- 设备参数独立持久化，不保存 IO 拓扑；
- 当前内部 Flash 双页后端已实现，外部 EEPROM 后端待实现；
- Katapult APP 写入上限已避开参数区，升级应保留设备身份和校准值。

## 当前实现状态

| 模块 | 状态 | 说明 |
|---|---|---|
| Remote Packet v1 | 已实现、已测试 | 24字节固定头、小端线序、CRC-32、严格长度和版本检查 |
| 分片与重组 | 已实现、已测试 | Classical CAN 8字节、CAN-FD 64字节、最大包2048字节 |
| 传输抽象 | 已实现、已测试 | SocketCAN、libusb和Mock USB共用`LinkTransport` |
| 发现、心跳、请求 | 已实现、已测试 | UUID发现、节点分配、500 ms心跳、2 s离线、重试和副作用去重 |
| 本地 IPC / C++ API / CLI | 已实现、已测试 | 应用不直接访问CAN；覆盖节点、资源、GPIO、UART、运动、波形和升级 |
| 设备参数 | 第一阶段已实现、已测试 | schema、双页存储、Mock、STM32 Flash后端、协议/API/CLI、Katapult保护 |
| GPIO / UART | 已实现、已测试 | Mock完整；F103与G431 USART1/2/3均已完成115200三路全双工并发实测，各方向每路1024字节逐字节一致 |
| PWM / 定时位流 / WS2812 | 第一阶段已实现、已测试 | 主机、Mock、GUI和三款STM32后端已编译；G431 PWM对象命令已实测，实体WS2812波形待验收 |
| 智能运动 | 第一阶段已实现、已测试 | Mock多轴、TIM2 compare调度、限位停机和遥测；跨板事务已接入Mock Remote Core与真实Mock队列，守护进程协调器接线、实体固件和时序验收待实现 |
| TMC2209 | 第一阶段已实现、部分实测 | FLY-D5五路单线通信及五电机已实测，F103/G431待系统验收 |
| Studio | 构建阶段已实现 | 工程schema v2、冲突检查、I2C/SPI图形编辑、Mock可视化、工程差异、`.config`与32线程构建；可生成接线资料包和明确软件/硬件证据边界的确定性生产记录，自动烧录回读待实现 |
| I2C / SPI | 协议、Mock与Studio配置竖切已实现 | 总线/设备合同、原子事务、设备级故障隔离、公开端点白名单、图形编辑及Studio到DigitalTwin黄金路径已测试；STM32 BSP待实现和实板验收 |
| 高速 Stream | 协议骨架已实现、已测试 | 合同、打开、数据、信用和状态编解码；运行时会话及USB数据面待实现 |
| Runtime API | 只读竖切已实现、已测试 | RuntimeSnapshot IPC v2贯通节点时钟同步质量；拓扑稳定、资源故障隔离、短缓存和新鲜度已测试，失败不回退陈旧快照；认证、写操作和事件流待实现 |
| ADC / Timer / Storage | 尚未实现 | 按当前优先级后置 |

## 正式板卡

| 板卡 | 当前能力 |
|---|---|
| STM32F072RBT6 / Mellow FLY-D5 | Classical CAN 1 Mbit/s、GPIO、五轴和五路TMC2209已实测；双模式Katapult切换待验收 |
| STM32F103CBT6 / WeAct BluePill Plus | 外部8 MHz HSE、32.768 kHz LSE资源、Classical CAN、GPIO、USART1/2/3、双模式Katapult；三路115200全双工并发实板验证通过；五轴/TMC待实板验收 |
| STM32G431CBU6 / WeAct Core | 外部8 MHz HSE、32.768 kHz LSE资源、CAN-FD、USART1/2/3、PC6 PWM、PC13 GPIO；三路UART、Studio专用固件及单轴空载调度已实测，五轴持续负载/USB/双模式Katapult待继续验收 |

## 严格分层

```text
Application
  -> libremotebsp
  -> toolbusd / Unix Domain Socket
  -> Remote Protocol
  -> Fragmentation
  -> LinkTransport
  -> SocketCAN / libusb
  -> Remote Core
  -> MCU BSP
```

协议层不知道 CAN 类型，传输层不知道 GPIO/UART/运动业务。MCU 固件不得包含
Modbus、GPS、传感器、阀门或厂商协议。

## 状态口径

- “已实现”：源码路径已经存在；
- “已编译”：目标交叉编译通过，但不等同硬件正确；
- “已测试”：自动测试或 Mock 路径通过；
- “已实测”：实体板实际验证过；
- “仅设计”：只有文档或界面草案。

详细未完成项和验收条件见 [TODO](../TODO.md)。
