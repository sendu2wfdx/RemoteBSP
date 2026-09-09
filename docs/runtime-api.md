# RemoteBSP Runtime API v1

## 目标与边界

Runtime API 是位于 `toolbusd` 和浏览器/上层应用之间的长期运行服务边界，作用类似
Moonraker，但面向通用 RemoteBSP 节点和资源。本轮只实现可替换 Provider 之上的
只读 HTTP 骨架，用于稳定数据模型和前端集成，不访问 SocketCAN、USB 或实体板。

当前明确不提供：

- GPIO、UART、运动等写命令；
- 固件生成、构建和烧录；
- 用户/角色授权、多租户与会话管理；
- WebSocket、SSE 或遥测历史库；
- 设备协议、运动学和具体设备业务。

服务默认绑定 `127.0.0.1`，此时可使用明确标记的无认证回环开发模式。`--host` 只接受
数字 IP 地址；即使是 `localhost` 也会被拒绝，避免名称解析误配把无认证服务绑定到
非回环接口。监听任何
非回环数字 IP 地址时必须在启动阶段成功加载 API 密钥文件，否则进程显式失败；不能
通过库调用绕过该边界。API 密钥只提供请求认证，不提供链路加密。跨主机部署必须在
受控网络中由 TLS 反向代理终止 HTTPS，不能把明文 HTTP 直接暴露到不可信网络。

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
        │ RuntimeSnapshot v2 聚合 IPC；旧命令继续兼容
        ▼
toolbusd 管理的传输层
```

HTTP 层不能导入 SocketCAN、USB 或板卡实现。当前 Toolbusd Provider 通过现有
`remote-cli → libremotebsp → toolbusd Unix Domain Socket` 边界读取信息，把节点枚举、
资源目录和状态转换成同一快照；请求处理器本身不会发送 CAN/USB 帧。

Provider 默认使用一次 `runtime-snapshot --json`，只接受版本化结构化输出。`remote-cli` 未带
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
  --ipc-timeout-ms 2000 \
  --snapshot-cache-ms 250 \
  --status-query-workers 8 \
  --clock-error-warning-ns 250000 \
  --clock-sample-age-warning-ms 1000
```

默认地址为 `http://127.0.0.1:8780/api/v1`。文件 Provider 每次请求重新读取文件，
输入上限为 1 MiB；数据损坏、文件缺失或 schema 错误返回 HTTP 503，不回退到陈旧
快照或演示数据，以免界面把旧状态误报为在线。

Toolbusd Provider 默认只执行一次 `runtime-snapshot` 只读命令；命令使用参数数组启动，
不经过 shell。显式旧文本兼容模式继续执行 `traffic-status`、`node-list`、
`resource-list` 和 `resource-status`。全局 IPC
连接失败、输出格式不兼容或基础节点目录无效时，HTTP API 返回 503。单个节点的
资源目录暂时不可用时，其他节点仍保留，该节点标记为 `degraded` 并产生告警；单个
资源状态读取失败时，该资源保留但标记 `available=false`、`health=unknown`。单次
快照默认最多查询 128 项资源，超过上限显式失败，避免异常目录造成无界请求放大。

Toolbusd Provider 默认使用 250 ms 的线程安全短时缓存。同一时刻最多有一个完整快照
刷新；其他 HTTP 请求共享该次结果，不会各自重复调用 `remote-cli`。成功和失败都会在
这段短窗口内合并：刷新失败返回 HTTP 503，并在 250 ms 内复用相同失败，不回退到上次
成功快照。等待正在进行的刷新默认最多 5000 ms，超时也返回 503。可通过
`--snapshot-cache-ms` 和 `--snapshot-refresh-wait-ms` 调整；缓存设为 0 表示禁用。
对于带有效时钟估计且尚未触发样本陈旧告警的 v2 快照，实际缓存命中窗口还会缩短到
“距离样本年龄阈值的剩余时间”；到达边界后强制刷新，不能让一份原本无告警的快照在
缓存中跨过 `clock_sync_sample_stale` 阈值。HTTP 元数据中的 `cache_ttl_ms` 仍表示配置的
最大缓存周期，而不是对每个节点动态计算后的剩余时间。

