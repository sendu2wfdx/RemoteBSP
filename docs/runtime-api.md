# RemoteBSP Runtime API v1

## 目标与边界

Runtime API 是位于 `toolbusd` 和浏览器/上层应用之间的长期运行服务边界，作用类似
Moonraker，但面向通用 RemoteBSP 节点和资源。当前实现包含可替换 Provider 之上的只读
状态面、绑定 toolbusd 实例的进程内短时控制租约，以及 GPIO 写入/释放持久操作账本。
GPIO 输出已经形成受信本机的软件控制闭环；Runtime 不访问 SocketCAN、USB 或实体板，
committed 只证明对应远端命令收到严格成功应答且终态已持久化，不等于当前实体电平或
更高层业务事务完成。

## 普通用户运维状态

认证后的 `GET /api/v1/operations` 提供只读运维入口，仪表盘也展示同一投影。它包含
Runtime 版本和启动时长、toolbusd 连接可用性、逻辑链路录制的活动状态与有界计数、趋势
存储是否启用、SSE 当前连接数/配额，以及最多 16 条最近服务端错误码和相对发生时间。

该入口严格使用 `available`、`unavailable`、`unknown` 区分事实：未配置 toolbusd 的
Mock/文件数据源不会被标成已连接，读取失败也不会沿用旧成功值。响应不包含套接字或趋势
目录路径、录制文件名、API 密钥、命令行、后端原始错误消息或请求内容；因此它适合普通
运维查看，但不能替代物理总线抓包或实体硬件验收。

认证读取 `/api/v1/health` 还会返回 `toolbusd_health`：它通过版本化只读 IPC 取得与
daemon instance ID 原子绑定的 `HealthSnapshot v1`，再按来源、节点、生产者代际、序号、
时间和单位做严格投影。`available=false` 只隔离本次遥测失败，不影响 Runtime 资源快照；
未认证的公开存活探针仍只返回固定的 `status/scope`，不会触发 Provider 或泄漏指标。
当前健康数据仅证明 toolbusd 请求队列、租约及流量准入等软件状态，不能代表 MCU 或物理
CAN/USB 测量。

当前明确不提供：

- UART、运动、PWM、I2C、SPI 等其他写命令；
- 固件生成、构建和烧录；
- 用户目录、多租户与通用会话管理；
- WebSocket 或跨进程遥测历史库；
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
        │ Toolbusd Provider 调用版本化结构化客户端
        ▼
remote-cli / libremotebsp / toolbusd 本地套接字
        │ RuntimeSnapshot v2 + 受控 GPIO/操作账本 IPC
        ▼
toolbusd 管理的传输层
```

HTTP 层不能导入 SocketCAN、USB 或板卡实现。当前 Toolbusd Provider 通过现有
`remote-cli → libremotebsp → toolbusd Unix Domain Socket` 边界读取信息并执行受控 GPIO
操作，把节点枚举、资源目录和状态转换成同一快照；请求处理器本身不会直接发送 CAN/USB
帧，也不能绕过 daemon 的租约、静态合同或持久账本准入。

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
  --api-key-file /etc/remotebsp/runtime-api-keys.json \
  --control-audit-dir /var/lib/remotebsp/runtime-control-audit \
  --control-audit-key-file /etc/remotebsp/runtime-control-audit.key \
  --ipc-timeout-ms 2000 \
  --snapshot-cache-ms 250 \
  --status-query-workers 8 \
  --event-capacity 1024 \
  --clock-error-warning-ns 250000 \
  --clock-sample-age-warning-ms 1000
```

`--control-audit-dir` 与 `--control-audit-key-file` 必须成对配置。启用认证控制面时，缺少
任一参数都会让读取端点继续可用、但所有 mutation 在改变本地租约或调用下游前以
`control_audit_unavailable` 失败关闭。密钥是 32～64 字节原始高熵数据，必须位于当前服务
用户拥有、禁止组/其他访问且不是符号链接或多链接的普通文件；日志目录也必须由当前用户
独占。不要把密钥明文放入命令行、环境日志、Studio 工程或 Git。

与该 Runtime 配套的 `toolbusd` 现在必须显式指定持久账本目录，例如
`--runtime-operation-ledger-dir /var/lib/remotebsp/operation-ledger`。目录不能在 daemon
重启时清空；正式部署应由 `toolbusd` 专用服务用户独占。旧文本 CLI 或缺少本地 operation
方法时 `configured=false`；连接旧 daemon 时，即使本地适配器已配置，账本预检也会拒绝
首个 acquire，确保 `operational=false` 并回滚本次本地租约。

