# Remote BSP 待办

本文只记录尚未完成的工作。当前事实与实测结果见 [README.md](README.md) 和
[文档导航](docs/README.md)。

## 当前产品路线

当前迭代按用户要求暂不推进需要多块实体板共同验证的跨板卡场景；相关设计和已有
软件证据保留，但不会用单板或 Mock 结果关闭跨板验收项。其余单板、主机、Studio、
协议、总线和可靠性工作继续依次推进。

- RemoteBSP Studio 保存板卡工程并生成完整 Kconfig `.config`。
- Kconfig 负责硬件基础参数、功能裁剪、静态预算和具体 IO/外设映射。
- 固件烧录并重启后建立固定资源表；运行中不动态申请 IO 或改变复用关系。
- SN、UUID、制造信息和 ADC 校准等少量参数使用独立 EEPROM/Flash 仿 EEPROM。
- Linux 负责 Modbus、传感器、TMC 寄存器语义、运动规划和设备业务协议。
- MCU 只执行原子硬件操作、确定性运动、限位联锁和必要的本地安全策略。

## P0：Studio 专用固件闭环

当前已能从工程JSON生成`.config`，通过固定白名单后端使用32线程构建，归档工程、
配置、GPIO/UART/PWM/定时位流只读静态表、ELF/BIN/HEX/MAP、日志、源码/依赖/工具链身份和SHA-256，
构建期输入漂移会拒绝归档，下载会复核普通文件边界、大小与哈希；并完成三块正式板卡的
真实交叉编译。下一步：

- 继续演进已落地的工程 schema v2、迁移规则和规范配置哈希，并补充向后兼容样例；
- 把 MCU/板卡完整 AF、定时器、DMA、EXTI、ADC 能力整理为机器可读数据源；
- 已生成确定性中文接线表、资源占用摘要、稳定资源清单和校验文件，并提供有界、
  确定性的工程差异、差异资料包、生产记录和带清单/校验和的生产批次包；下一步
  在已落地的原子本地批次历史、损坏隔离和四类检索之上增加签名、可信时间、权限
  边界和归档迁移；
- 已把独立部署作业接入非交互 CLI 的显式 `deploy-stlink` 命令：校验受保护构建记录/
  固件后执行 ST-Link 写入、校验、复位和有界重试，并通过调用者指定的有界 JSON 文件
  核对板型/工程/配置/固件四类身份；核验成功后生成自哈希部署记录，并可严格关联生产
  记录和批次；Web 已提供默认关闭的两阶段受控 ST-Link 入口；下一步接入 CAN/USB
  Katapult 执行器、可信时间与签名，并完成实体部署验收；
- 已用独立版本化 `FirmwareIdentity` 命令贯通 MCU、Mock、`libremotebsp`、`toolbusd`
  CLI 和 Studio，并由 Studio 构建注入工程、配置、固件输入三个 SHA-256；非 Studio
  构建逐字段返回 unavailable。`inspect-runtime-identity` 只读并准确返回完整或缺项，
  只读检查的 `deployment_verified` 固定为 false；Mock USB 已增加真实进程级完整身份、
  离线拒绝和同 UUID 重启不复用缓存验证，实体板仍需完成烧录后的自动核验；
- 为批量生产增加非交互命令行入口，网页只调用同一后端；
- 将实时 GPIO/PWM/WS2812 控制页通过 `toolbusd` 接入，不允许网页直连 CAN。

验收条件：不进入 `menuconfig`，从一份 Studio 工程可重复得到相同固件，完成构建、
烧录、重启和回读核对；冲突配置在构建前被定位并拒绝。

## P0：设备参数与持久化

已完成公共 schema、双页掉电安全快照存储、Mock/嵌入式 Remote Core、C++ API、CLI、
三款 STM32 内部 Flash 后端和 Katapult 写入上界保护。下一步：

- 在 F072、F103、G431 实板验证首次初始化、反复写入、掉电中断和磨损行为；
- 增加生产写入权限、一次性字段、解锁策略、审计日志和批量烧号工具；
- 完善参数 schema 迁移、默认值、长度变化和向后兼容测试；
- Studio Web 已提供只读快照和带 SHA-256 完整性校验的 v2 备份，恢复兼容 v1；显式 CLI
  写入/恢复已用 UUID、generation CAS 与固定维护确认短语贯通；下一步补齐受控写入/
  恢复页面、操作审计与实体参数区验收；