旧文本兼容路径单次刷新内的 `resource-status` 默认最多 8 路并发，并继续受资源总查询
上限约束；
分别由 `--status-query-workers`（1～32）和 `--maximum-resource-queries` 配置；
结构化快照路径上限为 128，显式旧文本模式保留 1～4096 的过渡范围。HTTP 服务默认
最多保留 32 个活动请求线程，满载后在监听队列施加背压，可通过
`--http-workers`（1～256）调整。这些限制只控制 Runtime 进程内聚合，不绕过
`remote-cli → libremotebsp → toolbusd` 边界。

Runtime 对主机时钟模型使用两个只读告警阈值：估计误差上界默认 250000 ns，最后入选
样本年龄默认 1000 ms；分别由 `--clock-error-warning-ns`（1～1000000000）和
`--clock-sample-age-warning-ms`（1～60000）配置。它们只决定 Runtime 告警，不改写
toolbusd 模型、不替代运动准入门限，也不会向节点发送命令。`GET /api/v1` 与
`GET /api/v1/health` 的 `capabilities.clock_sync_quality` 明确给出数据源、
`estimate_kind=host_model_estimate` 和生效阈值；数据源没有 v2 快照能力时报告
`available=false,estimate_kind=unavailable`。

旧版 `remote-cli` 暂时只能输出文本时，必须显式使用
`--toolbusd-legacy-text`。结构化输出解析失败不会静默回退到文本；否则升级不兼容可能
被误判为合法设备状态。该开关仅用于过渡，不能与 `--toolbusd-socket` 分开使用。

## HTTP 认证边界

回环开发模式无需密钥，能力声明为
`authentication=false,authentication_mode="disabled_loopback"`。即使在回环地址，也可
显式提供 `--api-key-file` 来启用与部署环境相同的认证路径。非回环监听必须提供该文件：

```json
{
  "schema_version": 1,
  "api_keys": ["REPLACE_WITH_RANDOM_ASCII_KEY_AT_LEAST_32_BYTES"]
}
```

```sh
chmod 600 /etc/remotebsp/runtime-auth.json
python3 -m runtime_api.server \
  --host 192.0.2.10 \
  --api-key-file /etc/remotebsp/runtime-auth.json
```

认证配置最多 16 KiB、16 个密钥；每个密钥为 32～128 字节的非空白可打印 ASCII，
重复密钥、未知字段、未知版本、空数组和格式错误都会使启动失败。密钥只从文件读取，
不提供容易进入进程列表和终端历史的明文命令行参数。生产密钥应由密码学安全随机源生成，
并通过同时保留新旧两个密钥完成有限时间轮换；修改文件后需重启服务加载。

当前加载器验证文件内容和大小，但不把跨平台文件所有者、POSIX mode 或符号链接来源
当作已验证的信任证据。部署者必须让服务管理器传入受控的常规文件路径，限制目录和文件
只对服务账号可读写，并避免可由低权限用户替换的符号链接；上面的 `chmod 600` 是部署
要求，不是 Runtime 已自动执行或验证的权限修复。

客户端只能选择一种请求头：

```http
Authorization: Bearer <api-key>
```

或：

```http
X-API-Key: <api-key>
```

服务对全部已配置密钥使用常量时间比较原语且不在首个匹配处提前返回。缺少密钥和错误
密钥统一返回 HTTP 401、`authentication_required`；重复认证头、同时提供两种认证方式
或 Bearer 格式错误返回 HTTP 400、`invalid_auth_header`。任何名为 `api_key`、
`api-key`、`apikey`、`x-api-key`、`access_token` 或 `token` 的查询参数均以 HTTP 400、
`credential_in_query` 拒绝，避免密钥进入访问日志、浏览器历史和代理缓存。

认证启用时，`/api/v1` 下除下述边界外均受同一中间件保护，包括尚未实现的写方法；
写请求通过认证后仍返回只读错误。`GET/HEAD /api/v1/health` 在未携带认证头时只返回
`status=ok,scope=liveness`，不读取 Provider，也不暴露快照、节点或能力；携带有效密钥
时返回完整健康信息。携带错误密钥不会降级成公开探针，而是返回 401。
`OPTIONS /api/v1...` 无需密钥并只返回方法边界，不启用 CORS、也不允许凭据跨域。

