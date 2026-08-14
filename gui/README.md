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

生成器会检查板级保留引脚、重复引脚、端点合法性、共享 EN、TMC 引用、单轴与
整板步频、PWM 定时器频率以及定时位流端点。当前只允许选择已经实现的 UART、PWM
和 WS2812 后端；更多预设组合会在后端实现并验证后开放。

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

设备身份、制造信息和 ADC 校准值使用独立 EEPROM/Flash 仿 EEPROM 参数区，不属于
Studio 的 IO 工程。其设计见
[固件配置与 RemoteBSP Studio](../docs/configuration-and-studio.md)。
