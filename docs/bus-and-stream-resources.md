# 总线设备与高速流资源设计

## 1. 目标与边界

RemoteBSP 不把所有板级总线透明隧道到 Linux。系统把硬件适配成有明确能力、
长度、时限和故障边界的逻辑资源：Linux 负责设备寄存器语义与业务协议，MCU
只负责原子、确定性的总线操作以及必要的本地恢复。

本设计覆盖三类对象：

- `I2C_BUS` / `SPI_BUS`：物理控制器和仲裁边界；
- `I2C_DEVICE` / `SPI_DEVICE`：静态绑定到总线的可访问端点；
- `STREAM`：连续采样、高速 UART、网络数据或大块传输的数据通道。

当前代码提供协议合同、编解码、主机 API、Studio 静态资源图生成，
以及 Mock I2C/SPI 原子事务竖切。
不访问实体板卡，不实现真实 MCU BSP，不把 Ethernet 加入现有传输层，也不提供
可运行的通用流会话。

## 2. 静态资源图

总线、设备、转换器连接关系由 Studio 工程和机器可读能力源确定，构建后固定：

```text
SPI_BUS 0（内部占用，不对 Linux 枚举）
  └─ SPI 转 8 路 UART 芯片
       ├─ UART_PORT 0（expanded，对 Linux 可见）
       ├─ UART_PORT 1（expanded，对 Linux 可见）
       └─ ...

I2C_BUS 0（可见）
  ├─ I2C_DEVICE 0x48（可见）
  └─ I2C_DEVICE 0x50（可见）
```

转换芯片背后的 SPI 控制器和片选不进入外部资源目录，Linux 只看到转换后的
`UART`、`GPIO`、`I2C_BUS` 等统一资源。转换后资源沿用
`kResourceFlagExpanded`。这样既避免绕过适配器直接操作内部 SPI，也避免同一
硬件被两个所有者并发使用。

若一个 SPI/I2C 控制器确实允许通用访问，则总线和设备均进入外部资源目录，
设备合同通过 `parent_bus_resource_id` 指向其总线。Linux 只对设备提交事务；
总线资源用于枚举、能力说明、健康状态和仲裁，不提供绕过设备配置的裸传输。

旧版 `ResourceType::Spi` 和 `ResourceType::I2c` 数值 3、4 保留，只表示 v1
早期扁平端点。新工程使用追加的 `I2cBus`、`I2cDevice`、`SpiBus`、
`SpiDevice` 和 `Stream`，因此已有描述符的线编码不变。

## 3. I2C 原子事务

`I2cTransferRequest` 包含设备资源 ID、事务超时、写数据、读取长度和事务标志。
“写寄存器地址后 repeated-start 读取”必须作为一次请求提交，由 MCU 在同一
总线占用期内完成，不能在 CAN 分片之间释放总线。

设备合同声明：

- 父总线 ID、最大时钟；
- 单次最大传输字节数；
- 有界队列容量与每秒最大操作数；
- 最小、最大事务超时；
- 是否支持 repeated-start 和总线恢复。

NACK、Timeout、Busy、Fault 和 LimitExceeded 是该设备事务的结果，不是节点
级通信错误。后端可在请求允许且合同声明支持时执行 SCL 解锁、STOP 或控制器
复位；恢复次数和错误类型后续应进入资源遥测。一个设备 NACK 不得清空其他设备
队列，也不得让运动、安全或其他节点停机。

## 4. SPI 原子事务

`SpiTransferRequest` 包含设备资源 ID、事务超时、发送数据、接收长度、空读填充
字节和保持 CS 标志。发送与接收在同一时钟阶段全双工进行，较短一侧由 BSP
丢弃或使用空读填充字节补齐。Mode、位宽、CS 极性、最大时钟等配置属于静态
设备合同或板卡能力源，运行期间不能重映射。

一次请求对应一次不可分割的片选周期。MCU 可以内部使用 DMA，但只有在整个事务
完成、超时或明确失败后才返回结果。不同 SPI 设备共享控制器时必须串行仲裁；
切换设备时由 BSP 原子应用该设备的 Mode、速率和 CS 配置。

本轮通用合同提供最大时钟、最大事务长度、队列容量和超时边界。Mode、位宽、
CS 极性等芯片相关静态字段应由后续统一板卡 schema 补充，不应通过运行时请求
临时覆盖。

## 5. STREAM 控制面与数据面

流不是“大号 SPI/I2C 请求”。它有独立生命周期和信用流控：

```text
STREAM_CONTRACT → STREAM_OPEN → STREAM_DATA / STREAM_CREDIT
                              → STREAM_STATUS → STREAM_STOP
```