## remote-cli 结构化只读契约

以下五种调用支持统一的全局 `--json` 开关：

```sh
remote-cli --json --socket /tmp/toolbusd.sock traffic-status
remote-cli --json --socket /tmp/toolbusd.sock node-list
remote-cli --json --socket /tmp/toolbusd.sock --node 1 resource-list
remote-cli --json --socket /tmp/toolbusd.sock --node 1 resource-status 16777217
remote-cli --json --socket /tmp/toolbusd.sock runtime-snapshot 128 1900
```

每次成功调用只在标准输出写入一个 JSON 文档，根信封固定为：

```json
{
  "schema_version": 1,
  "command": "node-list",
  "data": {"nodes": []}
}
```

五种 `data` 形状分别为：

- `traffic-status`：`traffic` 对象，包含链路模式、速率、准入汇总和固定顺序的六类
  业务计数器；
- `node-list`：`nodes` 数组，固件版本使用 `{major, minor, patch}` 对象，UUID 使用
  32 位十六进制字符串；
- `resource-list`：目标 `node_id` 和 `resources` 数组；
- `resource-status`：目标 `node_id` 和单个 `resource` 状态对象。
- `runtime-snapshot`：快照 IPC 版本和守护进程序号、流量、节点、带状态有效位的资源，
  封闭的节点级错误项，以及与节点一一对应的时钟同步质量项。

`RuntimeSnapshot IPC v2` 是 `IpcRequestKind::RuntimeSnapshot`。请求必须携带版本 2、
1～128 的最大资源数和 1～5000 ms 的总时间上限；响应仍受本地 IPC 64 KiB 硬上限。
守护进程一次只执行一个快照，防止昂贵刷新互相放大，但普通控制请求不获取这个互斥锁，
慢快照不会在主机侧阻塞控制路径。快照开始时固定节点身份/路由/在线状态，结束时再次
核对；期间拓扑变化则整份失败。流量计数在资源查询结束后锁存。

这里的“一致”是单次本地 IPC、同一守护进程序号和起止拓扑稳定，不表示所有 MCU 的
资源状态在同一物理时刻原子采样。toolbusd 内部仍使用现有 Remote Packet 分层逐项读取
资源，但省去了 Runtime 刷新中的 N+1 进程和本地套接字连接。单个资源状态查询失败时
保留描述符并置 `status_valid=false`；Provider 映射成 `available=false`、
`health=unknown` 和告警。节点资源目录不可用使用封闭错误码 1，映射成节点降级告警。
结构损坏、未知版本/错误码、重复或悬空 ID、资源超限和拓扑变化不会返回部分可信快照。

v2 在 v1 快照内容之后增加与节点一一对应的固定长度时钟质量记录，并把响应头原保留
字段定义为时钟记录数。因为这会改变 v1 的线格式，IPC 请求、响应和 CLI 的
`data.snapshot_version` 同步提升为 2；v1 客户端或服务端会显式报告版本不支持，不会
猜测或静默误读扩展字段。JSON 根信封的通用 `schema_version` 仍为 1。

Runtime Provider 对 v2 使用封闭字段集合，严格检查根信封、命令名、字段类型、数值
范围、固定枚举、流量类别顺序，以及响应中的节点/资源 ID 是否与请求相符。未知版本、
缺失或多余字段、非 UTF-8、超过 1 MiB、超时或非零退出均显式失败。版本升级应新增
解析器并经过明确协商，不能让旧版解析器猜测新字段语义。

## HTTP 契约

所有成功响应使用统一信封：

```json
{"api_version":"v1","ok":true,"data":{}}
```

凡读取快照的端点，成功信封还包含：

```json
{
  "meta": {
    "snapshot_freshness": {
      "cache_status": "refresh",
      "age_ms": 0,
      "cache_ttl_ms": 250
    }
  }
}
```

