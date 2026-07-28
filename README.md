# Remote BSP

这是一个与传输方式无关的远程 BSP 框架，用于从 Linux 主机管理远端 MCU
提供的通用硬件能力。目前已打通 `libremotebsp -> toolbusd -> SocketCAN ->
Mock MCU` 完整路径，支持多节点寻址、节点/资源健康监控、GPIO 和 UART。

## 构建与测试

需要 CMake 3.16 或更高版本、Ninja 和支持 C++17 的编译器。

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

如需运行真实 SocketCAN 回环测试，请先准备已启用的 `vcan` 接口，然后配置：

```sh
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON \
    -DREMOTEBSP_VCAN_INTERFACE=vcan0
cmake --build build
ctest --test-dir build --output-on-failure
```

协议格式和分层原则见 [docs/architecture.md](docs/architecture.md)。

STM32F103CBT6 与 STM32G431CBU6 的接线、固件编译和 SWD 烧录见：

- [STM32 硬件与接线](docs/stm32-hardware-plan.md)
- [STM32 固件编译与烧录](docs/stm32-build-and-flash.md)

## 运行 Mock MCU

在支持 SocketCAN 和 `vcan` 的 Linux 环境中：

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set dev vcan0 up

./build/mock_mcu/mock_mcu vcan0 classical
```

CAN-FD 模式使用：

```sh
./build/mock_mcu/mock_mcu vcan0 fd
```

广播发现和节点分配使用 CAN ID `0x700`。节点分配后，节点 N 使用
`0x600 + N` 接收业务请求、`0x580 + N` 返回响应、`0x500 + N` 发送心跳。
节点 ID 范围为 1～127。

Mock MCU 当前支持 `GET_INFO`、`GET_CAPABILITY`、`PING`、节点发现、节点
分配、心跳、GPIO 和 UART 原子操作。

## 运行完整链路

先启动 Mock MCU：

```sh
./build/mock_mcu/mock_mcu vcan0 classical
```

再启动守护进程：

```sh
./build/toolbusd/toolbusd vcan0 classical /tmp/toolbusd.sock
```

应用程序只能通过本地 Unix Domain Socket 访问 `toolbusd`。示例：

```sh
./build/remote-cli node-list
./build/remote-cli --node 1 ping hello
./build/remote-cli --node 1 get-info
./build/remote-cli --node 1 get-capability
./build/remote-cli --node 1 resource-list
./build/remote-cli --node 1 resource-describe 0x02000007
./build/remote-cli --node 1 resource-status 0x02000007
./build/remote-cli --node 1 resource-reset 0x02000007
./build/remote-cli --node 1 event-wait

./build/remote-cli --node 1 gpio-create 13 output 0
./build/remote-cli --node 1 gpio-write 1 1
./build/remote-cli --node 1 gpio-read 1

./build/remote-cli --node 1 uart-create 0 115200 8 none 1
./build/remote-cli --node 1 uart-write 2 hello
./build/remote-cli --node 1 uart-read 2 64
```

CAN-FD 模式下，将两个进程命令中的 `classical` 都改为 `fd`。

可以同时启动多个 Mock 工具板：

```sh
./build/mock_mcu/mock_mcu vcan0 fd --instance 1
./build/mock_mcu/mock_mcu vcan0 fd --instance 2
```

为验证持续 UART RX 事件，可给 Mock MCU 增加 `--uart-stream`。它会从逻辑
UART 7 周期产生原始字节事件；数据只在 Linux 侧解释，不在 Mock MCU 中解析。

Mock 工具板的资源目录包含 16 路 GPIO 和 8 路 UART。UART 0～3 标记为原生，
UART 4～7 标记为扩展，二者对 Linux 使用完全相同的 UART 命令。扩展串口占用
的内部 SPI 不作为可分配资源暴露。每路 UART 当前声明 4096 字节收发缓冲容量。

C++ 应用应直接链接 `libremotebsp`，接口与示例见
[libremotebsp 客户端 API](docs/libremotebsp-api.md)。