默认地址为 `http://127.0.0.1:8780/api/v1`。文件 Provider 每次请求重新读取文件，
输入上限为 1 MiB；数据损坏、文件缺失或 schema 错误返回 HTTP 503，不回退到陈旧
快照或演示数据，以免界面把旧状态误报为在线。

普通用户可直接打开 `http://127.0.0.1:8780/dashboard` 使用只读健康仪表盘。页面优先通过
`GET /api/v1/overview/stream` 接收 SSE 完整状态事件，连接失败时每 2 秒读取一次
`GET /api/v1/overview`，展示节点、链路、资源状态、节点健康快照、活动告警、最近趋势和窗口
峰值。刷新失败只影响本轮读取，并明确保留上一次成功画面；单个节点或资源渲染失败也与
其余条目隔离。页面和服务端趋势均有固定容量，节点、资源、告警及字段展开也有前端上限，
避免长期运行导致 DOM 或历史无界增长。

仪表盘严格按 `availability=available|unavailable|unknown` 显示数据；数值 `0` 只有在来源
明确标记 available 时才是有效测量，不会被当作“默认健康”。健康度同样必须是契约给出的
`healthy|degraded|fault|unknown`。CPU/ISR 阈值告警和峰值由只读 overview 投影提供，页面
不会自行补齐缺失值或改变设备状态。

未启用认证的回环开发模式可直接读取。启用 API key 后，静态页面仍只是一份不含数据的
外壳，overview 仍要求具有 `runtime.read` 权限；密钥仅保存在当前页面的内存与密码输入框
中，并通过 `X-API-Key` 请求头发送，不写入 URL、浏览器持久存储或页面日志。关闭或刷新
页面即清除。页面设置了同源 CSP、禁止嵌入和禁止 MIME 猜测，不加载第三方资源。

`GET /api/v1/overview` 是 Runtime Web 的单板只读下钻模型：每个节点包含链路、资源、
活动告警和运行时趋势，资源明确区分 `available` 与 `unavailable`；toolbusd 健康载荷
缺失、过期或无有效值时统一显示 `unknown`，绝不把缺失字段或数值 `0` 猜成健康。
趋势窗口默认按不同样本身份最多保存 60 个样本，同一快照重复读取不重复计数。峰值只统计
Runtime 契约中明确存在的非负整数测量，布尔值
不会被当成数值。CPU/ISR 指标只有在 `availability=available` 时才参与阈值告警：
千分之 800 为 warning、950 为 critical；这是一项软件展示策略，不是实体板测量结论。

趋势重启恢复默认关闭。需要时显式配置专用目录：

```bash
python3 -m runtime_api.server \
  --trend-store-dir /var/lib/remotebsp/runtime-trends \
  --trend-capacity 60 \
  --trend-store-maximum-bytes 1048576
```

目录固定使用 `trend-v1.json`，Unix 上要求目录不向组或其他用户开放；服务不会接受自定义
文件名或跟随符号链接。文件采用版本化白名单格式，只保存节点 ID、样本身份、采样时间和
非负整数指标，不保存 API key、控制请求、资源状态或原始快照。每个序列最多 600 条、总计
最多 128 个节点和 1 MiB（可在 4 KiB～16 MiB 内显式调整）。写入通过独占临时文件、
`fsync` 和原子替换完成；格式错误、未知字段或超限文件会被移到随机且不覆盖已有文件的
`trend-v1.corrupt-*.json` 隔离副本，然后从空趋势继续。无法隔离或无法原子落盘时请求明确
失败，不把内存结果冒充已持久化。节点趋势以 `snapshot_id + captured_at_ms` 去重，
toolbusd 健康趋势以 `producer_generation + sample_sequence` 去重，重启后仍保持相同语义。

