# RemoteBSP Runtime API v1

## 目标与边界

Runtime API 是位于 `toolbusd` 和浏览器/上层应用之间的长期运行服务边界，作用类似
Moonraker，但面向通用 RemoteBSP 节点和资源。当前实现可替换 Provider 之上的只读
状态面，以及绑定 toolbusd 实例的进程内短时控制租约；控制租约只协调上位机写意图，不访问 SocketCAN、
USB 或实体板，也不表示设备命令已经执行。

当前明确不提供：

- GPIO、UART、运动等写命令；
- 固件生成、构建和烧录；
- 用户目录、多租户与通用会话管理；
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
        │ HTTP /api/v1，状态读取 + 短时控制租约
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
  --event-capacity 1024 \
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
  "schema_version": 2,
  "keys": [{
    "key_id": "studio-readonly",
    "api_key": "REPLACE_WITH_RANDOM_ASCII_KEY_AT_LEAST_32_BYTES",
    "permissions": ["runtime.read"]
  }]
}
```

```sh
chmod 600 /etc/remotebsp/runtime-auth.json
python3 -m runtime_api.server \
  --host 192.0.2.10 \
  --api-key-file /etc/remotebsp/runtime-auth.json
```

认证配置 v2 最多 16 KiB、16 个密钥；`key_id` 是 1～64 字节的稳定 ASCII 审计身份，
只允许字母、数字、点、下划线和连字符，且不能重复。每个密钥为 32～128 字节的非空白
可打印 ASCII。重复密钥、重复 JSON 字段、未知字段、未知版本、空密钥数组和格式错误
都会使启动失败。v1 字符串密钥配置不会被静默解释成 v2 身份。密钥只从文件读取，
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

每个身份的 `permissions` 是有界、无重复的权限数组。支持的权限为：

- `runtime.read`：读取状态、健康和事件；
- `runtime.control.lease.acquire`：申请本人名下的短时控制租约；
- `runtime.control.lease.release`：释放本人名下的租约；
- `runtime.control.lease.revoke`：监督者撤销任意身份的租约。

空数组表示身份可以通过认证但不能执行上述操作，未知权限会使启动失败。权限模型不提供
通配符或隐式默认值。部署配置可把前三项赋给“操作者”，只把读取和撤销赋给“监督者”；
这是显式权限组合，不是尚未实现的用户目录或动态角色继承。读取权限不能申请租约，申请
权限也不能读取设备状态；未经授权的路径返回 HTTP 403、`permission_denied`。

认证器只长期保存每个配置密钥的 SHA-256 固定长度摘要，不保留明文密钥；候选值也先
计算同长度摘要，再对全部配置项使用常量时间比较原语，且不在首个匹配处提前返回。
这减少了比较阶段泄露密钥长度类别和进程内长期保留明文的风险，但不替代高熵密钥要求。
缺少密钥和错误
密钥统一返回 HTTP 401、`authentication_required`；重复认证头、同时提供两种认证方式
或 Bearer 格式错误返回 HTTP 400、`invalid_auth_header`。任何名为 `api_key`、
`api-key`、`apikey`、`x-api-key`、`access_token` 或 `token` 的查询参数均以 HTTP 400、
`credential_in_query` 拒绝，避免密钥进入访问日志、浏览器历史和代理缓存。

认证启用时，`/api/v1` 下除下述边界外均受同一中间件保护，包括控制租约方法；其他写
请求通过认证后仍返回 `read_only`。`GET/HEAD /api/v1/health` 在未携带认证头时只返回
`status=ok,scope=liveness`，不读取 Provider，也不暴露快照、节点或能力；携带有效密钥
时返回完整健康信息。携带错误密钥不会降级成公开探针，而是返回 401。
`OPTIONS /api/v1...` 无需密钥并只返回方法边界，不启用 CORS、也不允许凭据跨域。

## 安全审计

每个由 Runtime 处理的 REST 请求最多生成一条 `SecurityAuditRecord v1`。服务端使用
密码学随机源生成 32 位十六进制 `request_id`，并通过 `X-Request-ID` 响应头返回同一值，
便于把客户端故障与审计事件关联。记录的封闭结构为：

```json
{
  "schema_version": 1,
  "occurred_at_ms": 1789000000000,
  "request_id": "0123456789abcdef0123456789abcdef",
  "key_id": "studio-readonly",
  "method_category": "read",
  "path_category": "snapshot",
  "result": "allowed"
}
```

`method_category` 只会是 `read`、`write`、`options` 或 `other`；`path_category` 只会是
`root`、`health`、`snapshot`、`nodes`、`resources`、`alerts`、`events`、
`control_leases` 或 `unknown`。
未认证请求的
`key_id` 为 `null`。`result` 使用稳定 API 错误码，成功读取为 `allowed`，公开存活探针为
`public_probe`，预检为 `options`。记录不会保存 API 密钥、Authorization、X-API-Key、
原始路径、节点 ID、查询参数、请求体或响应体；即使攻击者把秘密放入 URL，也只会留下
`path_category` 和 `credential_in_query`。
`occurred_at_ms` 是 Runtime 主机的 Unix 毫秒墙上时间，只用于日志排序，不是设备时间、
时钟同步精度或防篡改时间证据。

默认 `BoundedAuditSink` 是线程安全的 256 条进程内环形缓冲，满时覆盖最旧记录并增加
`overwritten_count`，不会无界增长。嵌入服务可通过
`make_server(..., audit_output=..., audit_capacity=...)` 注入接收结构化字典的输出端。
输出端由独立守护线程消费同容量的有界队列，请求线程只执行非阻塞入队；队列满时增加
`dropped_output_count` 并丢弃该次外部转发，但进程内环形记录仍按自身覆盖规则保留。
输出异常被隔离并增加 `output_failure_count`，不改变 HTTP 结果。

HTTP 服务关闭时会通知输出线程停止，并最多等待 1 秒排空；输出端永久阻塞时关闭仍有界，
守护线程不会阻止进程退出。输出回调不应执行递归审计，也必须自行负责可靠落盘、文件轮换
和完整性保护。默认环形缓冲不是持久审计存储；跨进程可靠交付和集中采集仍是后续部署边界。

## 增量事件短轮询

`GET /api/v1/events` 提供 `RuntimeEvent v1` 的立即返回式短轮询，不建立 WebSocket、SSE
或服务端等待线程。它与其他只读端点调用同一个 `RuntimeProvider.read_snapshot()`；一次
成功的 RuntimeSnapshot 会在进程内差分为节点、资源、告警和时钟质量事件。Provider
缓存命中仍由既有缓存策略合并，读取失败返回原有 `provider_unavailable`，不会推进事件
状态或游标；恢复后的首个成功快照再与失败前状态比较。

首次调用不带 `cursor` 时，服务先观察当前快照，然后返回空 `events` 和当前
`next_cursor`，作为增量基线。客户端应先取得该基线游标，再读取完整 `/snapshot`，此后
携带游标短轮询；基线与完整快照之间的变化可能被重复应用，但不会因该顺序漏掉，客户端
应按实体 ID 幂等更新。事件不能替代完整快照，也不能作为控制命令。

```http
GET /api/v1/events?cursor=e1:0123456789abcdef0123456789abcdef:42&limit=50
X-API-Key: <api-key>
```

成功数据为：

```json
{
  "event_schema_version": 1,
  "events": [{
    "event_schema_version": 1,
    "sequence": 43,
    "cursor": "e1:0123456789abcdef0123456789abcdef:43",
    "snapshot_id": "snapshot-42",
    "captured_at_ms": 42000,
    "entity_type": "resource",
    "change": "updated",
    "node_id": "toolboard-1",
    "resource_id": "gpio-0",
    "alert_id": null,
    "payload": {"resource_id": "gpio-0"},
    "payload_omitted": false
  }],
  "next_cursor": "e1:0123456789abcdef0123456789abcdef:43",
  "has_more": false
}
```

游标是严格的 `e1:<128位小写十六进制日志incarnation>:<十进制序号>`，不能猜测为
时间戳。incarnation 在 Runtime 进程创建事件日志时随机生成；测试或嵌入调用可以显式注入，
但线上不得复用旧进程的值。`limit` 默认 50，只允许规范十进制 1～100；参数重复、未知
参数、未知版本、负数、前导零、超长或领先服务端的游标均返回稳定 400 错误。日志默认
保留 1024 条，可由 `--event-capacity` 配置为 1～4096 条。游标早于保留窗口或 incarnation
与当前进程不匹配时返回 HTTP 409、`event_cursor_expired`，并在
`error.details.reset_cursor` 提供当前游标；客户端必须重新读取完整快照后才能使用重置
游标，不能把日志轮换或进程重启造成的缺口伪装成连续事件。incarnation 使用 128 位系统
随机数防止跨进程误接受，但它不是持久计数器；其唯一性保证仍是概率性的。

同一快照和内容相同的后续快照不会重复产生事件。单次差分按 `node`、`resource`、
`alert`、`clock_quality` 固定类别顺序，再按稳定实体 ID 排序；并发观察由同一锁串行分配
递增序号。节点事件只比较身份、显示信息、在线状态和链路，不因 `last_seen_ms` 或队列
水位产生事件风暴。时钟质量比较忽略只随读取时间增长的 `sample_age_ms`，但保留模型代次、
状态、误差/漂移估计和最后样本等真实模型变化；样本跨越陈旧阈值仍会通过既有告警变化
显式呈现。`captured_at_ms` 更旧的迟到快照不会逆向覆盖状态；相同采集时间若对应不同
`snapshot_id` 或不同规范事件内容，日志无法建立可靠全序，会 fail-closed 返回
`event_log_unavailable`。下一份采集时间更大的合法快照可恢复日志。已经被更新状态判定为
过旧的异常快照也不会污染当前健康日志。

日志最多跟踪 4096 个实体和 2 MiB 规范状态。单事件 payload 最大 4096 字节，超出时
仍保留身份、变化类别和顺序，但置 `payload=null,payload_omitted=true`，提示客户端读取
完整快照。每页最多 100 项，因此响应大小和序列化工作有固定上限。慢客户端只持有一次
有限响应的请求线程；服务端不等待下一事件，也不允许通过长轮询持续占用工作线程。
所有事件读取都要求 `runtime.read`，公开健康探针边界不变，且没有任何事件写入或确认 API。
事件只表示 Runtime 两次成功快照之间观察到的状态差异；在没有快照读取的间隔内发生并
恢复的瞬态变化可能不可见，不能把该软件日志当作 MCU 主动推送、总线抓包或硬件实测证据。

## remote-cli 结构化只读契约

以下五种调用支持统一的全局 `--json` 开关：

```sh
remote-cli --json --socket /tmp/toolbusd.sock traffic-status
remote-cli --json --socket /tmp/toolbusd.sock daemon-identity
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
| `GET /api/v1/events` | 版本化增量事件游标分页；只做立即返回短轮询 |

