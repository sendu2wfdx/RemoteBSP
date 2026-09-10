# Runtime PWM 控制边界

> 当前实现状态（2026-09-11）：独立 PWM 持久操作账本及 v1 读取兼容/v2 写入已经完成，
> `toolbusd` Gate、daemon、IPC、`libremotebsp` client、`remote-cli` 与认证 Runtime HTTP 已
> 贯通 configure/stop。HTTP 使用 `POST /api/v1/control/pwm/configure|stop`，响应结果位于
> `data.operation.result`，资源状态统一从 `GET /api/v1/snapshot` 读取。租约释放、过期和
> daemon 关停按对象类型执行 `PWM_STOP`，不复用 GPIO close 语义。Studio 已有与工程配置面
> 分离的运行时适配界面，并增加默认关闭的同源认证代理；Bearer/API key 只从服务端文件
> 读取，不进入浏览器或工程。认证 HTTP 到 Mock 的普通资源、远端租约和未知申请隔离三类
> 进程闭环已纳入固定测试；实体 PWM 本轮没有新增验收，因此不能外推三板实体闭环。

## 目标

Runtime PWM 必须形成 `HTTP -> Runtime provider -> remote-cli IPC -> toolbusd ->
libremotebsp -> Remote Core -> PWM BSP` 的完整竖切。HTTP 和 Runtime provider 不得直接
持有传输，也不得绕过 `toolbusd` 的节点注册表、静态资源合同、控制租约和持久操作账本。

第一版只需要支持一个原子配置命令和安全停止：

- `configure`：在静态 PWM 资源上设置 `frequency_hz`、`duty` 和 `active_low`；若尚无
  PWM 对象则创建，已有对象则更新；
- `stop`：执行 `PWM_STOP`，确认停止后释放对象；租约释放、租约到期和 daemon 关停也
  必须走同一安全停止路径。

## 不能直接复用 GPIO 记录格式

GPIO 的持久操作记录只把布尔 `value` 纳入请求摘要和终态。PWM 配置至少有
`frequency_hz`、`duty`、`active_low` 三个会改变输出的字段。若仅复用
`RuntimeGpioWriteOperation`，相同租约和幂等键下的不同 PWM 配置会得到相同摘要，导致旧
结果被错误重放。因此可以复用 GPIO 的安全状态机和实现骨架，但不能复用其有损记录格式。

PWM 应新增独立的 `RuntimePwmConfigureOperation` 和 `OperationKind`。请求摘要必须覆盖：

- daemon 实例 ID、租约 ID、稳定节点 UUID、节点 ID、资源 ID；
- owner、权限位、幂等键；
- `frequency_hz`、`duty`、`active_low`；
- 协议版本或能唯一确定字段语义的操作种类。

终态至少保存 `object_id` 与实际接受的上述三个配置字段。账本磁盘格式变更必须提升 schema
版本，并明确拒绝未知新版本；不能让旧进程把 PWM 记录误解为 GPIO 记录。

## 权限与租约

新增细粒度权限 `runtime.pwm.write`，不得把 `runtime.gpio.write` 解释为 PWM 权限。Runtime
本地租约的 `command_group` 使用 `pwm.write`，toolbusd IPC 权限位使用独立 bit。

租约登记与执行前都必须验证：

1. daemon 实例 ID 一致；
2. lease ID、owner 和权限一致；
3. 当前节点 UUID 与请求中的稳定 UUID 一致；
4. 当前节点 generation 与租约登记时一致；
5. `resource_id` 属于当前节点的静态目录，类型为 PWM，合同允许独占写与租约；
6. 租约仍在单调时钟期限内。

同一 `(node_id, resource_id)` 只允许一个活动控制租约。一个 PWM 的失败清理只能冻结该
作用域，不得阻塞其他节点或 PWM 资源。

## 幂等与不确定提交