合同声明方向、可用传输、最大块长、缓冲容量、持续/峰值速率、最大延迟与抖动。
`STREAM_OPEN` 协商块长和初始信用；发送方只有在信用充足时才能发送
`STREAM_DATA`，接收方消费数据后用 `STREAM_CREDIT` 归还额度。因此任何一端都
不需要无界缓存。

控制面负责发现、打开、停止、状态、错误和安全策略。数据面负责带序号的数据块，
按合同可附节点时间戳。建议的链路选择是：

| 业务 | 首选链路 | CAN 降级 |
|---|---|---|
| 心跳、安全、运动段、少量控制 | CAN/CAN-FD | 原生承载 |
| 低速遥测和短数据块 | CAN-FD | 可限速承载 |
| 高速采样、网络包、文件或图像 | USB Bulk/Ethernet | 默认禁止 |

高速数据链路断开时，不自动把完整流量迁移到 CAN。控制面仍可通过 CAN 报告
“数据链路离线”，必要时只允许少量诊断数据降级。

## 6. CAN 带宽边界

协议包最大载荷不是可用吞吐承诺。一个 1024 字节原子事务经过协议头和 CAN 分片
后会占用大量帧；在 Classical CAN 上尤其不适合作为常态负载。主机发送前必须
同时满足资源合同和 `toolbusd` 的流量准入：

- 安全、运动、系统流量优先于总线事务和普通流；
- I2C/SPI 事务默认属于交互类，`STREAM_DATA/CREDIT` 属于流类；
- 持续流量不得仅依据设备 SPI 时钟估算，必须以端到端最窄链路为准；
- 超出准入预算时立即返回背压/拒绝，不在主机或 MCU 无限排队；
- 大于 CAN 实际预算的流合同必须要求 USB 或未来的 Ethernet 数据面。

`kMaximumAtomicBusTransferBytes` 和 `kMaximumStreamChunkBytes` 当前均为 1024，
它们是防御性线协议上限，不代表推荐 CAN 块长。实际设备合同应设置得更小，
并由 Studio 根据链路模式与保留给运动/安全的预算给出合法候选值。

## 7. 超时、背压与故障隔离

- 每次总线事务携带显式超时，并且必须落在设备合同范围内；
- 每条物理总线使用固定容量队列，满队列立即返回 Busy；
- 重试由 Linux 设备驱动按设备语义决定，MCU 不做无界自动重试；
- I2C 恢复只影响所属总线，SPI 控制器复位只影响所属总线；
- 连续失败可把设备标为 Degraded/Failed，但不直接升级为节点离线；
- 流发送受字节信用约束；有损流按合同丢弃并累计计数，无损流必须背压；
- 流或总线遥测自身也受流量预算限制，不能挤占安全和运动业务。

## 8. 本轮线协议与兼容性

新增命令使用未占用范围：

- `0x0300`/`0x0301`：I2C 合同与原子事务；
- `0x0400`/`0x0401`：SPI 合同与原子事务；
- `0x0A00`～`0x0A05`：流合同、打开、数据、信用、状态和停止。

包头版本仍为 v1，旧命令和旧资源类型数值不变。旧节点会按既有行为返回未知命令；
新主机必须先从资源枚举/能力合同确认支持，再调用新增命令。后续若改变字段布局或
语义，应提升各自的合同版本；只有修改通用包头时才提升主协议版本。

## 9. 后续实现顺序

1. 实现 STM32 I2C/SPI BSP、有界队列、恢复和遥测，并让实体后端通过
   现有 Mock 对等测试；
3. 在 `toolbusd` 增加合同缓存、按父总线仲裁及链路预算提示；
4. 实现 USB Bulk 上的 STREAM 会话与信用流控；
5. 待真实吞吐需求明确后，再设计 Ethernet `LinkTransport` 和多链路路由；
6. 上层 Web 配套可借鉴 Moonraker + Fluidd 的分层，但只通过 `toolbusd` 的稳定
   服务 API 使用节点、资源、作业和遥测，不直接访问 SocketCAN。

## 10. Mock 板卡描述与数字孪生

板卡描述 schema v2 新增 `bus_resources`。v1 描述继续按原格式加载，但不能携带
该字段；这样测试数据可以明确声明它依赖的新能力，而不是静默改变 v1 的语义。

每个公开的 `i2c_bus`、`i2c_device`、`spi_bus`、`spi_device` 资源必须在
`bus_resources` 中恰好存在一份同 ID、同类型的合同。总线的
`parent_bus_resource_id` 必须为零；设备必须引用同协议类型的公开父总线。
加载器不依赖声明顺序，但会在完成解析后检查整张资源图。

Mock 专用静态字段包括：

- I2C 设备：`i2c_address` 与可选 `initial_data`；
- SPI 设备：`spi_mode`、`bits_per_word`、`spi_chip_select` 与可选
  `deterministic_response`；
