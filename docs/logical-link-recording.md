# toolbusd 逻辑链路录制

toolbusd 的录制功能默认关闭。启动时配置固定目录后，可由本地 IPC 在运行期启停；
也可同时提供初始文件名，在启动后立即记录：

```text
toolbusd can0 fd /run/toolbusd.sock \
  --runtime-operation-ledger-dir /var/lib/toolbusd/ledger \
  --logical-recording-dir /var/lib/toolbusd/recordings \
  --record-logical-link maintenance-001.rbsplog
```

这份文件是 `logical-link-boundary-only` 逻辑证据，不是 CAN/CAN-FD 电气波形、位时序
或物理抓包。调用者不能提供路径，只能选择固定目录下的文件名；文件名必须以
`.rbsplog` 结尾。已有目标不会覆盖，停止、正常退出或异常析构时都会通过唯一临时
文件和硬链接原子收口。会话最多 4096 个事件、16 MiB；达到容量或遇到非法观察后，
录制器会拒绝生成看似完整的证据。

离线消费使用同一 `TransportReplaySession` 解析、摘要和校验和验证，再执行确定性回放。

```text
remote-cli logical-recording-status
remote-cli logical-recording-start maintenance-002.rbsplog
remote-cli logical-recording-stop
```

IPC 只能提交简单文件名，目录仍由守护进程启动配置固定；未配置目录时 start/stop
失败关闭，status 明确返回 `configured=0`。状态公开活动标志、输出名、事件数和固定上限。
SIGINT、SIGTERM 与正常退出都会等待链路工作线程和 IPC 客户端退出后再收口活动会话。
这些文件始终只代表 `logical-link-boundary-only`，不得称为物理 CAN 抓包或电气证据。
