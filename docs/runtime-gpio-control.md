# Runtime 到 toolbusd 的 GPIO 写控制边界

本轮交付一个可在 Mock/单元测试中验证的最小竖切：认证后的 Python Runtime HTTP
控制面通过 `remote-cli`/`libremotebsp`，按“登记短租约 → 写 GPIO → 释放租约”的
顺序请求 `toolbusd`。网页仍不能直接连接 toolbusd，Runtime 也不直接访问 SocketCAN、
USB 或实体板卡。本竖切不能据此宣称真实硬件数据面已经完成。

## HTTP 契约

GPIO 租约仍由 `POST /api/v1/control-leases` 申请，但 `command_group` 必须固定为
`gpio.write`，且认证身份同时拥有 `runtime.control.lease.acquire` 和
`runtime.gpio.write`。成功的新租约只有在 toolbusd 完成显式登记、且 Runtime 再次确认
租约尚未过期后才返回 HTTP 201；幂等重放返回 200。

写入使用 `POST /api/v1/control/gpio/write`：

```json
{
  "lease_id": "0123456789abcdef0123456789abcdef",
  "node_id": "node-00112233445566778899aabbccddeeff",
  "resource_id": "resource-01000005",
  "idempotency_key": "gpio-write-0001",
  "value": true
}
```

请求体字段封闭、拒绝重复字段，`value` 必须是 JSON 布尔值。Runtime 从认证结果取得
`owner_key_id` 和权限，客户端不能在请求体中自报身份或管理员标志。写前会重新核对活动
租约所有者、`node_id`、`resource_id` 和固定命令组；成功结果以 operation 对象返回，包含
`operation_id`、`lease_id`、稳定 scope、`object_id`、实际电平、状态和 `replayed`。

释放仍使用 `DELETE /api/v1/control-leases/{lease_id}`。监督者的
`runtime.control.lease.revoke` 来自服务端认证配置；Runtime 查出登记时的真实所有者后
调用不含 `foreign/admin` 位的 v2 IPC。因此浏览器参数不能把普通调用伪装成管理员撤销。

稳定错误类别为：请求字段错误 `400/request_body_invalid`，权限或所有权错误
`403/permission_denied`、`403/control_lease_not_owner`，租约不存在或在 daemon 换代后
失效为 `404/control_lease_not_found`，范围冲突为 `409/control_lease_conflict`。下游结构化
错误按稳定机器字段映射为 `409/control_target_rejected`、
`502/control_protocol_incompatible`、`503/control_backend_unavailable` 或
`504/control_deadline_exceeded`；错误详情只公开 `category`、`retryable` 和
`possibly_committed`，不公开 daemon 消息。所有 HTTP 结果复用现有有界脱敏审计，仅记录认证键 ID、
方法类别、`gpio_control`/`control_leases` 路径类别和结果码，不记录请求体、幂等键或电平。

## 调用顺序

三个本地 IPC 请求均为版本化、定长数值字段加有界 ASCII 字符串的严格编码：

1. `runtime-control-acquire` 携带当前 daemon 的 128 位实例 ID、128 位租约 ID、
   预期节点 UUID、调用者键 ID、唯一的 GPIO 写权限位、节点/资源和剩余 TTL。Runtime
   先扣除本地校验与目标解析耗时，再把不超过本地截止时间的整毫秒余量交给 daemon。
2. `runtime-gpio-write-operation` 必须引用已经登记且未过期的同一租约；未知租约绝不会
   在写入时隐式创建。请求还携带有界幂等键和目标电平，并返回持久 operation 状态。
3. `runtime-control-release-operation` 只允许登记时的所有者释放。v2 IPC 没有“代替他人撤销”
   字段，调用方不能通过自声明管理员标志越权。

一次 HTTP 请求在读取头部前建立不可续期的单调绝对期限。请求体、daemon 身份单飞、目标
快照、资源状态 fanout、`remote-cli` 子进程、登记、写入和释放都消费同一份剩余预算；任何
层都不能重新获得完整超时。下游调用前耗尽期限表示确定未提交，可以回滚本次新租约；下游
开始后才超时则返回 `retryable=false, possibly_committed=true`，不能据此直接重发。daemon
已经返回的精确错误优先于事后期限，不能被笼统 504 覆盖。

每次登记和写入前，`toolbusd` 都在当前节点 registry 中原子核对数字 node ID 与预期
UUID，再从目标节点读取静态 `ResourceDescriptor` 与
`ResourceContract`，并核对资源 ID、GPIO 类型、可写、独占写和租约支持位。要求 MCU
资源租约的合同暂不受此竖切支持，会失败关闭。合同查询期间节点离线、重启或代次变化，
数字 ID 被其他 UUID 复用，以及 daemon 实例 ID 不匹配时均不会下发写命令。租约还保存
取得时的节点代次；实际 GPIO_CREATE/GPIO_WRITE 下发前再次核对 UUID 与代次。

## 原子性、并发与幂等

