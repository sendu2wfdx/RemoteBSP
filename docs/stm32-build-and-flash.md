# STM32 固件编译与烧录

## 当前可运行范围

当前固件已经为以下目标生成可烧录文件：

| 目标 | 总线模式 | 已接通的远程功能 |
|---|---|---|
| STM32F103CBT6 | Classical CAN，500 kbit/s | 发现、分配、心跳、PING、GET_INFO、GET_CAPABILITY、GPIO、UART 0 |
| STM32G431CBU6 | CAN-FD，500 kbit/s + 2 Mbit/s BRS | 发现、分配、心跳、PING、GET_INFO、GET_CAPABILITY、GPIO |

STM32F103 的 UART 0 已接到 USART1，使用中断驱动的 RX/TX 环形缓冲。G431
的真实 UART 驱动尚未接入，因此当前 G431 固件仍不会报告 UART 能力。

Bluepill 专用配置 `stm32f103_bluepill_defconfig` 把 CAN 重映射到
PB8/PB9，避开板载 USB 对 PA11/PA12 的占用；UART 0 默认使用
PA9(TX)/PA10(RX)。

## Ubuntu 依赖

在名为 `Ubuntu` 的 WSL 发行版中安装：

```sh
sudo apt update
sudo apt install -y \
    cmake ninja-build git python3 python3-pip \
    gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
python3 -m pip install --user kconfiglib
```

准备 ST 官方依赖：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/fetch_stm32_deps.sh
```

脚本只下载 CMSIS Core、对应 MCU 的 CMSIS Device 和 HAL，不下载 USB、中间件
或示例工程。

## 使用默认配置编译

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/build_firmware.sh all
```

也可以只编译一个目标：

```sh
bash scripts/build_firmware.sh f103
bash scripts/build_firmware.sh bluepill
bash scripts/build_firmware.sh g431
```

输出位于 `firmware/out`，包括 ELF、Intel HEX、裸 BIN 和链接 MAP。

## 使用 menuconfig

先选择一个完整的初始配置，再进入菜单：

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
cp configs/stm32f103cbt6_defconfig .config
python3 scripts/menuconfig.py
```

G431 将第一条命令中的文件改为
`configs/stm32g431cbu6_defconfig`。保存后编译：

```sh
cmake -S . -B build-custom -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
    -DRBSP_CONFIG="$PWD/.config"
cmake --build build-custom
```

目前已验证的位时序是：

- F103：72 MHz，CAN 外设时钟 36 MHz，500 kbit/s，采样点约 88.9%。
- G431：FDCAN 内核时钟 170 MHz，仲裁段 500 kbit/s，数据段 2 Mbit/s，
  两段采样点均约 82.4%。

配置成其他速率时，编译期会检查是否能被当前时序精确生成；不能精确生成就会
停止编译，而不是悄悄使用错误速率。

F103 的 `STM32F103 UART 0 引脚` 菜单可以在以下两组 USART1 引脚间选择：

- PA9(TX)/PA10(RX)，默认值。
- PB6(TX)/PB7(RX)，启用 USART1 重映射。

被 UART 选中的引脚会从通用远程 GPIO 池中保留，不能再通过 `GPIO_CREATE`
重新配置。

## 使用 ST-Link 烧录

如果已安装 STM32CubeProgrammer，F103 和 G431 使用相同命令：

```powershell
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32f103cbt6.hex -v -rst
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32f103-bluepill-pb8-pb9.hex -v -rst
STM32_Programmer_CLI.exe -c port=SWD -w firmware\out\remotebsp-stm32g431cbu6.hex -v -rst
```

烧录前连接 `SWDIO/PA13`、`SWCLK/PA14`、`NRST`、`GND` 和目标电平
`VTref/3.3V`。

## 使用 DAPLink/CMSIS-DAP 烧录

OpenOCD 示例：

```sh
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg \
  -c "program firmware/out/remotebsp-stm32f103cbt6.elf verify reset exit"

openocd -f interface/cmsis-dap.cfg -f target/stm32g4x.cfg \
  -c "program firmware/out/remotebsp-stm32g431cbu6.elf verify reset exit"
