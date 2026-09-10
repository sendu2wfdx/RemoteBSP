# toolbusd 逻辑链路录制

toolbusd 的录制功能默认关闭。启动时同时提供固定目录和新的 `.rbsplog` 简单文件名，
才会在 LinkTransport 边界记录收发帧、空接收和链路错误：

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
当前启动入口适合维护窗口整段录制；运行中 IPC 启停与状态查询仍需接入守护进程控制面，
在该接口完成前不得把启动参数能力描述成在线动态录制。
