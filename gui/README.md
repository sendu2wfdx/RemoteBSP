# RemoteBSP Studio

RemoteBSP Studio 是板卡专用固件的图形配置入口。它保存工程 JSON，检查引脚和
外设冲突，生成 Kconfig `.config`；烧录后，GPIO、UART、运动、PWM 和 WS2812
映射在本次固件中固定，不支持在线改变 IO 复用。

当前界面包括：

- 三块正式板卡的分组引脚选择、固定占用和重复选择过滤；
- GPI/GPO 名称、方向、上下拉、有效电平、初始/安全电平与输入消抖；
- 普通硬件 UART 端点和固定波特率；
- 最多五轴 STEP/DIR/EN/DIAG、DIR 反相、共享 EN；
- 每轴选择 STEP/DIR 或 TMC2209，并设置单线通信引脚和地址 0–3；
- PWM 与 WS2812 独立增删、端点预设、频率/占空比/极性或灯珠参数；
- 独立实时控制页原型；
- Mock GPIO、步进位置、运动队列、PWM、灯带和故障状态可视化；
- 工程 JSON 导出、完整 Kconfig `.config` 生成和一键固件构建。
- 从通过统一后端校验的版本化工程生成中文接线表、资源占用摘要、稳定资源清单
  和 SHA-256 校验文件，并打包下载。
- 在浏览器中粘贴或选择两份工程，查看 schema、板卡、资源增删、接线与合同参数
  的中文差异摘要。

生成器会检查板级保留引脚、重复引脚、端点合法性、共享 EN、TMC 引用、单轴与
整板步频、PWM 定时器频率以及定时位流端点。当前只允许选择已经实现的 UART、PWM
和 WS2812 后端；更多预设组合会在后端实现并验证后开放。

BluePill Plus与WeAct G431 Core当前均提供USART1、USART2、USART3三路普通硬件UART。USART1可选择
PA9/PA10或PB6/PB7重映射，USART2固定PA2/PA3，USART3固定PB10/PB11。固件采用
前N路静态裁剪，所以Studio按UART 0→1→2连续增加；移除中间端口时会同时移除
后续端口，避免生成稀疏且无法由当前固件表达的配置。

## 启动

```sh
cd /mnt/d/Documents/RemoteBSP
python3 gui/server.py
```

浏览器打开 `http://127.0.0.1:8765`。

连接 Mock 的只读数字孪生状态：

```sh
./build-wsl/mock_mcu/mock_mcu vcan0 classical \
  --visual-state /tmp/remotebsp-mock-state.json
python3 gui/server.py --state /tmp/remotebsp-mock-state.json
```

状态文件只由 Mock MCU 写入，Studio 只读。GUI 不直接访问 CAN；未来实时控制、
设备参数维护和烧录均通过独立后端及 `toolbusd` 完成。

内置动画会明确显示“演示模式 / DEMO”，不代表实体节点在线。只有使用
`--state`连接 Mock 状态文件时才显示“Mock MCU 实时数据”。当前实时控制页中的
GPIO、PWM 和 WS2812 操作也是本地预览，不会向实体板发送命令。

## G431 实板验收记录

2026-08-17 使用 WeAct STM32G431CBU6 Core、CANable2.5 和 ST-Link 验证：

- Studio 默认工程成功生成配置并以32线程构建，随后手工烧录到实体板；
- 三路硬件 UART 与一条 TMC2209 专用单线 UART 均可按静态配置创建；
- PC13 合法 GPIO、PC6 合法 PWM 可创建和操作，非法 GPIO/PWM 编号被板端拒绝；
- 单轴 `PA0 STEP / PA1 DIR / PB0 EN / PB1 TMC UART` 合同为单轴120 kstep/s、
  整板200 kstep/s，100 STEP 空载段完成且无故障、欠载或安全停机；
- 加入 `PA8 TIM1_CH1 + DMA` WS2812 后的完整组合成功构建，RAM 77.22%、
  Flash 43.09%；因未连接灯带，本轮只验证配置、冲突检查和链接，不算波形实测。

