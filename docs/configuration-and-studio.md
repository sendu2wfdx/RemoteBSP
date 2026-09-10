# 固件配置与 RemoteBSP Studio

## 最终配置模型

RemoteBSP 采用“图形化工程配置、生成板卡专用固件、烧录后重启生效”的模式。
系统不支持运行中动态申请 IO，也不再使用 RBSM 或 Flash A/B 资源槽在线改变
GPIO、UART、运动轴、PWM、WS2812 等接线关系。

```mermaid
flowchart LR
    A["RemoteBSP Studio 工程 JSON"] --> B["板卡与 MCU 能力校验"]
    B --> C["生成 Kconfig .config + 只读静态资源表"]
    C --> D["固件构建并校验板型"]
    D --> E["ELF / BIN / HEX / MAP"]
    E --> F["ST-Link / CAN Katapult / USB Katapult"]
    F --> G["重启后使用固定资源表"]
```

Studio 工程是配置真相；Kconfig 与生成的 C 只读表是生成器和构建系统之间的稳定接口。
`menuconfig` 仍保留给开发、CI、故障排查和没有 GUI 的场合，但普通用户不需要直接
编辑它。

## Kconfig 负责什么

由 Studio 生成的 `.config` 同时表达以下内容：

- MCU、板型、Flash/RAM 型号；
- HSE/LSE、系统主频和时钟策略；
- CAN、CAN-FD 或 USB APP 主链路、速率、固定引脚和设备标识；
- Katapult 与 APP 的 Flash 布局；
- GPIO、UART、运动、TMC2209 单线通信、PWM、定时位流等模块是否链接；
- 最大轴数、队列深度、UART 缓冲和波形通道数等静态预算；
- GPI/GPO、UART、STEP/DIR/EN/DIAG、TMC、PWM、WS2812 的具体映射与安全状态。

未启用模块不得进入最终 ELF，也不得占用静态 RAM、定时器、DMA 或中断资源。
资源映射编译进 APP，启动时建立一次；修改映射必须重新生成、构建、烧录并重启。

当前实体固件静态表纵切由 Studio 从同一份 `pin_catalog.json` 和通过统一校验的工程
生成 `remotebsp_static_resources.h`。schema v2 同时包含普通 GPIO 的编码引脚与启动
安全模式、硬件 UART 的逻辑端口/RX/TX/波特率边界，以及 PWM、定时位流的逻辑通道、
输出引脚和运行上限。三块 STM32 固件启动时完整校验同一张表，GPIO、UART_CREATE、
PWM 与定时位流运行路径也直接用它做白名单准入。

板卡目录为已实现端点声明唯一的 `kconfig_symbol`；生成器用该字段同时选择 `.config`
并在 C 表中生成编译期一致性断言，不再另写一套 endpoint_id 到 Kconfig 的映射。
资源数、板型或端点符号与 `.config` 不一致会编译失败；工程中伪造的端口、通道或
引脚，以及跨资源引脚冲突会在生成阶段失败。表文件和组合输入哈希也进入构建归档，
从而能发现工程、配置与静态表漂移。

运动槽仍走既有 Kconfig 路径。I2C/SPI 实体端点仍由 Studio 拒绝构建，不因公共
Core/HAL 骨架存在而宣称板卡 AF、DMA、上拉或片选已经验证。

## 固定功能引脚与冲突检查

Studio 并不允许任意 IO 承担任意功能。生成器需要同时遵守：

1. MCU 能力：GPIO、AF、UART、定时器通道、ADC、DMA、EXTI 的合法映射；
2. 板级约束：晶振、SWD、CAN、USB、LED、按键、收发器及外围电路的固定占用；
3. 后端能力：已经实现并验证的 UART、PWM、WS2812、运动定时器组合；
4. 全局冲突：重复引脚、定时器通道、DMA、容量、共享 EN 和性能预算；
5. 安全约束：输出初值、有效电平、故障安全电平、输入上下拉与滤波。

GUI 会隐藏固定占用和已选择的引脚，生成器仍会做独立校验，固件启动时保留最后
一道安全检查。错误配置不能部分激活。