`HEAD` 与对应 `GET` 返回相同状态和头部但没有响应体。认证通过后的任何写方法返回
HTTP 405；`OPTIONS` 返回 HTTP 204 和 `Allow: GET, HEAD, OPTIONS`；未知版本或路径
返回 404。除 `/events` 的封闭 `cursor`、`limit` 外不接受查询参数，认证信息在所有路径
都不得放入查询字符串。

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

## 短时控制租约

`POST /api/v1/control-leases` 是当前唯一写入口，要求 Runtime 自身绑定数字回环地址、
启用 API 密钥认证并拥有 `runtime.control.lease.acquire`。非回环监听即使配置密钥也以
`control_transport_insecure` 失败关闭；远程客户端必须先由同机 TLS 反向代理终止 HTTPS，
再转发到 `127.0.0.1` 或 `::1` 上的 Runtime。请求体必须是最多 4096 字节、字段封闭且无
重复字段的 `application/json`：

```json
{
  "node_id": "mock-node-1",
  "resource_id": "gpio-0",
  "command_group": "write",
  "ttl_ms": 5000,
  "idempotency_key": "studio-request-0001"
}
```

`ttl_ms` 只允许 100～30000。节点、资源、命令组和幂等键均为长度有界的规范 ASCII
标识；它们建立协调命名空间，不证明对应节点或资源存在。相同身份以相同幂等键和完全
相同参数重试，会返回原租约且不延长截止时间；把同一幂等键用于不同参数会被拒绝。同一
节点和资源在同一时刻只有一个租约，命令组名称不能建立第二个排他域来绕过冲突。租约被
主动释放、监督撤销或自然过期后，其“身份 + 幂等键”终态继续保留 30 秒；这段时间内即使
参数完全相同也只返回 HTTP 409、`control_lease_conflict`，不会把已结束的操作复活为新
租约，调用者必须改用新的幂等键。30 秒从释放或检测到过期的单调时钟时刻开始计算。
并发竞争由进程内互斥锁线性化，失败方同样收到 HTTP 409。