实体 STM32 当前尚未实现通用 `resource-list` 目录命令；各具体 GPIO、UART、PWM、
运动合同和状态命令可用。该缺口与 GUI 的真实节点接入一并列入待办。

## 构建与下载

网页中的“构建固件”会使用32个并行任务。构建完成后可分别下载：

- `studio-project.json`；
- `firmware.config`；
- `firmware.elf`、`firmware.bin`、`firmware.hex`、`firmware.map`；
- `build.log`和`build-record.json`。

产物写入`firmware/out/studio/<构建ID>/`。构建ID包含板卡ID和配置哈希；构建记录
包含Git状态、工具链版本、资源数、结构化Flash/RAM占用及每个文件的SHA-256。
构建完成后界面直接显示内存使用量和百分比。下载接口只能访问构建记录
列出的文件，工程内容不能拼接命令或读取任意本地路径。

## 工程资料包

“生成接线表与资源资料包”使用与 `/api/project/validate` 相同的工程迁移、板卡能力、
引脚冲突和静态预算校验。校验通过后下载的 ZIP 包固定包含：

- `<板卡ID>-接线表.md`：UTF-8 中文接线表，列出逻辑资源、信号、MCU 引脚、
  主要参数和后端状态；
- `<板卡ID>-资源占用摘要.json`：按资源类型计数，并列出已配置引脚、板卡固定
  占用、外设、运动步频预算和所有仅 Mock 资源；
- `<板卡ID>-稳定资源清单.json`：按稳定资源键排序的机器可读清单，保存工程哈希
  和与 JSON 排版无关的资源集合哈希；
- `SHA256SUMS`：上述三个文件的 SHA-256。

ZIP 条目使用固定顺序、时间戳和权限；同一规范工程与同一板卡目录会生成相同的
资料包字节和哈希，适合版本比较和生产前复核。I2C/SPI 会明确标记“仅 Mock
数字孪生”，资料包不会绕过固件构建路径对未实现 STM32 BSP 的拒绝。

自动化工具可调用 `POST /api/project/generate-reports`，请求体仍为
`{"project": <Studio工程>}`。响应格式为 `PROJECT_REPORTS_V1`，包含资料包的
Base64、文件名、字节数、ZIP SHA-256、资源集合 SHA-256，以及包内各文件的
名称、类型、字节数和 SHA-256。

## 纯软件生产记录

“生成生产记录（不烧录）”把当前规范工程、稳定资源清单和工程资料包绑定到一份
版本化 JSON 证据索引。可选填写 Studio 构建 ID；一次构建完成后页面会自动填入
该 ID，后端只读加载对应的 `build-record.json`。下载记录至少包含：

- 规范工程 SHA-256、资源集合 SHA-256、工程资料包 SHA-256 和资料包内文件哈希；
- 构建记录源文件 SHA-256、构建 ID、配置 SHA-256、Git revision/dirty 状态；
- CMake、Ninja、ARM GCC 版本，Flash/RAM 占用和固件产物哈希；
- 当前工程与构建记录、归档工程、归档配置的匹配结果；
- 固件烧录、硬件连接和实测状态。

未填写构建 ID 时状态为 `design_only`；记录缺字段、工具版本为 `unknown` 时为
`build_incomplete`；工程、板卡、资源数量或归档哈希不符时为 `build_mismatch`。
只有软件构建证据齐全且互相匹配时才是 `software_build_recorded`，这个状态仍明确
表示“尚未烧录、尚未硬件实测”。记录不含生成时钟，因此同一工程、资料目录和
构建记录会生成相同字节及 SHA-256。

自动化工具调用 `POST /api/project/generate-production-record`，请求体为
`{"project": <Studio工程>, "build_id": <可选构建ID>}`，响应格式为
`PRODUCTION_RECORD_V1`。HTTP 请求总上限为 256 KiB，读取的构建记录最多
128 KiB、最多 32 个产物；接口不写文件、不构建、不烧录，也不连接节点。

## 工程比较

