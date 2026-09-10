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
租约所有者、`node_id`、`resource_id` 和固定命令组；成功结果包含 `lease_id`、
`object_id`、实际电平和 `replayed`。

释放仍使用 `DELETE /api/v1/control-leases/{lease_id}`。监督者的
`runtime.control.lease.revoke` 来自服务端认证配置；Runtime 查出登记时的真实所有者后
调用不含 `foreign/admin` 位的 v2 IPC。因此浏览器参数不能把普通调用伪装成管理员撤销。

稳定错误类别为：请求字段错误 `400/request_body_invalid`，权限或所有权错误
`403/permission_denied`、`403/control_lease_not_owner`，租约不存在或在 daemon 换代后
失效为 `404/control_lease_not_found`，范围冲突为 `409/control_lease_conflict`，控制后端
或最终准入失败为 `503/gpio_control_unavailable` 或
`503/control_lease_unavailable`。所有 HTTP 结果复用现有有界脱敏审计，仅记录认证键 ID、
方法类别、`gpio_control`/`control_leases` 路径类别和结果码，不记录请求体、幂等键或电平。

## 调用顺序

三个本地 IPC 请求均为版本化、定长数值字段加有界 ASCII 字符串的严格编码：

1. `runtime-control-acquire` 携带当前 daemon 的 128 位实例 ID、128 位租约 ID、
   预期节点 UUID、调用者键 ID、唯一的 GPIO 写权限位、节点/资源和剩余 TTL。Runtime
   先扣除本地校验与目标解析耗时，再把不超过本地截止时间的整毫秒余量交给 daemon。
2. `runtime-gpio-write` 必须引用已经登记且未过期的同一租约；未知租约绝不会在写入时
   隐式创建。请求还携带有界幂等键和目标电平。
3. `runtime-control-release` 只允许登记时的所有者释放。v2 IPC 没有“代替他人撤销”
   字段，调用方不能通过自声明管理员标志越权。

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

幂等域为“调用者键 ID + 幂等键”。完成结果保留 30 秒；同参数重试返回原对象 ID 并标记
`replayed`，不同参数复用键会被拒绝。活动租约、对象表与幂等历史均有上限，满载时失败
关闭，不会无界增长。GPIO 首次写始终先用 `GPIO_CREATE(false)` 创建安全低电平对象，
Gate 在本地登记对象与安全停机回调后才允许 `GPIO_WRITE` 切换到目标电平；后续写直接使用
已登记对象。目标写入前再次检查关停状态、租约、单调截止时间和节点代次，已过期或正在
关停的操作不会越过检查写高。返回成功只表示对应 Remote Packet 收到成功响应，不等于
上层业务事务完成。

释放、租约过期和 daemon 关停都会对已登记对象尝试安全写低，并等待正在执行的首次写
完成。CREATE 响应丢失会把该资源标记为“未知对象”并失败关闭；目标 WRITE 响应不确定时
立即尝试安全写低，安全停机本身失败则只 poison 当前 `(node_id, resource_id)`，不阻塞
其他资源。已安全退休的对象在相同节点代次内不再复用，只有节点代次变化或 daemon 重启
后才允许重新创建，避免缺少对象销毁协议时留下不可证明的远端对象。关停会扫描对象表而
非只扫描租约表，确保局部登记异常也不会漏掉已知对象。

## 本地信任与审计边界

Unix Domain Socket 在监听前固定为 `0660`，因此当前可信调用者边界是 socket 的所有者和
部署时配置的同组进程。`owner_key_id` 与权限位是这个可信本地代理在显式登记阶段提交、
随后由 daemon 状态绑定并逐次核对的断言；它们不是密码学凭证，也不能阻止同 UID/同组的
恶意进程冒充另一个 Runtime 身份。部署时不得把 toolbusd socket 授权给普通网页进程或
不受信任用户。

v2 IPC 尚未提供持久化、抗篡改的 daemon 审计日志；HTTP 层虽然已有有界脱敏审计，仍是
进程内存记录。因此这一竖切仍是受信本机边界内的最小控制闭环，不是可直接公网部署的
完整权限系统。

## 当前未覆盖

- 续租和租约列表；
- 真实 CAN/CAN-FD、USB 或板卡验证；
- 运动、PWM、SPI、I2C 和其他写资源；
- 不可信本地多租户隔离，以及下游响应丢失后的跨进程 exactly-once 保证；
- `GPIO_CLOSE`/显式对象销毁；当前已退休对象在同一节点代次内不会再次分配；
- 可区分业务拒绝、目标错误和传输不确定性的结构化 IPC 错误。

Runtime 根能力只在“回环监听、启用认证、使用 daemon 世代绑定租约管理器、Provider
确认结构化 acquire/write/release 三个 IPC 均存在”时报告
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

下一轮安全阻断项是补齐 `GPIO_CLOSE` 与结构化 IPC 错误，并把 HTTP 解析、身份读取、
目标解析、IPC 登记和写响应收敛为一个统一的
端到端总 deadline，并定义“本地租约在远端命令执行期间跨过期”的 exactly-once 状态机；
当前只保证已完成命令的 30 秒 daemon 幂等重放，不宣称跨过期或进程崩溃 exactly-once。
daemon 审计的持久化、完整性保护和失败事件增强也仍是部署前置项。
