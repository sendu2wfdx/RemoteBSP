# Katapult Bootloader 与 USB 调试

## 当前实现

本项目采用 CanBoot 后续维护版本
[Katapult](https://github.com/Arksine/katapult)。Katapult 作为独立的
GPLv3 Bootloader 构建和烧录，不与 RemoteBSP APP 链接；仓库脚本把上游固定在
提交 `ec59b9bb9ad6c2ec8d4dc6831fbc77f0b308e29e`，避免上游变化导致固件不可复现。

项目在固定上游提交之上应用
`bootloader/patches/katapult-dual-can-usb.patch`，为三颗 MCU 生成一份双模式
Katapult：

| 目标 | 正常升级模式 | 应急恢复模式 | APP 起始地址 |
|---|---|---|---:|
| STM32F072RBT6 / Mellow FLY-D5 | Classical CAN，PB8/PB9，1 Mbit/s | USB FS，PA11/PA12；双击 RESET | `0x08002000` |
| STM32F103CBT6 / WeAct BluePill Plus | Classical CAN，PB8/PB9，1 Mbit/s | USB FS，PA11/PA12 | `0x08002000` |
| STM32G431CBU6 / WeAct STM32G431CBU6 Core | FDCAN 外设的 Classical CAN，PB8/PB9，500 kbit/s | USB FS，PA11/PA12；PC13 恢复键 | `0x08002000` |

标准 Katapult 的通信接口是编译期单选。项目补丁同时链接 CAN 和 USB 后端，
但根据进入原因只初始化其中一个：

- APP 收到 `BOOTLOADER_ENTER`：写入原 Katapult 请求签名并进入 CAN，
  供正常在线升级；
- APP 收到 `BOOTLOADER_ENTER_USB`：写入项目的 USB 请求签名并进入 USB，
  适合设备安装后不方便操作恢复键的情况；
- 复位时板载恢复键为高电平：进入 USB，供 CAN 通信失效时恢复；BluePill
  使用 PA0，WeAct STM32G431CBU6 Core 使用 PC13；
- APP 无效：默认进入 USB；
- 正常复位：直接启动 APP；
- 双击 NRST 进入 USB 的逻辑已实现，但默认关闭，避免正常启动增加约 500 ms
  检测等待；需要时可在 Katapult menuconfig 中启用。

STM32F103 的 USB 与 bxCAN 共用 512 字节专用 SRAM。双模式 Bootloader 不会
同时初始化两者，并使用共享中断分派器处理 F103 的 USB/CAN 共用中断入口。
F103 APP 仍然只运行 CAN，不启用 USB CDC。

STM32G431 APP 可以同时运行 PB8/PB9 上的 FDCAN 和 PA11/PA12 上的 USB CDC；
进入 Bootloader 后 APP 已停止，Katapult 同样只启动所选的一个升级接口。

## Flash 布局

```text
0x08000000  +------------------------------+
            | 双模式 Katapult Bootloader   |  最大 8 KiB
0x08002000  +------------------------------+
            | RemoteBSP APP                |
            | 中断向量表也位于 0x08002000   |
0x08020000  +------------------------------+  128 KiB 末端
```

链接脚本、中断向量重定位和 Katapult 的应用偏移必须同时保持为 `0x2000`。
F103/G431 使用 VTOR；没有 VTOR 的 F072 把向量复制到 SRAM 并重映射地址 0。
`*-katapult.bin` 是只供 Bootloader 在线升级的 APP，不应从 `0x08000000`
直接烧录。`*-factory.bin` 才是供 ST-Link 首次烧录的 Bootloader+APP 合并镜像。

## 准备并构建

在名为 `Ubuntu` 的 WSL 中执行：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh
bash scripts/fetch_katapult.sh
bash scripts/build_bootloader.sh all
bash scripts/build_firmware.sh mellow-fly-d5-katapult
bash scripts/build_firmware.sh weact-bluepill-plus-katapult
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-katapult
bash scripts/build_factory_images.sh
```

主要输出：

- `katapult-stm32f072_dual.bin`
- `katapult-stm32f103_dual.bin`
- `katapult-stm32g431_dual.bin`
- `remotebsp-*-katapult.bin`：APP 在线升级包
- `remotebsp-*-katapult-dual-factory.bin`：ST-Link 首次整片烧录包

打包脚本会检查 Bootloader 不超过 8 KiB、APP 不越过 128 KiB Flash，并使用
擦除值 `0xFF` 填充二者间隙。

## 首次使用 ST-Link 烧录

F103 双模式量产镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32f103-bluepill-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

FLY-D5 双模式量产镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32f072-fly-d5-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

G431 双模式量产镜像：

```powershell
STM32_Programmer_CLI.exe -c port=SWD `
  -w firmware\out\remotebsp-stm32g431-katapult-dual-factory.bin 0x08000000 `
  -v -rst
```

首次验证建议只连接一块目标板。确认 APP 能被 `toolbusd` 发现后，再测试远程进入
Bootloader；不要在尚未记录板卡身份时让多块未分配的 Bootloader 同时停留在
总线上。

## 通过 CAN 在线升级

APP 先回复成功响应，等待约 100 ms，再写入 Katapult RAM 请求签名并复位。
命令带固定确认串且受节点请求去重缓存保护；主机重试不会触发两次副作用：

```sh
cd /mnt/d/Documents/RemoteBSP
./build-wsl/remote-cli --node 1 bootloader-enter
```

节点会暂时从 RemoteBSP 心跳表离线。查询当前处于 Katapult 的设备：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
python3 vendor/katapult/scripts/flashtool.py -i can0 -q
```

记录查询得到的 Katapult UUID，然后升级指定节点。F103 示例：

```sh
python3 vendor/katapult/scripts/flashtool.py \
  -i can0 -u <Katapult-UUID> \
  -f out/remotebsp-stm32f103-bluepill-katapult.bin
```

G431 示例将文件改为 `out/remotebsp-stm32g431-katapult.bin`。升级完成并复位后，
APP 恢复心跳，`toolbusd` 应在 2 秒内重新确认在线。

多板场景必须始终先用 RemoteBSP 节点 ID 让一块指定板进入 Bootloader，再按
Katapult UUID 定向升级。不要对多块未记录 UUID 的板执行广播升级。首次升级时
应建立“RemoteBSP 16 字节 UUID ↔ Katapult UUID”的资产映射。

## 通过 USB 在线升级

USB Bootloader 使用 MCU 原生 USB，而不是 ST-Link 的虚拟串口。接线为
PA11=`USB_DM`、PA12=`USB_DP` 和 GND；成品板还应按 USB 硬件规范加入串联电阻、
ESD 和合适的连接器。

CAN 通信正常时，可以让指定 RemoteBSP 节点通过命令进入 USB Katapult。设备
必须已经连接 MCU 原生 USB 数据线：

```sh
cd /mnt/d/Documents/RemoteBSP
./build-wsl/remote-cli --node 1 bootloader-enter-usb
```

APP 会先通过 CAN 回复 `ok`，约 100 ms 后写入 USB 请求签名并复位。随后 USB
枚举为 `1d50:6177` Katapult，CAN 心跳暂时停止。该命令同样受请求去重缓存保护，
主机重试不会重复触发复位。

如果 CAN 已经完全不可用，仍可使用不依赖命令的应急步骤。BluePill 的恢复键
是 PA0，WeAct STM32G431CBU6 Core 的恢复键是 PC13；两者都是高电平有效并使用
MCU 内部下拉：

1. 按住对应板卡的恢复键；
2. 按下并释放 NRST，或重新上电；
3. USB 枚举为 `1d50:6177` Katapult 后释放恢复键；
4. 在 Linux 中使用稳定的 `/dev/serial/by-id` 路径升级。

```sh
python3 vendor/katapult/scripts/flashtool.py \
  -d /dev/serial/by-id/<Katapult设备> \
  -f out/remotebsp-stm32f103-bluepill-katapult.bin
```

G431 将文件改为 `out/remotebsp-stm32g431-katapult.bin`。CAN 和 USB 模式写入
同一个 APP 包；区别只在进入原因和传输接口。项目同时提供受固定确认串和请求
去重保护的 APP 命令入口，以及不依赖 CAN 的物理恢复键入口。

## APP 的 USB CDC 调试输出（仅 STM32G431）

STM32G431 的 Katapult APP 配置默认启用
`USB 调试 -> USB CDC 非阻塞调试输出`。STM32F103 的 Kconfig 会隐藏该选项，
防止产生硬件上无法工作的 USB+CAN 组合。G431 的调试口具备以下隔离行为：

- 仅用于 APP 文本调试，不承载 RemoteBSP 协议；
- 主机未连接时，CAN/CAN-FD 继续运行；
- TX 环形缓冲满时丢弃新调试字节并增加计数，不等待 USB；
- USB 初始化失败不会令 APP 进入致命错误；
- USB RX 当前被丢弃，避免调试入口意外控制硬件。

Linux 上可查看：

```sh
ls -l /dev/serial/by-id/
picocom -b 115200 /dev/serial/by-id/<RemoteBSP-USB-Debug设备>
```

CDC 显示的波特率不决定 USB 实际传输速度。APP 启动后会输出一行
`RemoteBSP ... APP ready`。固件代码可调用
`rbsp_usb_debug_write()` 或 `rbsp_usb_debug_write_text()`，主循环持续调用
`rbsp_usb_debug_poll()` 完成后台发送；丢弃计数由
`rbsp_usb_debug_dropped_bytes()` 返回。

开发配置暂用 ST 的 `0483:5740`。产品化前必须在 menuconfig 中换成有权使用的
VID/PID，并确定稳定的产品字符串和驱动策略。

## 安全边界

当前目标是可靠升级通路，不是安全启动。Katapult 在线升级没有实现项目级固件
签名、加密或操作者认证；能接触 CAN 或 USB 的实体可能尝试改写 APP。产品阶段
必须结合威胁模型选择签名镜像、安全启动、总线物理隔离和升级授权策略。在完成
这些工作前，不应把当前 Bootloader 视为防篡改边界。

## 2026-07-29 STM32F103 单接口基线实机验收

以下记录完成于双模式补丁之前，用来证明 CAN Katapult、USB Katapult 和 APP
各自的底层通路正常。测试硬件为 STM32F103CBT6、ST-Link/V2.1 和使用
Candlelight/gs_usb 固件的 CANable2.5，Classical CAN 速率为 1 Mbit/s：

- ST-Link 首次写入并校验
  `remotebsp-stm32f103-bluepill-katapult-can-factory.bin` 成功；
- Bootloader 能跳转到位于 `0x08002000` 的 APP，节点发现、分配、心跳、
  PING、能力查询和 GPIO 均正常；
- RemoteBSP UUID 为
  `50ff6d065048865745231367cb030100`；
- 通过 `bootloader-enter` 远程复位后，Katapult UUID
  `70fa76b1eae9` 可被 flashtool 查询；
- WeAct BluePill Plus 的 USB Katapult 枚举为 `1d50:6177` 和 Windows
  `COM20`；CAN APP 运行时主动拉低 PA12 断开板载 D+ 上拉，不再产生
  “未知 USB 设备（设备描述符请求失败）”；
- PB2 按高电平 LED 输出验证为 `1 → 0`，PA0 使用内部下拉时空闲读数为 `0`；
- flashtool 确认 Application Start 为 `0x08002000`，通过 CAN 写入 25 页，
  SHA 校验成功，复位后原 RemoteBSP 节点自动恢复；
- 升级后 100 次顺序 PING、20 次 GPIO 往返、4 个并发客户端各 25 次请求均
  无失败；2023 字节最大载荷 PING 往返 184 ms；
- 最终 can0 保持 ERROR-ACTIVE，TX/RX 错误计数、丢包、仲裁丢失和 bus-off
  均为 0。

后续把 WeAct BluePill Plus 原生 USB 口接入主机后，进一步确认了 USB Katapult
可以正常枚举；F103 APP 同时初始化 USB 与 CAN 时则无法枚举。该现象与芯片的
USB/bxCAN 共用 SRAM 限制一致，现已通过 Kconfig 和默认配置禁止该组合。ST-Link
VCP 不能替代 MCU 原生 USB。

## 双模式构建验收

双模式补丁已从固定上游提交的干净副本自动应用并交叉编译：

- STM32F103 双模式 Katapult：6740 字节；
- STM32G431 双模式 Katapult：6392 字节；
- 两者都小于 8192 字节，APP 起始地址保持 `0x08002000`；
- 已验证最终 Kconfig 同时启用 `USBSERIAL`、`CANSERIAL` 和
  `DUAL_CAN_USB`；
- 干净补丁复现测试为
  `firmware/tests/test_katapult_dual_patch.sh`。

## 2026-07-29 STM32F103 双模式实机验收

双模式工厂镜像已经使用 ST-Link/V2.1 写入 STM32F103CBT6，CubeProgrammer
写后校验通过。覆盖前保存了完整 128 KiB Flash 备份。实测结果如下：

- 工厂镜像能够从双模式 Katapult 跳转到 `0x08002000` APP，RemoteBSP 节点
  UUID 仍为 `50ff6d065048865745231367cb030100`；
- `bootloader-enter` 通过 CAN 选择 Katapult，查询到 UUID
  `70fa76b1eae9`；
- CAN Katapult 写入 APP 共 14 页，校验 SHA 为
  `7D9879E7C7D2F0C672E1871951325A2B35DF6338`，复位后心跳和 PING 恢复；
- `bootloader-enter-usb` 无需操作 PA0，命令回复 `ok` 后原生 USB 枚举为
  `1d50:6177`、Windows `COM20`，Ubuntu 稳定路径包含 MCU UID；
- USB Katapult 同样写入 14 页并得到相同 SHA，复位后 APP 恢复；
- 从 USB 升级后的 APP 再次通过命令进入 CAN Katapult，第二轮 CAN 写入、
  校验和 APP 恢复均成功，证明两个后端可以连续切换。

单节点测试总线上，进入 USB Katapult 后没有任何 CAN 节点为 `toolbusd` 的周期
发现帧提供 ACK，CANable2.5/gs_usb 会累积到 ERROR-PASSIVE；USB 升级完成后需要
把 `can0` 下线再上线才能恢复。多节点总线通常仍有其他节点 ACK，但守护进程
仍应增加总线错误监控和自动恢复，不能依赖这一条件。CAN Katapult 会正常 ACK，
因此 CAN 在线升级过程和升级后的接口都保持 ERROR-ACTIVE。

BluePill 的 PA0 物理恢复入口尚未在本轮按键操作中复测；它不影响已经通过的
命令 USB 入口。WeAct STM32G431CBU6 Core 已完成 HSE、CAN-FD APP、PC6 LED 和
PC13 内部下拉实测；PC13 进入 USB Katapult 及 CAN/USB 升级仍待切换验收。