配置页底部可粘贴或选择两份工程 JSON，也可把当前编辑工程放到任意一侧。
`POST /api/project/compare` 接收 `{"left": <旧工程>, "right": <新工程>}`，
两侧分别执行 schema 迁移、规范化和与配置生成器相同的完整静态校验，然后返回
`PROJECT_COMPARISON_V1`：

- 工程级差异：原始/规范 schema 和目标板卡；
- 资源级差异：按稳定槽位区分新增、删除、修改和未变；
- 字段级差异：把引脚/父总线等接线变化、总线合同变化和普通设置变化分类；
- 中文 `headline` 与逐项 `summary`，网页不要求用户理解 JSON path；
- `comparison_sha256`：对结构化比较结果计算的稳定 SHA-256，便于自动化留档核对。

比较全程只读且仅在内存中执行，不写文件、不生成配置、不构建固件、不连接节点。
HTTP 请求总大小上限仍为 256 KiB；每侧规范 JSON 最多 128 KiB、256 项资源，
字段变化详情最多展开 512 条，超过时响应会显式标记截断。

“导出差异资料”调用 `POST /api/project/export-comparison`，请求体与比较接口相同。
后端只执行一次比较，再从同一个 `PROJECT_COMPARISON_V1` 对象生成确定性 ZIP：

- `<A板卡>-to-<B板卡>-工程差异-v1.json`：与响应中的 `comparison` 完全相同；
- `<A板卡>-to-<B板卡>-工程差异报告-v1.md`：面向普通用户的中文概览和逐项变化；
- `SHA256SUMS`：上述两份文件的 SHA-256。

JSON 和 Markdown 都记录比较结果哈希、两侧规范工程哈希、原始/规范 schema、迁移
步骤、输出边界及截断数量。ZIP 使用固定条目顺序、时间戳和权限；相同输入与板卡
目录会得到相同字节及归档 SHA-256。响应格式为
`PROJECT_COMPARISON_EXPORT_V1`，含原始比较对象、归档 Base64、字节数和各文件哈希。
接口继承比较入口的大小、资源数和字段详情上限，全程只在内存中处理，不写服务器
文件、不构建、不烧录，也不连接板卡。

## 生产批次与历史索引

“生产批次与历史追溯”是最小可信的可移植 Manifest，不是隐藏数据库。用户可选择或
粘贴既有生产记录，以及差异资料包中的工程差异 JSON；网页也可直接加入本次会话
刚生成的生产记录和工程差异。后端从这些证据生成固定包含以下文件的 ZIP：

- `<批次ID>-生产批次清单-v1.json`：版本化 `REMOTEBSP_PRODUCTION_BATCH_V1`；
- `<批次ID>-生产批次摘要-v1.md`：面向非技术用户的中文记录、差异和限制说明；
- `SHA256SUMS`：清单与摘要文件的 SHA-256。

批次清单按生产记录哈希稳定排序，并按比较结果哈希排序差异资料。每份生产记录建立
项目 SHA-256、构建 ID、板卡 ID 和记录 SHA-256 四类索引；差异资料关联两侧工程
SHA-256、比较结果 SHA-256 和确定性差异资料包 SHA-256。导入资料包内的清单 JSON
后，Studio 会重新计算清单、记录组、差异组和追溯索引校验值，并报告：

- 重复的生产记录、构建 ID、比较结果或差异资料包；
- 非设计记录缺失构建 ID / 构建记录哈希；
- 差异资料引用的工程在本批次中缺少对应生产记录；
- 工程与板卡引用冲突、非稳定排序、索引漂移和内容哈希不匹配。

自动化入口为 `POST /api/production-batch/export`，请求字段是 `batch_id`、`name`、
可选 `note`、`production_records` 数组和 `comparison_exports` 数组；响应格式为
`PRODUCTION_BATCH_EXPORT_V1`。导入校验使用
`POST /api/production-batch/validate` 和 `{"manifest": <批次清单>}`，响应格式为
`PRODUCTION_BATCH_VALIDATION_V1`。单批最多 32 份生产记录、64 份差异资料；单份生产
记录最多 64 KiB，批次清单最多 128 KiB，HTTP 请求总上限仍为 256 KiB。

