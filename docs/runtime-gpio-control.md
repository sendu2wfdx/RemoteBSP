# Runtime 到 toolbusd 的 GPIO 写控制边界

本轮只交付一个可在 Mock/单元测试中验证的最小竖切：Runtime 未来可通过
`libremotebsp` 或 `remote-cli`，按“登记短租约 → 写 GPIO → 释放租约”的顺序请求
`toolbusd`。网页和 Python Runtime 尚未接入该命令，代码也没有直接访问 SocketCAN、
USB 或实体板卡。本竖切不能据此宣称真实硬件数据面已经完成。

## 调用顺序

三个本地 IPC 请求均为版本化、定长数值字段加有界 ASCII 字符串的严格编码：

1. `runtime-control-acquire` 携带当前 daemon 的 128 位实例 ID、128 位租约 ID、
   调用者键 ID、唯一的 GPIO 写权限位、节点/资源和 1～30000 ms TTL。
2. `runtime-gpio-write` 必须引用已经登记且未过期的同一租约；未知租约绝不会在写入时
   隐式创建。请求还携带有界幂等键和目标电平。
3. `runtime-control-release` 只允许登记时的所有者释放。v1 IPC 没有“代替他人撤销”
   字段，调用方不能通过自声明管理员标志越权。

每次登记和写入前，`toolbusd` 都从目标节点读取静态 `ResourceDescriptor` 与
`ResourceContract`，并核对资源 ID、GPIO 类型、可写、独占写和租约支持位。要求 MCU
资源租约的合同暂不受此竖切支持，会失败关闭。合同查询期间节点离线、重启或代次变化，
以及 daemon 实例 ID 不匹配时均不会下发写命令。

## 原子性、并发与幂等

daemon 内的最终控制门以 `(node_id, resource_id)` 为互斥域；调用者不能通过自填命令
分组绕过互斥。全局锁只用于校验、占位和提交状态，可能等待远端响应的 writer 在锁外
执行。同一资源的命令有界串行等待，不同节点或资源可并发，因此单节点超时不会占住整个
Runtime 控制面。命令执行期间不能释放该资源租约或登记替代租约。

幂等域为“调用者键 ID + 幂等键”。完成结果保留 30 秒；同参数重试返回原对象 ID 并标记
`replayed`，不同参数复用键会被拒绝。活动租约、对象表与幂等历史均有上限，满载时失败
关闭，不会无界增长。GPIO 首次写使用 `GPIO_CREATE` 并记录对象 ID，后续写使用
`GPIO_WRITE`。返回成功只表示对应 Remote Packet 收到成功响应，不等于上层业务事务完成。

## 本地信任与审计边界

Unix Domain Socket 在监听前固定为 `0660`，因此当前可信调用者边界是 socket 的所有者和
部署时配置的同组进程。`owner_key_id` 与权限位是这个可信本地代理在显式登记阶段提交、
随后由 daemon 状态绑定并逐次核对的断言；它们不是密码学凭证，也不能阻止同 UID/同组的
恶意进程冒充另一个 Runtime 身份。部署时不得把 toolbusd socket 授权给普通网页进程或
不受信任用户。

v1 IPC 尚未提供持久化、抗篡改的 daemon 审计日志；当前可观测面只有调用方结果和
toolbusd 进程错误输出。因此这一竖切仍是受信本机边界内的最小控制内核，不是可直接公网
部署的完整权限系统。后续 HTTP 接入必须由已认证的 Runtime 把权限映射为固定枚举，并将
申请、拒绝、写入、幂等重放和释放写入现有脱敏审计；网页不得直接连接 toolbusd。

## 当前未覆盖

- Python Runtime/HTTP 路由及其 API 权限映射；
- 管理员撤销、续租和租约列表；
- 真实 CAN/CAN-FD、USB 或板卡验证；
- 运动、PWM、SPI、I2C 和其他写资源；
- 不可信本地多租户隔离，以及下游响应丢失后的跨进程 exactly-once 保证。

在这些边界补齐前，Runtime 根能力仍应保持 `write_commands=false` 和
`control_leases.downstream_commands=false`；现有 HTTP 控制租约不能被解释为已经获得
GPIO 写能力。
