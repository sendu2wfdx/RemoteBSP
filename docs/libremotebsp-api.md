# libremotebsp 客户端 API

应用程序通过 `libremotebsp` 访问 `toolbusd`，不直接打开 SocketCAN，也不需要
自行编码远程协议、分片或管理请求 ID。

## 基本用法

```cpp
#include <remotebsp/client.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    // 第二个参数是运行时节点 ID，可先通过 list_nodes() 按 UUID 选择。
    remotebsp::Client client("/tmp/toolbusd.sock", 1);

    for (const auto& node : client.list_nodes()) {
        std::cout << "节点 " << node.node_id
                  << (node.ready ? " 可用\n" :
                      (node.online ? " 地址分配中\n" : " 离线\n"));
    }

    const auto resources = client.list_resources();
    const auto gpio = client.gpio_create(
        13, remotebsp::GpioDirection::Output, false);
    client.gpio_write(gpio, true);

    remotebsp::UartConfig config;
    config.port = 0;
    config.baud_rate = 115200;
    const auto uart = client.uart_create(config);
    client.uart_write(uart, {'h', 'e', 'l', 'l', 'o'});

    const auto data = client.uart_read(uart, 231);
    if (const auto event = client.next_event()) {
        // event->header.object_id 是资源 ID，event->payload 是原始 UART 字节。
    }
    const auto status = client.resource_status(0x02000000);
    if (status.health == remotebsp::protocol::ResourceHealth::Failed) {
        client.reset_resource(status.resource_id);
    }
}
```

## 并发和错误

同一个 `Client` 实例可以被多个线程并发调用。每次调用建立独立 Unix Domain
Socket 连接，由 `toolbusd` 完成超时、重试和响应匹配。

- 本地连接、IPC 格式或超时错误抛出 `ClientException`。
- MCU 返回非成功状态时抛出 `RemoteException`，可通过 `status()` 获取远端
  状态码。
- 节点离线时，`toolbusd` 只拒绝该节点的新请求，不影响其他节点。

UART 接收同时支持非阻塞轮询和 `next_event()` 异步 RX 事件等待。GPS 等持续
数据可以走事件通路；每资源带宽配额、更细的事件优先级和溢出丢弃计数公开仍是
后续增强项。