`GET /api/v1/overview/stream` 使用 `text/event-stream` 主动发送与 overview 相同的完整只读
投影，复用 `runtime.read` 权限；密钥仍只允许放在 `Authorization` 或 `X-API-Key` 请求头，
不接受 URL token。浏览器因此使用支持请求头的 `fetch` 流读取，而不是会迫使密钥进入 URL
的原生 `EventSource`。默认最多 8 条 SSE 连接，每条连接固定 2 秒采样一次、只有一个最新
事件槽，单事件最多 1 MiB；慢客户端覆盖旧的未发送状态，不反压其他连接或生产端。写阻塞
受 HTTP I/O 超时约束，断连后停止该连接的生产线程并释放连接配额。空闲时发送注释心跳，
响应设置 `no-store, no-transform`、禁用代理缓冲并按认证头 `Vary`，服务端不在身份之间共享
编码后的响应。SSE 只传完整当前投影，不承诺跨断线补发；重连后以首条新事件重新建立基线。

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
- `runtime.gpio.write`：在已登记的本人 GPIO 租约范围内写输出；
- `runtime.control.operation.read`：按 operation ID 或严格 selector 查询本人操作结果。

空数组表示身份可以通过认证但不能执行上述操作，未知权限会使启动失败。权限模型不提供
通配符或隐式默认值。部署配置可按职责组合普通操作权限，只把读取和撤销赋给“监督者”；
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

Runtime 使用两条用途不同的审计路径：普通 REST 请求使用有界、非阻塞的
`SecurityAuditRecord v1`；租约申请/释放和 GPIO 写入使用同步、持久且失败关闭的
`ControlAuditJournal`。后者也不替代 `toolbusd` operation ledger，operation 的结果查询、
幂等重放和作用域阻断仍以 daemon 账本为权威。

### 普通请求审计

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

### 持久控制审计

`ControlAuditJournal(directory, key_file)` 使用规范 JSON 记录、单调序号、跨段
HMAC-SHA256 链和带 HMAC 的 manifest。默认硬上限为 65536 条、总计 64 MiB、单段
4 MiB；达到容量或遇到短写、`ENOSPC`、`EIO`、文件/目录同步失败时停止接受新的控制
变更，不静默覆盖记录。构造阶段会检查目录和密钥所有者/权限、密钥单链接、日志文件
类型与权限，并用非阻塞 `flock` 防止两个 Runtime 同时写同一目录。启动扫描发现错误
密钥、链篡改、中段截断、manifest 不一致或未知文件时失败关闭；只允许按已签名 manifest
丢弃一次未提交尾写。重启发现 durable intent 没有终态时，会在开放控制前同步补记
`unknown/process_recovery`。

控制调用遵守以下顺序：

1. `append_intent(...)` 在本地租约状态变化或任何下游 mutation 前同步完成；失败时下游
   调用次数必须为零；
2. 下游给出确定结果后，以 `append_terminal(...)` 同步 `committed`、`rejected`、
   `released` 或 `failed`，成功 HTTP 只能在该同步完成后返回；
3. 下游已开始但结果不可证明时，以 `append_unknown(...)` 记录原因和已有 operation ID，
   HTTP 固定不可直接重试，并引导查询原 operation。

控制日志只持久化认证身份、随机请求 ID、动作、可用时的规范租约 ID、HMAC 请求摘要、
结果和可用的 operation ID。它不保存 API 密钥、认证头、幂等键、原始请求体或 GPIO 值。
HMAC 请求摘要使用独立域和同一受控密钥，避免低熵控制字段被离线枚举。根能力和认证健康
响应分别报告 `control_audit` 与 `runtime_control_audit` 的 configured/operational、记录数、
段数、字节数、最后序号和未决 intent；journal 运行中发生任何持久化失败后，后续 mutation
保持关闭，而读取端点继续可用。

terminal 同步失败若发生在下游之后，不能擦除已经存在的 operation，也不能谎报确定未
提交或盲目回滚。GPIO Write/Release 继续通过 operation ID 查询；租约申请没有相同的
跨进程结果账本，其未知结果只能依赖 daemon 世代绑定、有限 TTL 和显式恢复。完整同步和
人工恢复边界见[Runtime 持久控制审计](runtime-control-audit.md)。

HMAC 是共享密钥完整性认证，不是签名、加密、可信时间或不可否认性；掌握密钥或控制同机
`root` 的主体仍可伪造/删除记录，没有外部链头锚定时也不能证明整库未被回滚。

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

六种 `data` 形状分别为：

- `traffic-status`：`traffic` 对象，包含链路模式、速率、准入汇总和固定顺序的六类
  业务计数器；
- `node-list`：`nodes` 数组，固件版本使用 `{major, minor, patch}` 对象，UUID 使用
  32 位十六进制字符串；
