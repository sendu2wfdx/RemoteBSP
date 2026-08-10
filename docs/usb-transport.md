# RemoteBSP USB Vendor Bulk 传输

## 目标与边界

USB 是 RemoteBSP 的第二种正式业务传输候选，不是调试串口，也不改变应用 API。
应用仍然通过 `libremotebsp -> Unix Domain Socket -> toolbusd` 访问远端资源；只有
toolbusd 到工具板之间的链路从 SocketCAN 换成 USB Vendor Bulk。

当前阶段已经完成：

- 通用 `LinkTransport`、`LinkFrame`、链路能力和逻辑路由号。
- Classical CAN/CAN-FD 通过 `SocketCanTransport` 接入新接口，原有线格式不变。
- Linux `LibusbTransport`：VID/PID/序列号选择、接口独占、Bulk IN/OUT、超时和断线错误。
- USB `RBU1` 帧编解码、拆包、粘包、长度和版本校验。
- Unix 字节流模拟的 `MockUsbTransport`，覆盖真实 USB 不保留应用 write 边界的情况。
- 发现、心跳、请求重试、GPIO、UART、运动、PWM、WS2812 和并发客户端 USB Mock 端到端测试。
- MCU 公共 `RBSP_USB` 模式、USB 帧编解码器和 `menuconfig` 主链路选项。
- STM32G431 USB Device Vendor Bulk 类、端点/PMA、HSI48+CRS、收发环形缓冲及 APP 交叉编译。

尚未完成：

- 实体 G431 枚举、掉线重连、吞吐和运动并发验证。
- F072/F103 的 USB Device 时钟、中断和板型后端。
- 多块同 VID/PID 设备的热插拔监视及 toolbusd 动态设备管理。
- CAN 与 USB 同时启用的多链路路由和会话迁移。

因此当前 Linux 主机、Mock 全链路和 G431 USB APP 均已实现；G431 产物已经交叉
编译，但在完成实体枚举、重连和压力测试前仍属于“待实板验收”。F072/F103 的
菜单不会显示 USB APP 选项，避免静默生成错误链路的固件。

## 线格式

USB Bulk 是有序可靠的字节流，但一次 `libusb_bulk_transfer` 不等于一条上层消息。
每个逻辑链路帧使用 12 字节小端序帧头：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | ASCII `RBU1` 魔数 |
| 4 | 1 | USB 帧版本，当前为 1 |
| 5 | 1 | 标志，当前必须为 0 |
| 6 | 2 | 载荷长度 |
| 8 | 4 | 逻辑路由号 |
| 12 | N | RemoteBSP 分片 |

第一版逻辑 MTU 为 64 字节。原因是现有 RemoteBSP 分片头用 6 位表示真实载荷长度，
单片最多容纳 59 字节协议数据；保持 64 字节可以让 CAN-FD、USB 和 MCU 共用已经
验证的分片器。后续可在 USB 适配器中把多个逻辑帧合并到一次 Bulk 事务，而不修改
Remote Packet 和分片线格式。

逻辑路由号沿用现有数值，但不再解释为通用层的 CAN ID：

- `0x700`：发现和节点分配。
- `0x600 + N`：发往节点 N 的请求。
- `0x580 + N`：节点 N 的响应。
- `0x500 + N`：节点 N 的事件和心跳。
- `0x480 + 临时号`：未分配节点的响应。

CAN 适配器把路由号映射为标准 CAN ID；USB 帧头直接携带路由号。

## Linux 构建依赖

```sh
sudo apt install -y pkg-config libusb-1.0-0-dev
```

真实设备选择器格式：

```text
VID:PID
VID:PID@SERIAL
```

例如（文档命令使用开发占位 VID/PID）：

```sh
./build-wsl/toolbusd/toolbusd \
    0x1209:0x0001@BOARD-01 usb /tmp/toolbusd.sock
```

VID/PID 必须由最终产品合法分配。Kconfig 中的开发默认值不能直接用于发布产品。
当同一 VID/PID 下存在多块板卡时必须指定 USB 序列号，否则 toolbusd 拒绝任意选取。

## 无硬件端到端验证

终端一：

```sh
./build-wsl/toolbusd/toolbusd \
    /tmp/remotebsp-usb-link.sock usb-mock /tmp/toolbusd.sock
```

终端二：

```sh
./build-wsl/mock_mcu/mock_mcu \
    /tmp/remotebsp-usb-link.sock usb-mock --uart-stream
```

终端三继续使用现有 CLI：

```sh
./build-wsl/remote-cli node-list
./build-wsl/remote-cli ping usb-test
./build-wsl/remote-cli traffic-status
```

自动测试名称为 `usb_frame_tests`、`embedded_usb_link_tests` 和
`e2e_usb_mock_tests`。

## G431 固件构建

```sh
cd /mnt/d/Documents/RemoteBSP/firmware
bash scripts/build_firmware.sh weact-stm32g431cbu6-core-usb
```

产物为 `out/remotebsp-stm32g431-weact-core-usb.{elf,hex,bin}`。它使用 PA11/PA12
和独立的 USB APP PID，不初始化 FDCAN；CAN-FD 版本继续使用原有板卡预设构建。
当前两种 APP 是编译期二选一，不支持运行时同时承载两条主链路。

G431 预设为 RX/TX 各保留 3072 字节原始环形缓冲，可完整容纳最大 2048 字节
Remote Packet 的全部逻辑分片及 RBU1 帧头。该选择优先保证长包不会半途截断，
也意味着 USB APP 的 SRAM 预算高于 CAN-FD APP；后续启用运动等大模块时必须以
链接器内存报告为准，不能盲目扩大专家级缓冲参数。

## 与 Katapult 的关系

USB RemoteBSP APP 和 USB Katapult 是两个独立运行阶段，必须使用不同 PID：

1. APP 运行 Vendor Bulk RemoteBSP。
2. 收到带确认串的 Bootloader 命令后延迟复位。
3. Katapult 以自己的 USB PID 重新枚举并完成升级。
4. 新 APP 启动后重新枚举为 RemoteBSP PID。

正式实现必须保证 USB 接收中断只搬运字节和重新挂接 OUT 端点；Remote Packet 解码、
资源操作和请求去重仍在主循环执行。USB 掉线不能停止本地运动定时器，队列不足时
仍按现有安全策略停机。