两个接口都只在内存中运行，不写服务器持久目录、不读取批次外部文件、不构建、
不烧录且不访问硬件。SHA-256 能检查内容一致性，但不是数字签名、可信时间戳或实体
生产验收；清单中的烧录和实测状态仍以生产记录的明确声明为准。

## 本地生产历史

批次清单经过校验后，可通过“保存当前批次”写入 Studio 本机历史。默认目录是
`firmware/out/studio-history/`，也可在启动时用 `--history-root` 指定专用目录。
历史不是远程数据库；只保存规范批次 Manifest，不复制固件、构建产物或硬件数据。

安全与恢复规则如下：

- 历史文件始终以 64 位 `manifest_sha256` 命名，批次 ID、名称和搜索词不会参与路径；
- 写入使用同目录临时文件、文件 `fsync`、原子替换和目录 `fsync`；失败的临时文件
  会清理，同一清单重复保存是幂等操作；
- 每条记录最多 128 KiB，默认最多 256 条、总计 16 MiB；目录审计最多处理 1024
  个条目，超过即拒绝启动而不是跳过检查；
- 新目录会写入专用格式标记；非空但无标记的目录会拒绝使用，避免误配置后移动普通
  JSON 文件；隔离目录也必须是普通目录，不能是符号链接；
- 启动时逐条核对规范 JSON、Manifest/分组/索引哈希和“哈希.json”文件名；损坏、
  错名、残留临时文件或符号链接会移动到 `quarantine/`，并在界面明确显示；
- 从历史读取时会再次校验；启动后被修改的记录会从检索索引移除并隔离，不会作为
  成功结果返回。

本地 API 包括：

- `POST /api/production-history/save`：请求 `{"manifest": <批次清单>}`，原子保存；
- `GET /api/production-history/status`：返回有效记录数、容量和启动隔离详情；
- `GET /api/production-history/search`：用 `query`、`field`、`limit` 搜索；`field` 可为
  `project_sha256`、`build_id`、`board_id`、`record_sha256` 或 `all`；
- `GET /api/production-history/record/<manifest_sha256>`：复核并读取一份批次清单。

搜索词最多 128 个字符，一次最多返回 50 条；空搜索列出当前全部有效记录。网页提供
字段选择、前缀/片段检索和清单下载。隔离只影响本地批次索引，不声称自动修复、构建、
烧录或完成硬件验收。

命令行和CI可以调用同一后端：

```sh
python3 gui/firmware_builder.py --project path/to/project.json --jobs 32
python3 gui/firmware_builder.py --all-board-defaults --jobs 32
```

## 仅生成 `.config`

网页中的“生成固件 .config”按钮调用 `/api/project/generate`。也可用命令行：

```sh
python3 gui/project_config.py \
  --project path/to/project.json \
  --output firmware/.config \
  --catalog gui/data/pin_catalog.json
```

随后也可手工按普通固件流程构建。当前尚未实现烧录和回读确认，界面不会宣称
已经把配置部署到节点。

工程后端会先执行 schema 迁移与规范化。没有版本字段的早期草案按 v0 依次迁移到
当前 v2；未来版本会明确拒绝，避免错误降级。`/api/project/inspect` 可在不生成
文件的情况下返回资源摘要和稳定工程 SHA-256；生成、构建接口返回同一工程身份，
构建记录同时保存工程哈希和最终固件配置哈希。

当前 `gui/server.py` 只属于 Studio 本地原型，`/api/state` 也只是演示或 Mock 的
只读快照，不是面向设备运行的网络服务。未来类似 Moonraker 的常驻 Runtime 与
类似 Fluidd 的 Web UI 边界见
[Studio 与上位机运行时边界](../docs/studio-runtime-design.md)。

设备身份、制造信息和 ADC 校准值使用独立 EEPROM/Flash 仿 EEPROM 参数区，不属于
Studio 的 IO 工程。其设计见
[固件配置与 RemoteBSP Studio](../docs/configuration-and-studio.md)。
