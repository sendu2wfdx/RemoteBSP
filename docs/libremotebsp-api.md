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

    const auto traffic = client.traffic_status();
    std::cout << "已准入包数 " << traffic.admitted_packets
              << "，拒绝包数 " << traffic.rejected_packets << '\n';

    const auto resources = client.list_resources();
    const auto contract = client.resource_contract(0x0100000d);
    const auto lease = client.acquire_resource(
        0x0100000d, 5000,
        remotebsp::protocol::ResourceLeaseMode::Exclusive);
    const auto gpio = client.gpio_create(
        13, remotebsp::GpioDirection::Output, false);
    client.gpio_write(gpio, true);

    remotebsp::UartConfig config;
    config.port = 0;
    config.baud_rate = 115200;
    config.receive_mode = remotebsp::UartReceiveMode::Streaming;
    const auto uart = client.uart_create(config);
    client.uart_write_all(uart, {'h', 'e', 'l', 'l', 'o'});
    if (const auto chunk = client.uart_stream_read(uart, 1024, 1000)) {
        // chunk->data 是原始字节；两个计数用于监控本地溢出和 CAN 事件断档。
        std::cout << "收到 " << chunk->data.size()
                  << " 字节，丢弃 " << chunk->dropped_bytes
                  << " 字节，缺失 " << chunk->lost_events
                  << " 个事件\n";
    }
    const auto status = client.resource_status(0x02000000);
    if (status.health == remotebsp::protocol::ResourceHealth::Failed) {
        client.reset_resource(status.resource_id);
    }

    client.renew_resource(0x0100000d, lease.lease_id, 5000);
    client.release_resource(0x0100000d, lease.lease_id);
}
```

## 资源能力合同与租约

能力合同通过`resource_contract()`读取，包含访问标志、定时分辨率、最坏延迟、
操作频率、队列容量和收发速率。数值为0表示节点没有声明该指标。

租约API包括：

- `acquire_resource()`：申请共享读或独占租约并返回64位租约ID。
- `renew_resource()`：使用资源ID和租约ID续租。
- `resource_lease_status()`：查询当前模式、剩余时间和活动租约数量。
- `release_resource()`：显式释放；到期或会话清理也会自动释放。

当前Mock MCU实现了远端会话级租约。由于`Client`的每次调用使用短UDS连接，而
`toolbusd`当前为所有本地客户端复用同一个远端会话，本地应用之间仍不能把租约
ID当作安全凭证。现阶段租约用于协议、冲突、超时和安全状态验证；持久IPC身份
完成后再提供不可绕过的本地应用所有权。

## 设备参数

设备身份、制造信息和校准值使用独立持久参数接口：

- `device_parameter_status()`：读取存储代数、锁定和初始化状态；
- `list_device_parameters()`：枚举参数ID、类型、标志和长度范围；
- `read_device_parameter(id)`：读取参数值及其代数；
- `write_device_parameter(id, value)`：完成临时解锁、写入、重新锁定并返回新状态。

当前schema包含SN、UUID、硬件版本、制造批次/日期、设备名称和16路ADC校准值。
写入属于有副作用请求，重试沿用请求ID，MCU不会重复执行。该接口不修改GPIO、UART、
运动或波形资源映射。

## CAN流量状态

`traffic_status()`查询`toolbusd`本地发送方向的准入状态，不访问远端节点。返回：

- Classical CAN或CAN-FD模式。
- 仲裁段和数据段位速率。
- 最大利用率与突发窗口。
- 当前全局可用预算。
- 累计准入、拒绝、保证发送、CAN帧数和估算线时间。
- 安全、运动、系统、交互、持续流和大块传输六类独立计数。

这些数值是依据配置位速率和保守帧模型得到的软件估算，不是控制器采样的真实
总线占用。接口实际位速率与`toolbusd`启动参数不一致时，估算结果无效。预算
不足的新请求会抛出`ClientException`，错误文本包含“CAN 带宽准入拒绝”；
该请求尚未发送到MCU，不会产生远端副作用。

## 智能步进运动

`Client`当前提供：

- `motion_contract(refresh)`：读取节点级运动合同；默认复用线程安全缓存，传入
  `true`可强制刷新。合同包含活动轴、每轴STEP时序、整板总步频、队列容量和
  最小提前量。
- `motion_enqueue(segment)`：提交一个包含所有轴的运动段；开始时间为0时由节点
  按最小提前量自动接续排程，并在返回值中给出实际开始时间。发送前会自动按
  当前合同校验轴集合、单轴步频/脉宽和整板总步频，拒绝信息以
  “主机运动能力准入拒绝”开头且不会产生远端副作用。
- `motion_status()`：读取节点时间、状态/故障、队列、段序号、每轴位置和运动计数。
- `motion_abort()`：走安全业务通道，立即清空队列并锁存主动停止故障。
- `motion_clear_fault()`：在队列已经清空后解除故障锁存。

运动轴合同要求独占租约。应用应先为运动组全部轴调用`acquire_resource()`，
保持租约覆盖整个前瞻队列；租约到期或会话释放时，Mock执行器会安全停止。运动段
序号从1开始严格递增，非最终段必须无缝接续，最后一段使用`final_segment=true`。
当前API已经在Classical CAN和CAN-FD的Mock端到端路径验证，尚未接入STM32固件。

## 并发和错误

同一个 `Client` 实例可以被多个线程并发调用。每次调用建立独立 Unix Domain
Socket 连接，由 `toolbusd` 完成超时、重试和响应匹配。

- 本地连接、IPC 格式或超时错误抛出 `ClientException`。
- MCU 返回非成功状态时抛出 `RemoteException`，可通过 `status()` 获取远端
  状态码。
- 节点离线时，`toolbusd` 只拒绝该节点的新请求，不影响其他节点。

UART 接收有两种互斥模式：

- 默认 `Polling` 模式使用 `uart_read()` 非阻塞轮询，兼容原来的8字节
  `UART_CREATE` 载荷，适合Modbus等主机发起的短事务。
- `Streaming` 模式由MCU将RX环形缓冲按批次主动上报，应用使用
  `uart_stream_read()` 从toolbusd按“节点+UART对象”隔离的64 KiB缓冲读取。

通用高速流使用 `stream_contract()`、`stream_open()`、`stream_write()`、
`stream_status()`、`stream_credit()` 和 `stream_stop()`。远端 Mock Core 已实现
`HostToNode` 有界接收和 `NodeToHost` 事件/信用竖切；N2H 主机侧暂时通过通用
`next_event()` 取得 `Command::StreamData` 事件、使用 `decode_stream_data()` 校验会话
代次与连续序号，并在消费后用 `stream_credit()` 精确确认累计字节。打开 H2N 可写流前
必须取得同一资源的独占租约；打开 N2H 流前必须取得合同允许的共享读或独占租约。停止、
租约失效或会话释放会清空未消费 Mock 字节、信用和未确认块。

这仍是 Mock BSP、Remote Core 和 MockNode 分片的纯软件能力。真实 N2H CAN/USB Bulk
发送路径、toolbusd 专用流缓冲和便利读取 API 尚未完成；双向流与 USB Bulk 实际路由仍
明确失败，不能依赖自动回退到 CAN。
  流式对象禁止再调用`uart_read()`，避免两个消费者争抢字节。

每个流事件使用请求ID携带单调序号。toolbusd过滤重复/旧事件，统计
`lost_events`；本地缓冲满时丢弃最旧字节并累计`dropped_bytes`。普通
`next_event()`仍能观察事件，但不会消费UART专用字节缓冲。`uart_write_all()`
把大数据拆成64字节块，在远端TX缓冲暂满时有限重试；单次原子写仍使用
`uart_write()`。
