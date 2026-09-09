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
