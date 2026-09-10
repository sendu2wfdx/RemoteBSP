# Runtime TLS 部署基线

Runtime 仍默认只监听数字回环地址。当前软件不直接终止 TLS，也不生成、申请或续期证书；
远程部署应由同机反向代理终止 HTTPS，再把请求转发到回环 Runtime。本基线是离线配置工件
和启动前检查器，不会启动监听器，更不会自动开放公网端口。

创建工件时必须引用运维人员已经准备好的绝对证书和私钥路径：

```bash
python3 -m runtime_api.tls_deployment create \
  --output /etc/remotebsp/runtime-tls-baseline.json \
  --runtime-bind 127.0.0.1 --proxy-bind 127.0.0.1 \
  --certificate-file /etc/remotebsp/tls/server.crt \
  --private-key-file /etc/remotebsp/tls/server.key
python3 -m runtime_api.tls_deployment preflight \
  --config /etc/remotebsp/runtime-tls-baseline.json
```

预检要求 Runtime 与代理绑定均为数字回环地址；配置、摘要和私钥为当前服务用户拥有的
单链接普通文件且权限严格为 `0600`；证书不可由组或其他用户写入；所有路径拒绝符号链接。
检查器还让系统 TLS 库实际加载证书链与私钥，从而拒绝损坏或不匹配的文件，最低版本固定
为 TLS 1.2。

反向代理必须删除 `Forwarded`、`X-Forwarded-For`、`X-Real-IP` 以及任何注入身份的头部。
Runtime 只信任直连对端和自身 API key，不把代理提供的客户端地址或身份当作授权依据。
配置工件采用严格字段集合，并附带 SHA-256 完整性摘要；摘要用于发现意外篡改，不是签名，
不能抵御已经取得配置目录写权限的攻击者。正式部署还应由只读配置管理或包管理系统固定
工件，并由外部监控检查证书有效期和吊销状态。

本阶段不宣称已完成公网暴露、生产 CA、mTLS、OCSP、证书自动轮换或真实代理兼容测试。

## Runtime 启动接入

显式传入同一工件可让 Runtime 自身在回环地址终止 HTTPS：

```bash
python3 -m runtime_api.server --host 127.0.0.1 --port 8780 \
  --tls-baseline-config /etc/remotebsp/runtime-tls-baseline.json
```

启动入口会重新执行完整预检，并要求 `--host` 与工件的 `proxy_bind` 完全一致。证书和私钥
只装载一次进入内存 TLS 上下文，避免“预检通过后再次从漂移路径读取”的窗口。缺少工件、
摘要漂移、证书/私钥变化或权限变化都会在监听前失败关闭。未传该选项时行为保持原样：
Runtime 继续提供默认的回环 HTTP 开发入口。

自动测试使用运行时生成、仅存于临时目录的一日自签名证书，启动独立 Runtime 进程完成
真实 HTTPS 握手，并确认向同一端口发送明文 HTTP 得不到成功 HTTP 响应。该证书仅用于
测试，既不打包也不构成生产证书或真实公网部署证据。