- 增加 ADC 校准数据的结构化格式和校准流程；
- 实现外部 EEPROM 后端，并保证协议和上层 API 不变；
- 评估写频率、寿命和坏页策略，禁止把高频遥测写入参数区。

参数区不得保存 GPIO、UART、运动轴、PWM、WS2812 等资源拓扑，也不承担 APP A/B。

## P1：智能步进运动与跨板同步

现状：Mock 多轴执行器和 STM32 TIM2 compare 下一边沿调度已实现；F072/FLY-D5 五轴
及 TMC2209 已有实板基础验证。待办：

- 对 compare 调度器做 F072/F103/G431 实板步频上限、抖动和 ISR 最坏耗时测试；
- 用示波器/逻辑分析仪验证 STEP 高低宽度、DIR 建立时间、共享 EN 和同步边沿；
  2026-09-10 关闭 DL16 官方上位机后，`atk-logic` 已在 D5/PA6 完成 1 kHz/50%、
  1 kHz/12.34%、100 kHz/50% 和 100 kHz/25% 采集，均为 0 毛刺；最短脉宽分别为
  499.9 µs、123.3 µs、5.0 µs 和 2.5 µs。最终生命周期修复固件再次通过
  100 kHz/25%。此前 `incomplete` 只发生在 PA0/PA4，仍需确认公共 GND 并复测。
  单路 PWM 不得记作 STEP、TimedBitstream 或跨板波形验收；
- 主机四时间戳模型、`TimeSync v1`、Mock `boot_epoch`、同步管理器和 `toolbusd`
  有界周期调度已接入；通用双页启动代次日志及掉电故障注入已完成，但三板尚未划分
  独立保留区并接入回调；下一步增加实体计数器 BSP 和可靠启动代次板级验收，
  并验证真实 CAN/CAN-FD 传输边界；
- 跨板运动组的版本化线格式、时钟冻结、协调器、RequestManager 服务桥接、
  `toolbusd` 主循环、版本化 IPC/API/CLI、双节点 Mock USB 进程级闭环和 STM32
  公共 Remote Core 参与者和 STEPGEN 静态独占租约生命周期已经完成；下一步为三款板
  提供经过掉电/快速复位验证的可靠 `motion_boot_epoch()`，并处理部分 COMMIT 已送达
  后的网络分区、尽力停止和运维恢复；
- 完善分段轨迹、队列水位、欠载预警、迟到段拒绝与平滑停车；
- 限位和 TMC DIAG 使用本地中断/定时滤波，形成有界安全事件；
- 每块板的最大轴数由 CPU、定时器、GPIO、步频总预算和其他已启用模块共同决定；
- 评估 STEP DMA 批量边沿作为可选后端，不把 DMA 设为所有板卡的强制依赖；
- F103/G431 五轴、共享 EN、TMC2209 与持续运动需要实体板验收；
- TMC 配置事务由 Linux 串行调度，不做低收益的 MCU 并发事务。

## P1：数字孪生

- Mock 与 Studio 已共用版本化板卡描述建立 I2C/SPI 总线、设备合同和公开端点；
  Studio 已从同一能力目录生成 GPIO/UART/PWM/定时位流只读静态表，并由三板启动校验
  和运行时白名单直接消费；2026-09-10 真机发现 STM32 `ResourceEnum` 缺口后，已补齐
  Enum/Describe/Status/Contract，G431 重刷后枚举 3 UART、PWM0、TimedBitstream0，
  RuntimeSnapshot 返回 1 节点/5 资源；下一步迁移运动资源，I2C/SPI 仍须等实体
  AF/DMA/电气能力确认后才可进入正式表；
- 模拟 STEP/DIR/EN、限位/DIAG、按钮、UART、PWM、WS2812 和设备参数；
- 注入时钟漂移、CAN 延迟/丢包、队列欠载、资源卡死、掉线和重启；
- 导出全局时间、本地 tick 和 STEP 边沿，验证同板及跨板同步；
- 已完成故障脚本 replay v1、显式 Mock 逻辑传输钩子和 `RecordingLinkTransport` 会话
  录制：固定单调时间基、事件序号、H2N/N2H、节点代次、发送失败、接收空/异常以及
  delay/drop/duplicate/reboot；`toolbusd` 已增加默认关闭、固定目录、限额且禁止覆盖的
  启动时逻辑录制入口及离线校验/回放。下一步补运行期 IPC/CLI 控制和信号退出进程测试，
  再接物理链路采集；不把逻辑回放冒充 CAN 仲裁、USB transaction 或真实复位时长；