新租约返回 HTTP 201，幂等重放返回 HTTP 200。响应含 32 位随机十六进制 `lease_id`、
审计身份、作用域以及墙上时间表示的取得和预计过期时间。实际过期判定使用单调时钟，
不会因系统墙钟回拨延长租约。默认最多 256 个活动租约，可由
`--control-lease-capacity` 在 1～4096 范围内调整；过期项在下一次操作或计数时转入上述
终态历史。终态历史同样有界：活动项与终态项合计上限为活动容量的 4 倍且最多 4096 条，
不会为了接受新申请提前淘汰仍处于 30 秒保护期的终态。活动表或合计历史达到上限时，申请
以 HTTP 503、`control_lease_capacity_exceeded` 失败关闭；待租约或终态保护期届满并在
下一次操作中回收后才恢复容量。

`DELETE /api/v1/control-leases/{lease_id}` 不接受请求体。所有者需要
`runtime.control.lease.release`；持有 `runtime.control.lease.revoke` 的监督者可撤销
其他身份的租约。所有申请、幂等重放、冲突、权限拒绝、释放和撤销均进入现有脱敏审计，
审计只记录身份和 `control_leases` 路径类别，不记录作用域、幂等键或请求体。

服务在连接交给有限工作线程之前设置单次 I/O 空闲超时，并另外执行两个不会被持续滴入
字节重置的总期限：请求处理开始至完整 HTTP 头部解析完成为“头部总期限”，控制请求体
开始读取至完整收齐为“请求体总期限”。三者默认均为 5 秒，并统一由
`--http-request-timeout-ms` 在 100～30000 ms 范围内调整，避免慢速请求行、慢速头部或
慢速请求体无限占用线程。也就是说，攻击者即使在每次 socket 空闲超时前持续发送少量
字节，仍会分别触发头部或请求体的绝对总期限。控制请求明确拒绝任何
`Transfer-Encoding`，包括同时携带
`Content-Length` 的请求，只接受单一且规范的十进制 `Content-Length`。这是一层应用
防线；生产部署仍应在反向代理设置独立的头部、请求体和总请求期限以及连接数限制。

