# Remote BSP 仓库协作约定

## 交流与文档

- 始终使用中文与用户交流。
- 新增或修改的文档、代码注释、配置帮助和脚本提示使用中文；协议字段、API、芯片外设名等必要术语可保留英文。
- 当前进度、实测结果和路线图写入 `README.md`、`TODO.md` 或 `docs/`，不要把易过时的状态堆入本文件。

## 项目目标与边界

- Linux 主机通过 CAN/CAN-FD 统一管理多块远端 MCU 工具板，应用通过 `libremotebsp` 和 `toolbusd` 使用远端资源，不得直接访问 SocketCAN。
- MCU 只实现通用、原子、确定性的硬件操作与必要的本地安全机制；Modbus、传感器、阀门、GPS、厂商协议和运动规划运行在 Linux。
- 保持 `Protocol -> Fragmentation -> Transport -> Remote Core -> BSP` 严格分层；协议层不知道 CAN 类型，传输层不知道 GPIO、UART 或运动业务。
- 当前支持的实体目标为：STM32F072RBT6 / Mellow FLY-D5、STM32F103CBT6 / WeAct BluePill Plus、STM32G431CBU6 / WeAct STM32G431CBU6 Core。
- RemoteBSP APP 默认运行 CAN/CAN-FD；STM32G431 另提供与 CAN 固件互斥的 USB Vendor Bulk APP，USB Katapult 仍是独立恢复升级阶段和独立 PID。
- 不加入 STM32 之外的 MCU、Linux 内核驱动、Ethernet/UART 业务传输或设备专用驱动，除非用户明确扩展范围。

## 固件与配置原则

- RemoteBSP Studio 是普通用户的正式配置入口，保存版本化工程并生成完整 Kconfig `.config`、板卡专用固件和构建记录，最终负责 ST-Link、CAN Katapult 或 USB Katapult 烧录；终端 `menuconfig` 只作为开发、CI 和无 GUI 环境的备用入口。
- Kconfig 负责 MCU/板型、晶振、CAN/USB、Bootloader 布局、功能裁剪、静态资源上限，以及 GPIO、UART、STEP/DIR/EN/DIAG、TMC、PWM 和 WS2812 的具体映射。
- 资源映射编译进板卡专用固件并在重启后固定生效；运行期间不动态申请 IO 或改变引脚复用。不保留旧版二进制资源清单或在线资源覆盖路径。
- SN、UUID、制造信息、设备名称和 ADC 校准等少量设备参数使用与资源映射独立的 EEPROM/Flash 仿 EEPROM 存储接口；当前先实现内部 Flash 双页后端，后续可替换外部 EEPROM。
- MCU 能力、板卡固定占用、GUI 候选项、Mock 和固件最终校验应来自同一机器可读能力源；GUI 只能选择合法 AF、定时器、DMA 和完整外设端点组合。
- `firmware/configs` 的正式预设只保留 MCU 基础配置和名称明确的板卡配置；临时接线与压力测试配置放到测试目录或构建目录，不新增长期组合预设。
- 运动、TMC 通讯、WS2812 等可选模块未启用时不得链接，也不得占用静态 RAM、定时器、DMA 或中断资源。
- 单节点、单资源或单串口异常不得阻塞其他节点和资源；实时路径使用有界队列、超时、去重、故障隔离和安全停机。

## 当前研发优先级

1. RemoteBSP Studio 工程、专用固件生成与烧录闭环。
2. 智能步进运动与跨板同步。
3. 数字孪生和资源能力共模。
4. 遥测、健康监控和故障隔离。
5. 其余通用 SPI、I2C、ADC、PWM、Timer、Storage 资源。

## 开发与验证

- Linux 构建和测试只使用名为 `Ubuntu` 的 WSL；不得使用或修改 `RK3568` 环境。
- Windows 工作区为 `D:\Documents\RemoteBSP`，WSL 路径为 `/mnt/d/Documents/RemoteBSP`。
- 编译默认使用 32 个并行任务。
- 没有实体硬件时优先使用 Mock MCU 与 `vcan0`；修改协议、Remote Core、运动或传输后运行相关单元测试和端到端测试。
- 修改 STM32 公共代码、Kconfig 或构建脚本后，按影响范围交叉编译 F072、F103、G431 以及相关运动/Katapult 配置。
- 保留用户已有修改和文件。删除源码、配置、构建缓存或硬件备份前确认范围；不要删除工作区之外的文件。
- 未经用户明确要求，不提交、不推送、不重写 Git 历史。