## P1：遥测、监控与故障隔离

- G431 `ResourceStatus` 已接入 UART 真实 RX/TX 环形缓冲水位与溢出计数，以及
  PWM/TimedBitstream 的对象/后端忙和后端失败基础状态；`pwm-stop` 对象释放缺口已
  修复并在实板验证 Busy -> Normal、对象 1 不复位重建为对象 2。公共 Core 现把
  I2C/SPI 设备持有独占租约映射为 Busy，释放、超时和会话清理后恢复 Normal；该项只有
  主机单元测试；G431 板级 I2C/SPI HAL 已交叉编译但未烧录/电气实测，
  F072/F103/G431 板级 HAL 均已实现并交叉编译，但尚无实体总线验证。下一步接入 STEPGEN、总线
  错误计数和完整 MCU 健康生产者；未覆盖字段仍不得把全零解释为实体健康；
- 公共 Core `ResourceReset` 已恢复 UART 清缓冲/故障并释放、PWM stop、
  TimedBitstream abort 的兼容语义，且后端失败时保留对象/故障状态；下一步
  把恢复结果接入更完整的健康历史和运维界面；
- F072/F103 的 I2C1 PB6/PB7 与 SPI1 PA5/PA6/PA7+PA4 CS，以及 G431 I2C1、
  SPI1/SPI2 的板级 HAL 已编译；待接入合适从设备后完成烧录、应答/超时/
  恢复、片选与时序采集。生产板级代码已有主机 HAL 桩覆盖 flags、统一超时预算、
  可选恢复和 SPI CS/失败恢复，但该证据不是实板。SPI1 使用 PA6，当前 DL16 D5/PA6 接线保持时不得
  烧录该总线测试配置；

- MCU 公共健康应答已接入 HAL 可用性位图、CPU/ISR 负载、栈水位、运动队列、活动租约、
  资源故障、uptime 和采样丢失计数；下一步为 F072/F103/G431 提供可保证跨重启变化的
  生产者代际及真实 CPU/ISR/栈采样后端，并补 ISR 最大耗时、迟到和欠载计数；
- `toolbusd`：CAN 利用率、排队延迟、丢帧、重试、超时、离线和流量准入统计；
- 资源：UART 溢出、GPIO 滤波事件、PWM/位流故障、运动限位与驱动错误；
- 指标采用稳定 ID、单位、时间基准和版本，不把人类文本作为机器接口；
- `HealthSnapshot v1` 协议契约已完成，生产者代际为必填字段，并区分 available、
  unavailable、unknown 和缺席；toolbusd、Mock Remote Core、公共 MCU Core、
  `libremotebsp`、CLI 与 Runtime 可信投影已贯通；下一步接入三款实体板的采样 HAL，实体
  CPU/ISR/栈与硬件时间戳仍需板卡环境；
- 单节点或单资源异常不得阻塞其他节点；恢复必须显式、可观测、可测试；
- GUI 增加趋势、峰值锁存、阈值告警和按节点/资源下钻；
- 明确区分估算值、软件测量值和硬件实测值。

## P2：UART 与工业现场使用

普通 UART 已有协议、对象、流式接收、缓冲和 Mock；F103、G431三路均已完成
115200全双工并发实测。后续：

- 评估F103/G431普通UART可选DMA后端、缓冲水位遥测和资源冲突规则；
- 设计标准 RS-485 适配器：RX/TX、DE/RE、DMA/中断流式接收和每端口隔离；
- 在 Linux 实现 Modbus RTU 轮询和 3.5 字符帧间隔，不把 Modbus 放入 MCU；
- 实测 8 路并发轮询、GPS 持续输入、缓冲水位和 CAN 带宽配额；
- SPI 转多 UART 芯片作为板级通用资源适配器，不能包含设备业务协议。

TMC2209 的 40000 bit/s 单线通信是运动模块的可选专用后端，不是通用软串口。

## P2：PWM、WS2812 与本地事件