当前生成器支持三块正式板卡、最多五轴、共享 EN、DIR 反相、TMC2209 地址 0–3、
静态 GPIO、一个已实现硬件 UART 端点、一个已实现 PWM 端点和一个已实现
WS2812/定时位流端点，并检查单轴及整板步频预算。后续扩展应补全机器可读的 MCU
复用图，而不是在 GUI 和 `main.c` 中分别维护规则。

## 运行时可以做什么

资源拓扑固定不表示资源状态固定。Linux 仍可通过逻辑资源 ID：

- 读写 GPIO；
- 收发 UART；
- 启停 PWM、修改实时占空比；
- 发送 WS2812 像素数据；
- 提交运动段、停止、复位和读取运动状态；
- 查询健康、遥测、资源合同和故障信息。

远程命令不携带物理引脚号，不能把正在运行的引脚改作另一类外设。

## 独立设备参数区

资源拓扑之外仍需要少量可维护的持久参数。当前使用 MCU 内部 Flash 模拟 EEPROM，
后续可通过同一存储接口切换为外部 EEPROM，而无需修改远程协议。

当前参数包括：

- SN 和持久 UUID；
- 硬件版本、制造批次和制造日期；
- 设备名称；
- ADC 校准数据。

参数存储采用两个擦除页、CRC、代数和提交标记，以完整快照方式切换，避免掉电留下
半份有效数据。F103 保留末尾 2 KiB，F072 和 G431 各保留末尾 4 KiB。Katapult 的
应用写入上限分别设置为 F103 `0x0801f000`、F072/G431 `0x0801e000`，正常 APP
升级不会擦除 health epoch 或设备参数双页。

设备参数区不保存 IO 映射、运动轴接线或资源拓扑，也不承担固件 A/B 分区。

参数可通过 `libremotebsp` 或 CLI 维护：

```sh
./build-wsl/remote-cli --node 1 param-status
./build-wsl/remote-cli --node 1 param-list
./build-wsl/remote-cli --node 1 param-get serial-number
./build-wsl/remote-cli --node 1 param-set serial-number RBSP-000001
# 12字节：gain_q16_16=1.0、offset_uv=0、reference_uv=3300000，小端编码。
./build-wsl/remote-cli --node 1 param-set adc0 hex:0000010000000000a05a3200
```

Studio Web 已通过 `toolbusd` 提供单节点参数的只读快照与备份，不直接访问 CAN。
修改与恢复保持显式 CLI：每次执行都必须匹配运行节点 UUID、当前参数 generation，
并提交固定维护确认短语；逐项写入使用 generation CAS，检测到并发变化即停止。
当前尚未提供网页写入口。后续生产工具还需增加权限分级、批量烧号、审计日志、
schema 迁移及实板掉电测试。

## Studio 当前完成度

已完成：

- 版本化 JSON 工程草案；
- 板卡目录、分组引脚选择、固定占用和重复选择过滤；
- GPIO、UART、运动轴、共享 EN、DIR 反相、TMC2209、PWM、WS2812 编辑；
- 后端 `/api/project/generate`，生成完整 Kconfig `.config`；
- 后端 `/api/project/build`，固定命令与目录、32线程构建、产物归档和白名单下载；
- 归档Studio工程、`.config`、ELF/BIN/HEX/MAP、日志、工具链/Git信息和SHA-256，
  并从链接器输出提取结构化Flash/RAM占用；
- 从统一能力目录和工程生成实体固件直接消费的普通 GPIO 只读表，归档其独立哈希
  与固件组合输入哈希，并在编译期拒绝串板；
- 生成可下载的中文接线表、JSON资源占用摘要、稳定排序资源清单及`SHA256SUMS`，
  接线资料和固件配置复用同一套后端静态校验；
- 提供两份版本化工程的只读比较接口与图形入口，区分 schema、板卡、资源增删、
  引脚/父子关系、合同和普通设置变化，并给出面向非技术用户的中文摘要；
- 从同一份比较结果确定性导出版本化 JSON、中文 Markdown 差异报告和
  `SHA256SUMS`；稳定哈希、输入迁移和截断数量均可复核，且不写服务器文件；
- 通过可导入/导出的版本化生产批次清单连接项目哈希、构建 ID、生产记录哈希和
  差异资料包哈希；确定性索引检查重复、缺失引用、内容变化和有界容量，但不冒充
  数据库、数字签名、烧录记录或实体验收；