- `resource-list`：目标 `node_id` 和 `resources` 数组；
- `resource-status`：目标 `node_id` 和单个 `resource` 状态对象。
- `runtime-snapshot`：快照 IPC 版本和守护进程序号、流量、节点、带状态有效位的资源，
  封闭的节点级错误项，以及与节点一一对应的时钟同步质量项。
- `health-snapshot`：IPC 版本、daemon instance ID，以及来源固定为本机 toolbusd、节点为 0
  的 `HealthSnapshot v1`；物理收发和 MCU 指标没有来源时明确为 unavailable。

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
| `GET /api/v1` | schema 版本、端点与精确 configured/operational 能力声明 |
| `GET /api/v1/health` | 未认证时仅存活探针；认证后含 Provider 可用性和当前 `snapshot_id` |
| `GET /api/v1/snapshot` | 完整、同一时刻的节点/资源/告警快照 |
| `GET /api/v1/nodes` | 节点摘要、资源数和活动告警数 |
| `GET /api/v1/nodes/{node_id}` | 单节点详情与运行态 |
| `GET /api/v1/nodes/{node_id}/resources` | 单节点资源 |
| `GET /api/v1/nodes/{node_id}/alerts` | 单节点告警 |
| `GET /api/v1/resources` | 全部资源，并补充所属 `node_id` |
| `GET /api/v1/alerts` | 全部告警 |
| `GET /api/v1/events` | 版本化增量事件游标分页；只做立即返回短轮询 |
| `POST /api/v1/control-leases` | 申请有界短时租约 |
| `POST /api/v1/control/gpio/write` | 在本人活动租约范围内提交 GPIO operation |
| `DELETE /api/v1/control-leases/{lease_id}` | 本人释放或经服务端授权的监督撤销 |
| `GET /api/v1/control/operations/{operation_id}` | 查询本人持久 operation 结果 |
| `POST /api/v1/control/operation-lookups` | 用严格 kind/lease/idempotency selector 定位结果 |

`HEAD` 与对应 `GET` 返回相同状态和头部但没有响应体。表中未声明写能力的路径继续返回
HTTP 405；`OPTIONS` 按具体控制端点返回允许的方法，其他读取端点返回
`GET, HEAD, OPTIONS`。未知版本或路径返回 404。除 `/events` 的封闭 `cursor`、`limit`
外不接受查询参数，认证信息在所有路径都不得放入查询字符串。

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

`POST /api/v1/control-leases` 是控制流程的租约入口；GPIO 写入使用
`POST /api/v1/control/gpio/write`，释放使用 `DELETE /api/v1/control-leases/{lease_id}`。
这些入口都要求 Runtime 自身绑定数字回环地址、
启用 API 密钥认证并拥有 `runtime.control.lease.acquire`。非回环监听即使配置密钥也以
`control_transport_insecure` 失败关闭；远程客户端必须先由同机 TLS 反向代理终止 HTTPS，
再转发到 `127.0.0.1` 或 `::1` 上的 Runtime。请求体必须是最多 4096 字节、字段封闭且无
重复字段的 `application/json`：