- 总线和设备共同字段：时钟、最大事务长度、队列容量、超时区间、操作频率和
  能力标志。

初始数据和确定性响应不得超过合同的 `maximum_transfer_bytes`。加载后的
`DigitalTwin` 自动实例化 `MockBusBsp`，并由 `make_remote_core` 注入
`RemoteCore`，因此同一份版本化描述可以直接驱动协议级端到端测试。
同一 I2C 总线不能重复声明设备地址，同一 SPI 总线不能重复声明片选；设备的
时钟、事务长度、队列、超时、操作频率和能力标志均不得超出父总线合同。

故障场景新增一次性的 `bus_status` 动作：

```json
{
  "at_ms": 10,
  "action": "bus_status",
  "resource_id": 201326593,
  "status": "nack"
}
```

状态可为 `ok`、`nack`、`timeout`、`busy`、`fault` 或
`limit_exceeded`。事件只影响指定设备的下一次事务，用于确定性验证单设备故障
不会污染同节点的其他 I2C/SPI 设备。

内部转换器占用仍通过 `reserved_resources` 表达。例如 SPI 1 被 UART 扩展器
占用后，描述中不得再公开枚举实例 1 的旧式 `spi` 或新版 `spi_bus`。Linux 只
能看到转换后的 `expanded` 逻辑资源。

## 11. Studio 工程与机器可读能力共模

Studio 工程 schema v2 新增 `i2c.buses/devices` 和
`spi.buses/devices`。v0/v1 工程在加载时只会补充空组，不会自动公开
任何外设。工程只能引用 `gui/data/pin_catalog.json` 中完整的板级 AF
端点，不能自由拼接 SCL/SDA 或 SCK/MISO/MOSI。SPI 片选也必须从
该端点的 `chip_select_pins` 白名单中选择。

能力目录对每个公开端点声明：

- 控制器实例、成套引脚和公开属性；
- 最大时钟、最大事务长度、有界队列、超时区间和操作频率；
- repeated-start/recovery 或 full-duplex/keep-CS 标志上限；
- `backend_status=mock_only`，明确表示尚未通过 STM32 BSP 验收。

校验器会同时检查全工程引脚冲突、单控制器唯一公开、设备父总线类型、
同总线 I2C 地址/SPI 片选唯一性，以及子合同不超过父合同与板卡端点上限。
`/api/project/generate-mock-manifest` 可将通过校验的工程导出为 Mock
板卡描述 schema v2，同一份生成物由 Python 黄金清单测试和 C++
`DigitalTwin` 原子事务测试共用。
当前该导出器是“仅总线”竖切；若同一工程还含 GPIO、UART、运动、
PWM 或 WS2812，导出会列出未支持的资源类型并失败，不会生成丢字段的
部分板卡清单。待后续将其他资源也映射到版本化 Mock schema 后，再放开
混合工程导出。

Mellow FLY-D5 的 SPI1 以 `internal_controllers` 标记为内部 UART
扩展银行占用，且不出现在公开 SPI 端点列表。生成器仍把该占用写入
`reserved_resources`，因此即使能力目录未来误配，Mock 加载器也会再次拒绝
将同一 SPI 控制器向 Linux 枚举。

本轮没有添加 STM32 I2C/SPI 驱动。带非空总线图的工程调用实体
`.config`/固件生成路径时会明确拒绝，直到对应板卡的时钟、AF、开漏上拉、
DMA/中断和超时恢复策略在实体环境完成验收。

### 11.1 Studio 图形编辑器

“资源配置”页为 I2C 和 SPI 分别提供总线与设备编辑区。普通用户的
操作顺序是：

1. 选择板卡，添加总线；下拉框只显示该板的公开白名单端点。
2. 设置最大时钟、事务长度、队列、超时、操作频率和合同能力。
3. 添加设备并选择父总线；I2C 设置 7-bit 地址，SPI 从端点白名单
   选择片选，并设置 Mode 和位宽。
4. 可以用十进制或 `0xNN` 字节列表填写 I2C Mock 初始数据或 SPI
   Mock 确定性响应。

前端在每次编辑后立即检查名称、父子关系、端点、合同上限、地址/
片选唯一性、Mock 字节和全工程引脚冲突，然后延迟 180 ms 调用
`/api/project/validate` 使用后端同一套校验规则复核。导出 Studio 工程前会
再次同步复核；复核失败只显示错误，不创建下载文件，当前编辑状态也
不会被清空。

界面中的总线和设备均显示“仅数字孪生/Mock”标记；实体固件按钮仍
保留给无总线的已实现资源，若工程中含 I2C/SPI，后端会拒绝实体构建。