```

## GPIO 编号

远程 GPIO 使用 `端口序号 * 16 + 引脚号`：

| GPIO | 编号 | GPIO | 编号 |
|---|---:|---|---:|
| PA0 | 0 | PA15 | 15 |
| PB0 | 16 | PB15 | 31 |
| PC13 | 45 | PD0 | 48 |

## STM32F103 实体 UART

当前 UART 0 使用 256 字节 RX 和 256 字节 TX 存储区。环形缓冲保留一个空槽，
所以每次 `UART_WRITE` 最多原子接收 255 字节；空间不足时整段返回
`RESOURCE_FAILED`，不会发送半条报文。`UART_READ` 为保证响应可去重重放，
单次最多取 231 字节，更长数据可以连续读取。

ST-Link 虚拟串口接线：

| ST-Link VCP | STM32F103 |
|---|---|
| TX | PA10 / USART1_RX |
| RX | PA9 / USART1_TX |
| GND | GND |

创建并测试 UART：

```sh
./build-wsl/remote-cli --node 1 get-capability
./build-wsl/remote-cli --node 1 uart-create 0 115200 8 none 1
./build-wsl/remote-cli --node 1 uart-write 1 hello
./build-wsl/remote-cli --node 1 uart-write-hex 1 010300000002c40b
./build-wsl/remote-cli --node 1 uart-read 1 231
```

2026-07-28 使用 ST-Link 的 COM18 实测：

- CAN→USART1 和 USART1→CAN 文本双向一致。
- 包含 `00` 的 `00..FE` 共 255 字节二进制序列双向逐字节一致。
- 256 字节 TX 写入被完整拒绝，COM18 收到 0 字节，节点和 CAN 心跳保持正常。

固件会拒绝 CAN/FDCAN 引脚和 SWD 引脚。F103 还会按照 LQFP48 的实际键合
管脚拒绝不存在的 GPIO；G431 首版保守开放 GPIOA/GPIOB。

## 首次上板检查顺序

1. 只连接 SWD，确认能够读出芯片 ID、烧录并复位。
2. 不连接总线时检查 TXD 静态电平和 STB 是否为低。
3. 接一块 USB-CAN，先只接一个节点并在总线两端各放 120Ω。
4. F103 配置 Classical CAN 500 kbit/s；G431 配置 CAN-FD、仲裁段
   500 kbit/s、数据段 2 Mbit/s，并启用 BRS。
5. 启动 `toolbusd`，确认节点在 2 秒内被发现且每 500 ms 更新心跳。
6. 执行 `ping`、`get-info`，最后再创建和读写一个未保留的 GPIO。

## CANable2 原厂 SLCAN 固件

CANable2 重新连接后可能在 `ttyACM0`、`ttyACM1` 等编号之间变化，因此实机
测试使用 `/dev/serial/by-id` 稳定路径。仓库脚本会自动查找唯一的 CANable2，
以 500 kbit/s 建立 `can0`，并把发送队列从默认的 10 调整为 1024：

```sh
cd /mnt/d/Documents/RemoteBSP
sudo bash tests/hardware/start_canable2_slcan.sh can0 6 1024
```

经典 CAN 的协议包会分成多个 8 字节帧。`NODE_ASSIGN` 当前需要 15 个分片；
如果保留 slcan 默认的 `txqueuelen=10`，SocketCAN 写入虽然可能返回成功，尾部
分片仍会在队列层被丢弃，节点会长期停在 `ready=0`。协议最大包为 2048 字节，
经典 CAN 最多需要约 683 个分片，因此实机脚本默认使用 1024 帧队列。
F103 固件也按完整的 2048 字节协议上限构建；当前 Bluepill 配置占用
10808 字节 SRAM（52.77%），仍保留约 9.4 KiB 给中断栈和后续外设缓冲。

启动守护进程并验证实体节点：

```sh
./build-wsl/toolbusd/toolbusd can0 classical /tmp/toolbusd.sock

./build-wsl/remote-cli node-list
./build-wsl/remote-cli --node 1 ping hello
./build-wsl/remote-cli --node 1 get-info
./build-wsl/remote-cli --node 1 get-capability
```

实体链路压力测试可以同时覆盖顺序 PING、GPIO 往返、并发客户端和大包分片：

```sh
bash tests/hardware/physical_can_stress.sh \
  /tmp/toolbusd.sock 1 200 50 0 4 50 2023
```

最后一个参数是大载荷 PING 的字节数。协议包上限为 2048 字节，扣除 24 字节
协议头和 1 字节响应状态码后，PING 可成功回显的最大载荷为 2023 字节；
2024 字节会在主机 API 层立即拒绝，不会向总线发送一个必然无法构造响应的
请求。

2026-07-28 的 Bluepill + TJA1051/3 + CANable2 实测中，2023 字节载荷连续
10/10 次成功，单次往返约 425～451 ms；8 个并发客户端各执行 100 次 PING，
共 800 次请求无失败。测试结束后 can0 的发送错误、接收错误、丢包、仲裁丢失
和 bus-off 计数均为 0，普通 PING 仍正常。

如果需要绕过 SocketCAN 检查 CANable2 原厂固件、发送测试帧并读取其软件错误
寄存器，应先停止 `toolbusd` 和 `slcand`，再执行：

```sh
sudo bash tests/hardware/canable2_raw_probe.sh \
  /dev/serial/by-id/usb-Openlight_Labs_CANable2_* 32
```