这不是分布式锁，也不是安全执行令牌。租约只存在于单个 Runtime 进程，重启后全部丢失；
当前没有续租、列表或跨 Runtime 实例一致性。使用 `--toolbusd-socket` 时，Runtime 会在
每次申请、释放或撤销前，通过版本化 `daemon-identity` 本地 IPC 读取 toolbusd 本次进程
的非零 128 位随机实例标识。并发请求共享一次在锁外执行的身份读取，身份结果与租约变更
则在进程锁内串行提交；标识变化、不可读或格式无效都会立即清空当前租约与幂等历史并
失败关闭。因此 toolbusd 重启后，旧 Runtime
租约不能继续释放或被后续命令路径误用；重新申请从新实例世代开始。未配置 toolbusd 的
Mock/文件 Provider 仍使用 `runtime_process` 进程世代。能力字段
`control_leases.backend_binding` 分别报告 `toolbusd_instance` 或 `runtime_process`。

这个绑定不能阻止同机其他进程绕过 Runtime 连接 `toolbusd`，也不会发送任何设备写命令。
身份检查与未来实际命令之间仍可能发生 daemon 重启；真正的写命令竖切必须把实例标识和
租约校验带入 toolbusd 的同一原子准入点，而不能只在 HTTP 入口预检。因此能力声明保持
`write_commands=false`、`control_leases.downstream_commands=false`，且
`control_leases.loopback_only=true`。未来写命令入口
必须在同一原子决策中校验身份、租约所有权和命令范围，并由 toolbusd 重新执行最终准入；
不能仅凭客户端持有一个字符串 `lease_id` 就认为已获准执行。

只有在“回环监听且认证已启用”使控制租约可用时，能力字段才报告 `read_only=false`；
默认无认证回环开发模式和非回环监听仍报告 `read_only=true`。即使为 `false` 也只表示
API 已有本地租约状态写入口，不能据此推断设备可写；是否存在设备命令必须检查
`write_commands`。其他路径不支持的写方法仍返回历史兼容错误码
`read_only`，该错误码在这里表示“目标路径不可写”，不是整个 Runtime 没有任何本地写
状态。

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

当前认证授权和短租约竖切解决的是“哪个密钥身份可以读、申请、本人释放或监督撤销”、
单进程内并发写意图互斥，以及 toolbusd 进程重启后的旧租约失效。TLS、反向代理信任边界、
用户目录和动态角色、密钥热加载/撤销、速率限制、租约与设备命令的原子准入绑定，以及审计异步持久化与完整性保护仍是后续
部署门槛，不能把本轮的软件测试当作公网暴露、真实设备控制或硬件环境的安全实测证据。