`configure` 和显式 `stop` 都是可能产生远端副作用的操作，必须先同步持久化 pending，再
下发远端命令，最后同步持久化 terminal。IPC 断开、期限耗尽或 terminal 持久化失败且已经
开始下发时，错误必须设置 `possibly_committed=true`，调用方只能按 operation ID 或严格
selector 查询，不能盲目换幂等键重试。

严格 selector 至少包含 owner、operation kind、lease ID、idempotency key；命中记录后仍需
核对完整请求摘要。相同 selector、不同配置参数应返回 `idempotency_conflict`。

若 pending 已持久化但尚未下发即确定失败，终态可记录 `rejected/not_sent`，此时
`possibly_committed=false`。若 terminal 无法持久化：

- 已能确认 `PWM_STOP` 成功并且对象已释放，记录/恢复语义为 `safe_closed`；
- 无法确认停止成功，记录/恢复语义为 `scope_blocked`，保持资源冻结；
- 节点以新 generation 重新注册后，可把旧对象判为不再存活，但必须留下
  `node_reboot_confirmed` 恢复证据后再解除冻结。

## 安全停止

PWM 安全态是停止输出并释放 PWM 对象，不是把 duty 改成零后继续持有对象。原因是当前
Remote Core 的 `PWM_STOP` 已定义对象释放语义，并能使资源状态从 Busy 恢复 Normal。

以下路径必须逐资源尝试 `PWM_STOP`，单个资源失败后继续清理其余资源：

- 显式 stop；
- 控制租约 release/revoke；
- 租约 TTL 到期；
- Runtime 会话或 toolbusd 关停。

若 `PWM_STOP` 响应不确定，保留对象 ID、节点 generation 和 UUID，冻结本作用域并只允许以
同一幂等操作查询/恢复；不得创建第二个对象，也不得假定资源已经空闲。

## HTTP 合同

稳定合同提供一个原子配置端点和一个显式停止端点；两者都需要 Bearer/API key：

`POST /api/v1/control/pwm/configure`

```json
{
  "lease_id": "32位十六进制",
  "node_id": "node-<32位UUID>",
  "resource_id": "resource-06000000",
  "idempotency_key": "pwm-configure-1",
  "frequency_hz": 1000,
  "duty": 5000,
  "active_low": false
}
```

`POST /api/v1/control/pwm/stop`

响应中的控制结果固定读取 `data.operation.result`，不得假设结果位于顶层。资源当前状态不
提供独立 PWM status 端点，统一从 `GET /api/v1/snapshot` 的节点资源快照读取。

Studio 不持久化认证密钥，也不允许浏览器绕过 Runtime 直连 CAN/toolbusd。只有服务器以安全
配置发布认证代理及 `configure_path`、`stop_path`、`snapshot_path` 后，Studio 才能解锁；
当前未配置，因此操作按钮失败关闭。

显式 stop 也必须带独立幂等键；不能仅依赖无幂等键的租约 DELETE 来表达设备控制操作。
HTTP 请求期限是单调绝对期限，并沿调用链传至 provider。下游开始前超时可安全重试；下游
开始后超时先自动查询账本，仍未知时返回 operation locator 和
`possibly_committed=true`。

## 最低验证矩阵

- IPC 编解码往返、截断、尾随字节、非法频率/占空比/布尔值；
- 权限拒绝、资源类型拒绝、UUID/代次变化、租约过期；
- 相同幂等键同参数重放、不同参数冲突、并发单飞；
- pending 写失败（确定未提交）、远端响应丢失（可能已提交）、terminal 写失败；
- configure 后 release、过期和 shutdown 都实际触发 `PWM_STOP`；
- 一个 PWM stop 失败时另一个 PWM 仍能配置和停止；
- Mock 三进程 E2E 从认证 HTTP 发起，最终检查 Mock PWM 快照和资源 Busy/Normal 状态，
  并确认链路只经过 toolbusd。