daemon 内的最终控制门以 `(node_id, resource_id)` 为互斥域；调用者不能通过自填命令
分组绕过互斥。全局锁只用于校验、占位和提交状态，可能等待远端响应的 writer 在锁外
执行。同一资源的命令有界串行等待，不同节点或资源可并发，因此单节点超时不会占住整个
Runtime 控制面。命令执行期间不能释放该资源租约或登记替代租约。

幂等域为“调用者键 ID + 幂等键”。Gate 内存镜像保持有界，权威 operation 结果写入
`toolbusd` 持久账本；同参数重试返回原 operation/对象 ID 并标记 `replayed`，不同参数复用
键会被拒绝。活动租约、对象表、内存镜像和持久账本均有硬上限，满载时在目标 I/O 前失败
关闭，不会无界增长。GPIO 首次写始终先用 `GPIO_CREATE(false)` 创建安全低电平对象，
Gate 在本地登记对象与安全停机回调后才允许 `GPIO_WRITE` 切换到目标电平；后续写直接使用
已登记对象。目标写入前再次检查关停状态、租约、单调截止时间和节点代次，已过期或正在
关停的操作不会越过检查写高。返回成功只表示对应 Remote Packet 收到成功响应，不等于
上层业务事务完成。

释放、租约过期和 daemon 关停都会等待正在执行的首次写完成，再对已登记对象执行安全
写低和版本化 `GPIO_CLOSE`。固件把对象绑定到创建它的传输会话，跨会话 READ/WRITE/CLOSE
均拒绝；会话结束也只清理本会话对象。Close 对不存在对象幂等成功，响应不确定时保留本地
故障占位并只重试 Close；只有收到确定成功才删除对象和租约，因此同一节点代次随后可以
安全重建。写低或 Close 失败只 poison 当前 `(node_id, resource_id)`，不会释放远端所有权
给新调用者，也不阻塞其他资源。CREATE 响应丢失仍会把该资源标记为“未知对象”并失败关闭。
关停扫描对象表而非只扫描租约表，确保局部登记异常也不会漏掉已知对象。

## 本地信任与审计边界

Unix Domain Socket 在监听前固定为 `0660`，因此当前可信调用者边界是 socket 的所有者和
部署时配置的同组进程。`owner_key_id` 与权限位是这个可信本地代理在显式登记阶段提交、
随后由 daemon 状态绑定并逐次核对的断言；它们不是密码学凭证，也不能阻止同 UID/同组的
恶意进程冒充另一个 Runtime 身份。部署时不得把 toolbusd socket 授权给普通网页进程或
不受信任用户。

操作结果账本已经持久化，但它不是操作者审计日志，也不提供密码学防篡改；HTTP 脱敏审计
仍是进程内存记录。因此这一竖切仍是受信本机边界内的最小控制闭环，不是可直接公网部署
的完整权限系统。

## 当前未覆盖

- 续租和租约列表；
- 真实 CAN/CAN-FD、USB 或板卡验证；
- 运动、PWM、SPI、I2C 和其他写资源；
- 不可信本地多租户隔离；
- 可靠 MCU boot generation 驱动的自动解冻，以及实体掉电/链路断开下的安全时延证明；
- 无限期结果保留或对 `expired_unknown` 的可重放承诺（明确不提供）。

Runtime 根能力只在“回环监听、启用认证、使用 daemon 世代绑定租约管理器、Provider
确认结构化 acquire、operation write/release、status/lookup IPC 均存在”时报告
`gpio_write.configured=true`。初始 `operational=false`；只有完整的“登记 → 下游幂等
登记 → 最终 authorize”成功后才置为 true，幂等重放也必须重新访问下游。该证明绑定
daemon 身份和单调 revision；迟到的旧 acquire/write 结果不能覆盖较新的失效状态，旧
daemon 或不兼容 IPC 也不能虚报可用。`write_commands` 与
`control_leases.downstream_commands` 只跟随 operational，端点列表则跟随 configured。
Mock、文件 Provider、旧文本 remote-cli、非回环监听或未认证模式均保持关闭。

普通目标解析或下游登记失败只回滚本次新租约，不影响其他节点/资源的活动租约；只有
daemon 身份变化、不可读或格式无效才全局失效。若 daemon 已成功登记但 Runtime 在返回前
发现本地租约过期或世代变化，会用登记时的真实所有者做一次 best-effort 幂等释放，再以
泛化 503 失败，响应不会回显 remote-cli stderr、套接字路径或底层异常文本。

结构化 IPC 错误、统一端到端 deadline 和
[不确定提交恢复与操作结果账本](runtime-operation-ledger.md)已完成软件闭环。写入和释放在
目标 I/O 前持久化 pending、成功返回前持久化终态；Runtime 可按 operation ID 或严格
selector 跨本地租约 TTL 查询，并把 pending 映射为 202、unknown/expired_unknown 映射为
不可直接重试的 409。daemon 重启会恢复 durable pending 为 unknown 并重建资源阻断。
这些保证来自单元、Mock 与本地进程测试，不是实体 GPIO 电平或掉电文件系统验证；daemon
操作者审计的持久化、完整性保护和失败事件增强也仍是部署前置项。