- 用实体板和示波器验收三款 MCU 的 PWM、TIM1+DMA 定时位流和 WS2812 时序；
- 补全多组合法定时器/通道/DMA 预设及共享定时器同频约束；
- 关闭模块时确认代码、静态 RAM、定时器、DMA 和中断均不占用；
- GPIO 输入的版本化订阅、边沿、微秒去抖、有界队列、溢出计数和 Mock 本地事件已贯通；
  下一步实现三款 STM32 的周期采样/EXTI 后端并做单板时延与最小稳定脉宽验收；
- 本地事件引擎只实现编译期支持的有限策略，如限位立即停机、按钮锁存和安全输出；
- 不实现可上传任意脚本或字节码的 MCU 规则引擎；
- WS2812 的颜色、色序、亮度和动画由 Linux 生成，MCU 只输出确定性位流。

## P2：上位机 Runtime API 与 Web 控制台

已建立 Runtime API 读取竖切，提供版本化节点、资源、告警和快照接口，并可通过
`remote-cli`/`libremotebsp`读取`toolbusd`本地IPC；Mock、文件和假IPC路径均已完成
纯软件测试。后续继续采用类似 Moonraker 与 Fluidd 的分层，但不复制打印机业务：

- Runtime 默认使用版本化 `remote-cli --json`，单次本地 IPC 快照 v2 已贯通节点时钟
  同步质量，误差上界和样本年龄阈值告警已实现，并保留受陈旧阈值约束的短缓存、
  失败合并和明确新鲜度；API key 身份、`runtime.read` 权限和有界脱敏审计已实现，
  带进程实例游标的有界增量事件短轮询已实现；仅回环认证开放的进程内短时控制租约、
  申请/本人释放/管理员撤销权限、绝对请求期限和终态幂等保护也已实现；租约现通过
  版本化 daemon identity 绑定本次 `toolbusd` 启动，重启、身份非法或不可达时失败关闭；
  认证回环 HTTP 已把 `runtime.gpio.write`、稳定 UUID、节点代次、剩余 TTL 和幂等键映射到
  `toolbusd` GPIO IPC v2；首次对象创建为低电平，释放、过期和关停均执行资源级安全
  写低并执行 `GPIO_CLOSE`，Close 响应不确定时保持单资源 poison、幂等重试只补 Close，确认
  后同一节点代次可以安全重建；固件对象绑定传输会话并隔离跨会话访问；能力证明绑定 daemon
  身份与单调 revision，普通目标错误仅回滚本租约，错误不向 HTTP 暴露内部路径；版本化
  IPC 错误信封和请求级单调绝对期限已贯通请求体、身份单飞、目标快照、CLI、登记、写入与释放，
  并严格区分确定未提交和可能已提交；版本化持久操作账本已覆盖 GPIO 写入与释放，具备
  写前 pending、同步终态、跨租约 TTL 的 operation ID/selector 查询、重启 unknown 恢复和
  资源冻结，Runtime HTTP 已接入 pending 202、终态重放、查询/定位和不确定结果自动恢复；
  `ControlAuditJournal` 已通过显式目录与密钥文件提供同步 intent/terminal/unknown、
  HMAC-SHA256 链、分段容量、进程锁和失败关闭，覆盖租约申请/释放与 GPIO 写入；普通读取
  审计仍保持有界、非阻塞的进程内边界；
  下一步补齐生产文件系统掉电/损坏演练、可靠 MCU boot generation 驱动的自动解冻、
  TLS 部署基线、API 密钥热撤销、控制审计密钥轮换与外部链头锚定、跨重启事件存储、
  认证 SSE 完整状态推送、慢客户端隔离及轮询降级已实现；系统级资源耗尽验证仍待完成；
- Runtime API 只通过 `libremotebsp` 使用 `toolbusd`，不得直接访问 SocketCAN 或 USB；
- 在现有版本化 REST、短轮询和完整状态 SSE 之上评估可恢复的增量遥测流；
- 明确多客户端租约、身份、权限、审计和命令冲突处理；
- Web 控制台展示拓扑、实时状态、运动队列、告警、日志和升级流程；
- Studio 配置构建面与运行时控制面保持分离，只共享稳定模型和视觉组件；
- 先完成薄服务和 Mock 端到端测试，再扩展完整 Web 产品。

## P3：I2C、SPI、协议转换与高速流

