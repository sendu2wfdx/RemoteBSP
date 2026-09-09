# RemoteBSP Runtime API v1

## 目标与边界

Runtime API 是位于 `toolbusd` 和浏览器/上层应用之间的长期运行服务边界，作用类似
Moonraker，但面向通用 RemoteBSP 节点和资源。本轮只实现可替换 Provider 之上的
只读 HTTP 骨架，用于稳定数据模型和前端集成，不访问 SocketCAN、USB 或实体板。

当前明确不提供：

- GPIO、UART、运动等写命令；
- 固件生成、构建和烧录；
- 用户认证、授权与多租户；
- WebSocket、SSE 或遥测历史库；
- 设备协议、运动学和具体设备业务。

服务默认只允许绑定 `127.0.0.1`、`localhost` 或 `::1`。认证实现前不能直接监听
局域网地址；远程访问也不应通过修改此限制临时开放。

## 分层

```text
Web UI / 上层应用
        │ HTTP /api/v1，只读
        ▼
Runtime HTTP 层
        │ 只依赖 RuntimeProvider
        ▼
文件 Provider / Mock Provider
        │ 未来增加 toolbusd Provider
        ▼
libremotebsp / toolbusd / 传输层（本轮未接入）
```

HTTP 层不能导入 SocketCAN、USB 或板卡实现。未来接入 `toolbusd` 时应新增 Provider，
把节点枚举、资源目录和遥测转换成同一快照，而不是在请求处理器里发送总线命令。

## 启动

使用内置 Mock：

```sh
cd /mnt/d/Documents/RemoteBSP
python3 -m runtime_api.server
```

读取由其他进程原子替换的快照文件：

```sh
python3 -m runtime_api.server --snapshot /tmp/remotebsp-runtime-v1.json
```

默认地址为 `http://127.0.0.1:8780/api/v1`。文件 Provider 每次请求重新读取文件，
输入上限为 1 MiB；数据损坏、文件缺失或 schema 错误返回 HTTP 503，不回退到陈旧
快照或演示数据，以免界面把旧状态误报为在线。

## HTTP 契约

所有成功响应使用统一信封：

```json
{"api_version":"v1","ok":true,"data":{}}
```

失败响应使用：

```json
{
  "api_version": "v1",
  "ok": false,
  "error": {"code": "node_not_found", "message": "节点不存在：node-1"}
}
```

已实现端点：

| 方法与路径 | 内容 |
|---|---|
| `GET /api/v1` | schema 版本、端点和未实现能力声明 |
| `GET /api/v1/health` | Provider 可用性和当前 `snapshot_id` |
| `GET /api/v1/snapshot` | 完整、同一时刻的节点/资源/告警快照 |
| `GET /api/v1/nodes` | 节点摘要、资源数和活动告警数 |
| `GET /api/v1/nodes/{node_id}` | 单节点详情与运行态 |
| `GET /api/v1/nodes/{node_id}/resources` | 单节点资源 |
| `GET /api/v1/nodes/{node_id}/alerts` | 单节点告警 |
| `GET /api/v1/resources` | 全部资源，并补充所属 `node_id` |
| `GET /api/v1/alerts` | 全部告警 |

`HEAD` 与对应 `GET` 返回相同状态和头部但没有响应体。任何写方法返回 HTTP 405；
未知版本或路径返回 404；本轮不接受查询参数，避免形成未定义的过滤/分页语义。

## 快照 JSON v1

机器可读契约位于
[`runtime-snapshot-v1.schema.json`](../runtime_api/runtime-snapshot-v1.schema.json)，
运行时还执行 JSON Schema 难以表达的唯一性和引用完整性检查。

最小结构如下：

```json
{
  "schema_version": 1,
  "snapshot_id": "snapshot-42",
  "captured_at_ms": 42000,
  "nodes": [{
    "node_id": "toolboard-1",
    "board_type": "weact-g431-core-v10",
    "display_name": "工具头控制板",
    "state": "online",
    "last_seen_ms": 41990,
    "links": [{"kind": "can_fd", "state": "online"}],
    "resources": [{
      "resource_id": "motion-axis-0",
      "kind": "motion_axis",
      "name": "X轴",
      "available": true,
      "state": {"enabled": true, "position_steps": 1200}
    }],
    "runtime": {"uptime_ms": 41000, "queue_depth": 8}
  }],
  "alerts": [{
    "alert_id": "alert-9",
    "node_id": "toolboard-1",
    "resource_id": "motion-axis-0",
    "severity": "warning",
    "code": "queue_low",
    "message": "运动队列水位偏低",
    "active": true,
    "occurred_at_ms": 41980
  }]
}
```

约束包括：

- `snapshot_id`、`node_id`、`resource_id` 和 `alert_id` 是稳定标识符；显示名称不能
  被用作程序身份；
- 节点 ID、节点内资源 ID、节点内链路类型和告警 ID 各自唯一；
- 告警引用的节点和可选资源必须存在于同一快照；
- `state` 和 `runtime` 是资源类型扩展区，但必须仍是有限、合法的 JSON 对象；
- 数组在规范化后按标识符排序，调用者不能依赖 Provider 的原始顺序；
- `captured_at_ms`、`last_seen_ms` 和 `occurred_at_ms` 使用同一上位机单调时基。它们
  不是 UTC 墙上时间，跨进程展示时应同时携带未来的时基元数据。

## 写命令与事件流的预留原则

后续写 API 不应直接复用只读快照端点。建议采用 `/api/v1/commands` 或作业资源，
请求至少包含请求 ID、操作者、目标节点/资源、截止时间和幂等键；返回“已接受”不
等于 MCU 已执行。紧急停止使用专门的高优先级命令类型，不能被普通队列阻塞。

事件流应在完成身份认证和背压设计后增加。WebSocket/SSE 消息至少携带快照修订号，
客户端发现修订断档时重新读取 `/snapshot`，不能凭增量事件猜测当前设备状态。

## 构建接入

本轮没有修改顶层 CMake 或安装规则。后续集成负责人需要决定：

1. Python 包的安装位置和 systemd 服务用户；
2. `runtime-snapshot-v1.schema.json` 的安装路径；
3. Runtime 与 `toolbusd` Unix Domain Socket 的权限组；
4. Web UI 静态文件由 Runtime、反向代理还是独立服务托管。

在这些部署决策完成前，Runtime API 只作为仓库内可启动、可测试的开发服务。