```json
{
  "node_id": "mock-node-1",
  "resource_id": "gpio-0",
  "command_group": "gpio.write",
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
其他身份的租约。所有申请、幂等重放、冲突、权限拒绝、释放和撤销均进入普通脱敏请求
审计；越过权限检查的申请和释放还必须在任何状态变化前进入持久控制审计。普通请求审计
只记录身份和 `control_leases` 路径类别，不记录作用域、幂等键或请求体；控制 journal 用
HMAC 请求摘要关联规范请求，但同样不保存幂等键原文或请求体。

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

这个 HTTP 绑定不能阻止同机其他进程绕过 Runtime 连接 `toolbusd`。GPIO、PWM 和通用
定时位流已经完成受信本地纵向控制竖切；GPIO 的顺序、最终准入和本地信任边界见
[Runtime 到 toolbusd 的 GPIO 写控制边界](runtime-gpio-control.md)。
GPIO 写竖切已经把实例标识、稳定节点 UUID、节点代次和租约校验带入 toolbusd 最终
准入点，而不是只在 HTTP 入口预检。因此能力声明仅在
认证回环、daemon 世代绑定和完整结构化 GPIO IPC 已配置且至少一次最终准入成功时报告
`write_commands=true`、`control_leases.downstream_commands=true`；独立的
`gpio_write.configured/operational` 字段区分已配置与曾完成最终准入；operational 状态
还绑定 daemon 身份和单调 revision，旧并发结果不能复活已经撤销的能力证明，并始终保持
`control_leases.loopback_only=true`。`operation_ledger` 同样公开
`configured/operational/schema_version=1`，但不把仅存在本地适配代码误报为已通过 daemon
准入。PWM 使用 `pwm.write`，通用定时位流使用 `timed-bitstream.write` 租约组；后续写命令入口
必须在同一原子决策中校验身份、租约所有权和命令范围，并由 toolbusd 重新执行最终准入；
不能仅凭客户端持有一个字符串 `lease_id` 就认为已获准执行。

通用定时位流控制使用以下固定入口：

```text
POST /api/v1/control/timed-bitstream/configure
POST /api/v1/control/timed-bitstream/frame
POST /api/v1/control/timed-bitstream/stop
```

它要求 `runtime.timed_bitstream.write`，并复用同一租约、节点 UUID/代次、operation lookup、
统一期限和持久审计边界。`configure` 只接受 `bit_period_ns`、`zero_high_ns`、
`one_high_ns`、`reset_time_us` 四个时序字段；`frame` 接受 `bit_count` 和偶数长度十六进制
`data`，正文必须精确等于 `ceil(bit_count/8)`，上限为 16176 bit/2022 字节；`stop` 不接受
额外操作字段。MCU 仍只理解确定性位流，WS2812 色序、亮度、像素和动画由 Linux 生成。
账本记录 configure 参数或 frame 的完整请求摘要，不保存最多 2022 字节正文；pending 重启
恢复为 `unknown/scope_blocked`，不自动重放。成功 stop 为 `safe_closed`，远端
`LeaseRequired` 资源仍在释放控制租约后才执行 `ResourceRelease`。

控制请求现在从读取请求行之前建立一次不可续期的单调绝对期限。头部、请求体、daemon
身份单飞、目标快照、资源状态 fanout、`remote-cli` 子进程、租约登记、GPIO 写入与释放
都消费同一份剩余预算；旧接口即使不识别 deadline，也会在调用前后校验，且兼容 fanout
由 Provider 级固定线程池和单个 in-flight 世代限制，连续超时不能累积线程或子进程。

Control/Health IPC 使用[结构化错误信封 v1](ipc-error-contract.md)。Runtime 严格验证 CLI
非零退出时的 stdout、命令名、版本、数字错误码/类别、flags 与消息边界，再映射为脱敏
HTTP 错误。下游开始前的期限耗尽可声明未提交并回滚本次租约；下游开始后没有精确结果
时必须返回 `retryable=false, possibly_committed=true`。daemon 已返回的精确错误不能被事后
期限覆盖。公开无凭证 `/health` 仍只返回进程存活，不读取 Provider；认证健康读取也受同一
请求期限约束。

若目标 I/O 后的账本终态持久化失败，toolbusd 固定返回
`BackendUnavailable`、`retryable=false`、`possibly_committed=true`，而不是允许调用方直接
重放；Runtime 只可在剩余请求预算内查询原 operation，查询不可用时返回冻结 locator。

## GPIO 操作结果查询与恢复

GPIO 写入和释放使用 `toolbusd` 的版本化持久操作账本。daemon 在可能改变目标状态前先
同步 pending 记录，取得严格成功响应后再同步终态；所有账本操作响应都携带稳定的 64 个
小写十六进制 `operation_id`。HTTP 操作对象还包含 `lease_id`、稳定
`scope.expected_node_uuid/resource_id`、`operation_kind`、`state`、`recovery`、
`replayed`、结果与稳定错误。`committed` 只证明历史操作曾取得并持久化成功结果，不证明
当前实体电平。

拥有 `runtime.control.operation.read` 的同一认证身份可按 ID 查询：

```text
GET /api/v1/control/operations/{operation_id}
```

如果首次响应连 operation ID 都没有到达，可用严格 selector 定位：

```http
POST /api/v1/control/operation-lookups
Content-Type: application/json