- 已完成 `I2C_BUS`/`I2C_DEVICE`、`SPI_BUS`/`SPI_DEVICE` 线协议、主机 API、严格
  编解码、Mock原子事务，以及 Studio schema v2 端点/合同校验、图形编辑和总线专用
  Mock 清单；STM32 公共 Remote Core/HAL 条件编译骨架、静态端点表合同、设备级租约和
  原子事务测试已完成且默认关闭；`toolbusd` 已增加有界合同缓存、首访单飞、父总线仲裁、
  节点代次失效和异常释放。下一步由 Studio 生成经实体能力确认的总线静态表，并逐板
  实现/验收 HAL；
- SPI 转 UART/GPIO/I2C 等板级适配器暴露转换后的统一资源，隐藏内部 SPI；
- 事务必须有最大长度、超时、队列、错误状态和资源级恢复，不得导致节点全局停机；
- 高速 Stream 合同及 H2N/N2H Mock 有界会话已完成，覆盖租约、序号、防重放、
  精确累计 ACK、信用、两阶段提交、背压、故障和旧缓冲隔离；下一步把双向会话绑定到
  独立真实数据面，不通过 CAN 透明隧道全部数据；
- USB Bulk 作为首个高速数据面，Ethernet `LinkTransport` 仅作为后续明确扩展；
- 当前阶段只实现协议、Mock 和软件测试，不操作实体转换芯片或网络硬件。

## P4：其余通用硬件资源

按优先级依次实现 ADC、通用 Timer 和 Storage：

- 保持 Protocol、Transport、Remote Core、BSP 分层；
- 提供资源合同、超时、有界缓冲、故障隔离和 Mock；
- 没有启用的功能不链接、不占资源；
- MCU 不包含传感器、阀门、GPS、Modbus 或厂商设备驱动。

## 板卡验收矩阵

| 板卡 | 已完成 | 主要待办 |
|---|---|---|
| STM32F072RBT6 / Mellow FLY-D5 | Classical CAN、GPIO、五轴与五路 TMC2209 基础实测 | compare 压力、波形、双模式 Katapult 切换、设备参数掉电测试 |
| STM32F103CBT6 / WeAct BluePill Plus | Classical CAN、GPIO、USART1、双模式 Katapult | 五轴/TMC、波形、静态 Studio 固件、设备参数实板验收 |
| STM32G431CBU6 / WeAct Core | CAN-FD、GPIO、PWM、三路UART、单轴转动、Studio专用固件100 STEP空载调度；2026-09-10 完成200次顺序、4×50并发、2023字节分片、三类恢复及5项静态资源到RuntimeSnapshot实测；DL16已取得PA6 1kHz/50% PWM原始采集 | 确认公共GND并复测PA0/PA4；五轴/TMC持续负载、STEP/TimedBitstream/WS2812波形、USB Vendor Bulk、双模式 Katapult、参数区验收 |

## 可审计成熟度门槛

- `maturity/remotebsp-maturity-v1.json` 是当前证据基线，必须通过 Schema 与语义验证；
- Mock、自动测试、交叉编译和实体硬件证据分层记录，禁止相互替代；
- 已建立机器可校验的 RemoteBSP/Klipper 对照草案和运行记录完整性验证器，固定公平性、
  共同工作负载、样本数、独立运行、原始文件哈希和安全失败否决；当前草案尚未锁定双方
  revision、实体环境和验收阈值，也没有执行数据，因此整体对比结论保持 blocked；
- 只有所有适用维度达到验证门槛、无未关闭 blocker 且对照基准可复现时，才允许把
  `overall_comparison.allowed` 改为 `true`。

## 持续验证要求

- 主机代码：构建并运行不依赖实体 CAN 的完整测试；有 `vcan0` 时再运行 CAN/CAN-FD
  端到端、多节点、流量准入和 SocketCAN 测试；
- 固件公共代码、Kconfig 或构建脚本：交叉编译 F072、F103、G431 及受影响板卡组合；
- Katapult 或 Flash 布局：运行双模式补丁测试、工厂镜像打包测试，并确认参数区边界；
- Studio：三块正式板卡工程都必须能生成 `.config` 并真实交叉编译；
- 没有实体板时优先使用 Mock MCU 和 USB Mock，禁止把“交叉编译通过”写成“实测通过”。