`cache_status` 为 `refresh`、`hit` 或 `disabled`。Toolbusd Provider 的 `age_ms`
使用 Runtime 进程内单调时钟计算，因此可以可靠表示本进程缓存年龄；文件和 Mock
Provider 不猜测跨进程时钟域，年龄与缓存周期均为 `null`。相同信息还通过
`X-RemoteBSP-Snapshot-Cache` 和 `X-RemoteBSP-Snapshot-Age-Ms` 响应头提供，未知年龄
写作 `unknown`。`Cache-Control` 仍为 `no-store`，这里的短时缓存是服务端 IPC 合并，
不是允许浏览器持有陈旧状态。

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
| `GET /api/v1/health` | 未认证时仅存活探针；认证后含 Provider 可用性和当前 `snapshot_id` |
| `GET /api/v1/snapshot` | 完整、同一时刻的节点/资源/告警快照 |
| `GET /api/v1/nodes` | 节点摘要、资源数和活动告警数 |
| `GET /api/v1/nodes/{node_id}` | 单节点详情与运行态 |
| `GET /api/v1/nodes/{node_id}/resources` | 单节点资源 |
| `GET /api/v1/nodes/{node_id}/alerts` | 单节点告警 |
| `GET /api/v1/resources` | 全部资源，并补充所属 `node_id` |
| `GET /api/v1/alerts` | 全部告警 |

`HEAD` 与对应 `GET` 返回相同状态和头部但没有响应体。认证通过后的任何写方法返回
HTTP 405；`OPTIONS` 返回 HTTP 204 和 `Allow: GET, HEAD, OPTIONS`；未知版本或路径
返回 404。本轮不接受查询参数，避免形成未定义的过滤/分页语义，认证信息尤其不得放入
查询字符串。

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

Toolbusd Provider 把 v2 时钟记录放入每个节点的 `runtime.clock_sync`。字段包括
`boot_epoch`、`model_generation`、`state`、总样本数、入选拟合样本数、频率偏差
`rate_deviation_ppb`、漂移不确定度、最小网络 RTT、当前误差上界、样本年龄和最后样本
主机单调时间。`source_available=false,state="unknown"` 表示显式旧文本路径没有该观测
能力；v2 中尚未注册模型使用 `registered=false,state="unregistered"`；已注册但尚无有效
拟合使用 `estimate_valid=false,state="unsynced"`，所有估计量为 `null`。`unsynced` 也可
表示一个曾有效但已经过期的估计，此时 `estimate_valid=true` 且保留可审计数值，但不能
据此准入跨板运动。这些数值来自主机模型及软件时间戳，只是估计和保守上界，不代表
硬件时间戳精度、MCU 晶振规格或实体 CAN/CAN-FD 链路的实测精度。

Runtime 根据该对象生成以下稳定告警码；这些都是主机模型状态或观测能力告警，不是
硬件故障诊断：

| 告警码 | 条件 |
|---|---|
| `clock_sync_observability_unavailable` | 旧文本或第三方客户端没有 v2 质量观测，仅以 `info` 标记能力未知 |
| `clock_sync_unregistered` | 节点尚未注册主机时钟模型 |
| `clock_sync_unsynced` | 模型未同步或已过期 |
| `clock_sync_degraded` | 模型状态为降级 |
| `clock_sync_error_bound_exceeded` | 有效估计的误差上界超过 Runtime 配置阈值 |
| `clock_sync_sample_stale` | 有效估计的样本年龄超过 Runtime 配置阈值 |

同一节点可同时出现状态告警和阈值告警，以保留原因；能力未知时只产生
`clock_sync_observability_unavailable`，不会猜测 `unregistered`、`unsynced` 或硬件失败。

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

当前适配器为保持边界清晰而复用 `remote-cli` 的版本化 JSON 输出。短时缓存、单飞刷新
和 `RuntimeSnapshot IPC v2` 已把一次真正刷新收敛为一个 `remote-cli` 进程和一次本地
套接字请求。toolbusd 内部的远端资源读取仍是受总资源数和总时间限制的逐项请求；未来可
用缓存、批量 Remote Packet 命令或原生语言绑定优化，但 Runtime 不能为此直接访问
SocketCAN、USB 或传输层。

这一认证竖切只解决“谁可以读取 API”的最低部署边界；TLS、反向代理信任边界、细粒度
授权、密钥热加载/撤销、速率限制与安全审计仍是后续部署门槛，不能把本轮的软件测试当作
公网暴露或硬件环境的安全实测证据。
