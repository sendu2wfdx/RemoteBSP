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
文件 Provider / Mock Provider / Toolbusd Provider
        │ Toolbusd Provider 只调用既有只读客户端
        ▼
remote-cli / libremotebsp / toolbusd 本地套接字
        │ 未来可替换为原生绑定或结构化 IPC
        ▼
toolbusd 管理的传输层
```

HTTP 层不能导入 SocketCAN、USB 或板卡实现。当前 Toolbusd Provider 通过现有
`remote-cli → libremotebsp → toolbusd Unix Domain Socket` 边界读取信息，把节点枚举、
资源目录和状态转换成同一快照；请求处理器本身不会发送 CAN/USB 帧。

Provider 默认给四个命令添加 `--json`，只接受版本化结构化输出。`remote-cli` 未带
`--json` 时继续输出原有的人类可读文本，不改变已有终端用法。

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

通过既有本地 IPC 连接已经运行的 `toolbusd`：

```sh
python3 -m runtime_api.server \
  --toolbusd-socket /tmp/toolbusd.sock \
  --remote-cli ./build-wsl/remote-cli \
  --ipc-timeout-ms 2000
```

默认地址为 `http://127.0.0.1:8780/api/v1`。文件 Provider 每次请求重新读取文件，
输入上限为 1 MiB；数据损坏、文件缺失或 schema 错误返回 HTTP 503，不回退到陈旧
快照或演示数据，以免界面把旧状态误报为在线。

Toolbusd Provider 只执行 `traffic-status`、`node-list`、`resource-list` 和
`resource-status` 四种只读命令；命令使用参数数组启动，不经过 shell。全局 IPC
连接失败、输出格式不兼容或基础节点目录无效时，HTTP API 返回 503。单个节点的
资源目录暂时不可用时，其他节点仍保留，该节点标记为 `degraded` 并产生告警；单个
资源状态读取失败时，该资源保留但标记 `available=false`、`health=unknown`。单次
快照默认最多查询 128 项资源，超过上限显式失败，避免异常目录造成无界请求放大。

旧版 `remote-cli` 暂时只能输出文本时，必须显式使用
`--toolbusd-legacy-text`。结构化输出解析失败不会静默回退到文本；否则升级不兼容可能
被误判为合法设备状态。该开关仅用于过渡，不能与 `--toolbusd-socket` 分开使用。

## remote-cli 结构化只读契约

以下四种调用支持统一的全局 `--json` 开关：

```sh
remote-cli --json --socket /tmp/toolbusd.sock traffic-status
remote-cli --json --socket /tmp/toolbusd.sock node-list
remote-cli --json --socket /tmp/toolbusd.sock --node 1 resource-list
remote-cli --json --socket /tmp/toolbusd.sock --node 1 resource-status 16777217
```

每次成功调用只在标准输出写入一个 JSON 文档，根信封固定为：

```json
{
  "schema_version": 1,
  "command": "node-list",
  "data": {"nodes": []}
}
```

四种 `data` 形状分别为：

- `traffic-status`：`traffic` 对象，包含链路模式、速率、准入汇总和固定顺序的六类
  业务计数器；
- `node-list`：`nodes` 数组，固件版本使用 `{major, minor, patch}` 对象，UUID 使用
  32 位十六进制字符串；
- `resource-list`：目标 `node_id` 和 `resources` 数组；
- `resource-status`：目标 `node_id` 和单个 `resource` 状态对象。

Runtime Provider 对 v1 使用封闭字段集合，严格检查根信封、命令名、字段类型、数值
范围、固定枚举、流量类别顺序，以及响应中的节点/资源 ID 是否与请求相符。未知版本、
缺失或多余字段、非 UTF-8、超过 1 MiB、超时或非零退出均显式失败。版本升级应新增
解析器并经过明确协商，不能让 v1 解析器猜测新字段语义。

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

现有 toolbusd 节点列表不提供离线节点最后一次真实可见的时间。Toolbusd Provider
只会给本次 IPC 明确观察到在线的节点写入 `last_seen_ms=captured_at_ms`；离线节点固定
使用 `last_seen_ms=0` 和 `runtime.last_seen_known=false`，表示未知，前端不得显示为
“刚刚在线”。Provider 还会在任何资源查询前拒绝重复的 numeric node ID 或 UUID，
防止同一路由被重复展开并产生互相矛盾的状态。

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

当前适配器为保持边界清晰而复用 `remote-cli` 的版本化 JSON 输出，每次完整快照需要一次全局
状态、一次节点列表、每个就绪节点一次资源列表以及每项资源一次状态查询。正式长期
运行前可进一步给 libremotebsp 增加原生语言绑定或单次快照 IPC，并在 Provider 层做
有界并发与明确新鲜度的快照缓存；不能让 Web 请求数量直接放大为无界 IPC 请求。

在这些部署决策完成前，Runtime API 只作为仓库内可启动、可测试的开发服务。