- 为通过校验的批次清单提供有界本地历史：以内容哈希命名并原子写入，启动和读取时
  复核，损坏/错名/符号链接记录进入显式隔离区；按工程哈希、构建 ID、板卡和生产
  记录哈希检索，不把隔离项当作成功历史；
- 提供适合 CI/批产准备的非交互 CLI，直接复用工程校验、显式软件构建、批次生成/
  校验和历史保存后端；`deploy-stlink` 是必须显式调用的硬件命令，复核受保护固件后
  执行 ST-Link 写入/校验/复位，再从调用者指定的严格有界 JSON 身份文件核对四字段；
  其他命令都不隐式烧录或访问板卡；
- 新增独立版本化 `FirmwareIdentity` 命令并贯通 MCU、Mock、`libremotebsp`、
  `toolbusd` CLI 与 Studio；Studio 构建注入工程、配置和固件输入三个 SHA-256，
  非 Studio 构建按字段报告 unavailable；
- 提供只读 `inspect-runtime-identity`，同时核对 `node-list` 与 `firmware-identity`
  所指 UUID/板型，准确输出完整身份或缺项。该命令不执行烧录，也不把“读取完整”
  等同于“本次部署已验证”，所以 `deployment_verified` 始终为 false；
- Studio Web 提供设备参数只读快照与备份；显式 CLI 写入/恢复使用节点 UUID、
  generation CAS 和固定确认短语，网页尚不提供写入口；
- 生成确定、版本化的纯软件生产记录，关联工程/资源/资料包哈希及已有构建记录中
  的 Git、工具链、内存和产物证据；缺失、不匹配、未烧录和未实测均显式标记；
- 提供独立部署作业软件模块：只接受经构建记录哈希保护的固件，生成不经 shell 拼接的
  ST-Link 命令计划，支持有界烧录重试，并在调用者提供身份读取器后核对板型、工程哈希、
  配置哈希和固件哈希；身份不一致不会返回成功；
- 三块正式板卡通过上述Studio后端完成真实交叉编译；
- Mock GPIO、步进位置、PWM、WS2812 和故障状态可视化。

ST-Link 还提供纯离线部署预检工件 `REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1`。它从受保护
构建目录重新解析版本化 Studio 工程、完整 `firmware.config`、构建记录和 ELF，要求工程
及配置摘要与构建身份一致，并保存板卡专用 OpenOCD target、可选探针序列号、无 shell
拼接的精确 argv、四重预期身份、四类输入摘要和工件自身 SHA-256。生成与复核命令均明确
报告 `hardware_access=false`、`flash_performed=false`；任何计划字段或归档产物漂移都会
失败关闭。该工件用于烧录前审阅和归档，不是烧录成功或实体回读证明：

```text
studio_cli.py deployment-preflight-stlink --build-id <ID> --plan-output stlink-plan.json
studio_cli.py deployment-plan-validate --plan stlink-plan.json
```

USB Katapult 同样提供 `REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1` 离线预检工件。
除上述工程、完整配置、构建记录、APP `firmware.bin` 和四重身份外，它还绑定固定
`/dev/serial/by-id` 设备、外部 flashtool 的解析路径与 SHA-256，以及精确参数数组。
计划固定声明 `stage=katapult_usb_recovery`、`application_transport_active=false` 和
`transport_exclusive=true`：USB Katapult 是独立恢复阶段，不能与 RemoteBSP APP 的
CAN/CAN-FD 或 USB Vendor Bulk 运行态混作同一传输。离线生成和复核都不会打开 USB：

```text
studio_cli.py deployment-preflight-usb-katapult --build-id <ID> \
  --usb-device /dev/serial/by-id/<设备> --flashtool ./flashtool.py \
  --plan-output usb-katapult-plan.json
studio_cli.py deployment-plan-validate --plan usb-katapult-plan.json
```

CAN Katapult 使用独立的 `REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1`。它绑定 CAN
接口和规范化的小写 Katapult UUID，并固定 `targeting=direct_katapult_uuid`、
`broadcast_allowed=false`；空 UUID、`all` 或命令注入字符不会形成计划。工件同时绑定
8 KiB APP、flashtool 路径/摘要和精确 argv，并声明 `stage=katapult_can_recovery`、
运行 APP 传输未激活且恢复传输互斥。Katapult UUID 只是恢复阶段的定向目标，不冒充
RemoteBSP APP 重启后的设备 UUID；真正部署仍必须用四重身份和设备 UUID 回读收口。

