# 本地 IPC 结构化错误合同 v1

本文定义 `toolbusd` 向 `libremotebsp`、`remote-cli --json` 和 Runtime API 返回的机器可读
错误边界。当前覆盖 Runtime Control Acquire、GPIO Write、Control Release 和
HealthSnapshot；其他旧 IPC 暂时继续使用文本错误，调用方不得把文本解析为机器状态。

## 二进制信封

错误响应体由 12 字节小端头和 1～256 字节 UTF-8 消息组成：

| 字段 | 类型 | 约束 |
|---|---|---|
| `version` | `u16` | 固定为 1 |
| `header_size` | `u16` | 固定为 12 |
| `code` | `u16` | 必须是已登记错误码 |
| `category` | `u8` | 必须与错误码的固定类别一致 |
| `flags` | `u8` | bit0=`retryable`，bit1=`possibly_committed`，其他位为 0 |
| `message_len` | `u16` | 1～256，且必须与总长度完全一致 |
| `reserved` | `u16` | 固定为 0 |
| `message` | UTF-8 | 不允许 ASCII 控制字符、DEL、非法序列或尾随字节 |

错误码按请求、认证、授权、冲突、不可用、超时和内部故障分组。编码器与解码器共同使用
逐错误码的 canonical flags 白名单；不能只依赖类别推测是否可重试。

核心不变量是：`possibly_committed=true` 时 `retryable` 必须为 `false`。在 mutating packet
进入发送边界后，超时、畸形响应、连接中断或安全关闭不确定都属于可能已提交；只有发送前
拒绝或严格合法的明确错误响应才能声明确定未提交。消息只供人类诊断，任何程序分支必须
使用版本、错误码、类别和 flags。

## CLI 与 Runtime 映射

`remote-cli --json` 在失败时以非零退出码输出：

```json
{
  "schema_version": 1,
  "command": "runtime-gpio-write",
  "error": {
    "ipc_error_version": 1,
    "code": 202,
    "category": 6,
    "retryable": false,
    "possibly_committed": true,
    "message": "控制命令已开始执行但未在期限内返回"
  }
}
```

Runtime 必须同时验证进程退出状态、stdout 根对象、命令名、全部字段、数字码与类别、flags
组合和消息边界。stderr 和 `message` 不进入 HTTP。旧 CLI、畸形 JSON 或未知版本不能降级
为文本猜测；对已经开始的控制调用按 `retryable=false, possibly_committed=true` 失败关闭。

HTTP 对外只映射稳定的 `control_target_rejected`、`control_protocol_incompatible`、
`control_backend_unavailable` 和 `control_deadline_exceeded`，并公开经过验证的
`category/retryable/possibly_committed`。局部确定拒绝不应撤销其他节点或资源的可用证明；
daemon 身份不匹配、协议不可验证或提交状态未知则进入失败关闭路径。

## 兼容与测试边界

新客户端遇到覆盖请求的旧 daemon 文本错误时，只报告“信封无效或版本过旧”，不会伪造
机器字段。测试必须同时覆盖编码和解码畸形输入、Debug/Release、真实非零 CLI stdout、
迟到的精确 daemon 错误、下游调用前后期限耗尽，以及 `possibly_committed` 与 `retryable`
互斥。跨进程查询和重启恢复不属于本合同，由操作结果账本负责。