{
  "operation_kind": "gpio_write",
  "lease_id": "0123456789abcdef0123456789abcdef",
  "idempotency_key": "gpio-write-0001"
}
```

释放定位的 `operation_kind` 为 `control_release`，幂等键固定为 `release:v1`。查询不要求
Runtime 本地租约仍活动，因此 TTL 后仍可查询；但必须匹配 owner，未知记录与其他 owner
记录返回同形 `expired_unknown`，按 ID 查询时 `operation_kind` 和 scope 为 `null`，避免
泄漏。selector 已知操作类型，因此必须回显该类型。

状态映射为：`pending` 返回 202 和 `Location`/`Retry-After`；`committed` 返回 200 并重放
持久结果；`rejected` 按稳定原因返回 409、503 或 504；`unknown` 与
`expired_unknown` 返回 409、`safe_to_retry=false`。`unknown.scope_blocked` 与
`awaiting_reboot` 冻结单个稳定 UUID/资源作用域；`safe_closed` 或
`node_reboot_confirmed` 才能解除快速缓存，权威阻断始终由 daemon 持久账本执行。账本
不可读、损坏或版本不兼容返回 503，并关闭写能力证明。

写入/释放出现 `possibly_committed=true` 时，只要本次请求尚有预算，Runtime 会使用同一
绝对 deadline 自动执行一次只读 lookup；得到 pending 或终态就按上述状态返回。查询采用
固定有界线程池和 singleflight，同一查询只产生一个 CLI 调用；短等待者超时不会取消共享
任务或让长等待者继承其 deadline。daemon identity 在查询期间变化时最多对新实例重查
一次，再次变化或不可验证即返回 503。预算耗尽或 lookup 无法取得可信结果时返回 409 与
明确 locator，绝不盲重放原写请求。

管理员代原所有者释放时，账本仍使用租约登记的真实 owner 鉴权。Runtime 在下游调用前用
有界 256 项、24 小时过期的进程内索引保存“发起管理员 + locator → 原 owner”关联；locator 请求体
不接受也不返回 owner。即时恢复失败后，同一管理员可用原 locator 延后查询，其他身份不能
借此选择或探测 owner；对外查询仍要求显式 `runtime.control.operation.read` 权限，不因撤销
权限而自动扩大读取面。并发请求各自持有服务端随机 reservation token；确定失败只撤销本次
token，不会删除其他未决请求或已绑定 operation 的映射，uncertain 返回前会把临时 token
收敛为单个 retained locator，避免同一 selector 重试造成无界增长。索引满载时在下游调用前
以 503 失败关闭；Runtime 重启或索引过期后
该关联即丢失，当前 HTTP 接口不能继续跨 owner 定位，也不会接受客户端补交 owner；需由
受控运维流程处理，不能将这项进程内便利机制描述为跨 Runtime 重启保证。

只有在“回环监听且认证已启用”使控制租约可用时，能力字段才报告 `read_only=false`；
默认无认证回环开发模式和非回环监听仍报告 `read_only=true`。即使为 `false` 也只表示
API 已有本地租约状态写入口，不能据此推断设备可写；是否存在设备命令必须检查
`write_commands`。其他路径不支持的写方法仍返回历史兼容错误码
`read_only`，该错误码在这里表示“目标路径不可写”，不是整个 Runtime 没有任何本地写
状态。

## 写命令与事件流原则

后续写 API 不应直接复用只读快照端点。建议采用 `/api/v1/commands` 或作业资源，
请求至少包含请求 ID、操作者、目标节点/资源、截止时间和幂等键；返回“已接受”不
等于 MCU 已执行。紧急停止使用专门的高优先级命令类型，不能被普通队列阻塞。

认证 SSE 完整状态推送已经实现：连接数、采样速率、单事件大小和每连接队列均有界，
慢客户端相互隔离；浏览器断线后回退到轮询。SSE 携带快照身份/修订语义，客户端不能
凭丢失的事件猜测当前设备状态。跨 Runtime 重启的事件历史与可恢复增量遥测仍未实现。

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

当前认证授权、短租约与持久操作账本解决的是“哪个密钥身份可以读、申请、本人释放、
监督撤销或查询本人操作”、单进程并发写意图互斥、GPIO 最终准入和失效安全停机，以及
toolbusd 重启后的旧租约失效、未知操作恢复和资源阻断。持久控制审计另有 16 项 journal
内核测试和 6 项 HTTP 集成测试；本阶段 Runtime 全量 164 项软件回归已通过。数字只对应
当前测试清单，不包含实体板或生产文件系统真实掉电证据。TLS、反向代理信任边界、用户
目录和动态角色、API 密钥热加载/撤销、控制审计密钥轮换与外部链头锚定、速率限制、
其他资源的原子准入、跨重启事件历史和主动推送仍是后续
部署门槛，不能把本轮的软件测试当作公网暴露、真实设备控制或硬件环境的安全实测证据。