```text
studio_cli.py deployment-preflight-can-katapult --build-id <ID> \
  --can-interface can0 --katapult-uuid <UUID> --flashtool ./flashtool.py \
  --plan-output can-katapult-plan.json
studio_cli.py deployment-plan-validate --plan can-katapult-plan.json
```

## 部署尝试与回读证据

离线计划和“已经烧录并核验”之间使用独立的
`REMOTEBSP_DEPLOYMENT_ATTEMPT_V1`，不能用计划文件冒充执行结果。初始记录只能是
`outcome=absent`，开始/结束时间、退出码和输出摘要均为空，且
`hardware_success_claimed=false`。执行整合层开始受控执行后可派生终态记录；即使工具因
本机配置错误而未能启动，也必须生成 `tool_invoked=false` 且带错误类型的 `failed` 终态，
不能丢失尝试。终态保存原始
计划 SHA-256、前一状态 SHA-256、UTC 开始/结束时间、工具退出码、stdout 字节数及
SHA-256；stdout 原文不进入记录。

工具失败时 execution 为 `failed`，未进行回读时 readback 保持 `absent`；工具成功但
回读失败时 readback 为 `failed` 并记录有界错误类型。只有工具确实被调用、退出码为零、
执行无错误、四重预期身份全部匹配且读到合法设备 UUID，才允许
`outcome=verified` 和 `hardware_success_claimed=true`。离线 CLI 故意不提供填写执行结果或
生成 verified 的选项，只能创建未执行记录并验证它和当前计划的绑定：

```text
studio_cli.py deployment-attempt-create --plan stlink-plan.json \
  --attempt-output attempt.json
studio_cli.py deployment-attempt-validate --plan stlink-plan.json \
  --attempt attempt.json
```

时间来自执行整合层并明确只是记录字段；本框架不把主机 UTC 伪装成可信时间证明。

受控执行命令 `deployment-execute` 会先重新验证 ST-Link、CAN Katapult 或 USB Katapult
计划，再调用对应的既有 deploy 路径。它必须同时出现 `--execute` 和精确确认短语
`EXECUTE_DEPLOYMENT_PLAN`，默认不会执行。工具以参数数组启动、不经过 shell；stdout 与
stderr 分别限制为 1 MiB，尝试记录只保存各自字节数和 SHA-256。工具非零退出、启动异常、
输出越界或回读异常都会形成原子保存的 `failed` 终态；工具成功但没有显式回读适配器的
四重身份与设备 UUID 结果仍不能成为 `verified`。

```text
studio_cli.py deployment-execute --plan stlink-plan.json \
  --identity-file runtime-identity.json --attempt-output attempt.json \
  --execute --confirmation EXECUTE_DEPLOYMENT_PLAN
```

当前自动测试只使用假工具和 Mock 回读适配器，没有连接或烧录实体板。

尚未完成：

- 把现有显式 ST-Link CLI 部署作业接入 Studio API/界面，并增加 CAN Katapult、
  USB Katapult 执行器；
- 将 `deploy-stlink` 的实体烧录作业与运行时 `FirmwareIdentity` 读取原子关联；当前
  身份读取已经贯通，但独立 `inspect-runtime-identity` 固定不作部署归因，测试仍包含
  注入式执行器/读取器，不能记作自动发现和实体回读闭环；
- 完整 MCU 引脚复用、定时器和 DMA 数据库；
- 将烧录回读结果、节点 UUID 和实体电气验收证据追加到生产记录；
- 设备参数的受控 GUI 写入/恢复页及生产审计。

## 推荐生成物

当前一次构建保存：

- Studio 工程 JSON 与最终 `.config`；
- Studio 生成的 `remotebsp_static_resources.h` 及其 SHA-256；
- `.elf`、`.bin`、`.hex`、`.map`；
- MCU、板卡、Git 提交、配置哈希和工具链版本；
- Flash/RAM 占用与构建日志；
- 烧录方式以及烧录后读取到的固件版本、UUID 和配置哈希。

构建、烧录后端必须与网页界面解耦，命令行、CI 和批量生产工具调用同一套接口。
