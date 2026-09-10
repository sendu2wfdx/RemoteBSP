# Runtime TLS 部署基线

Runtime 仍默认只监听数字回环地址，也不会生成、申请或续期证书。部署可以由同机反向代理
终止 HTTPS，也可显式让 Runtime 在回环地址终止 TLS；两种方式都不会自动开放公网端口。
本基线提供离线配置工件、启动前检查和 Runtime 证书轮换入口。

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

本阶段不宣称已完成公网暴露、生产 CA、mTLS、OCSP、自动申请/续期证书或真实代理兼容测试。

## Runtime 启动接入

显式传入同一工件可让 Runtime 自身在回环地址终止 HTTPS：

```bash
python3 -m runtime_api.server --host 127.0.0.1 --port 8780 \
  --tls-baseline-config /etc/remotebsp/runtime-tls-baseline.json \
  --tls-rotation-audit /var/lib/remotebsp/tls-rotation-audit.json
```

启动入口会重新执行完整预检，并要求 `--host` 与工件的 `proxy_bind` 完全一致。证书和私钥
只装载一次进入内存 TLS 上下文，避免“预检通过后再次从漂移路径读取”的窗口。缺少工件、
摘要漂移、证书/私钥变化或权限变化都会在监听前失败关闭。未传该选项时行为保持原样：
Runtime 继续提供默认的回环 HTTP 开发入口。

自动测试使用运行时生成、仅存于临时目录的一日自签名证书，启动独立 Runtime 进程完成
真实 HTTPS 握手，并确认向同一端口发送明文 HTTP 得不到成功 HTTP 响应。该证书仅用于
测试，既不打包也不构成生产证书或真实公网部署证据。

## 证书轮换与连接边界

运维系统先把新证书和私钥写到新文件，重新生成完整基线工件及摘要，再向 Runtime 进程发送
`SIGHUP`。Runtime 在后台对候选工件重新执行路径、权限、摘要、绑定地址和证书/私钥匹配
检查，并构造独立 `SSLContext`；只有全部成功后才原子替换当前上下文。失败时继续使用旧
上下文，不能把半加载或错误证书提供给任何连接。

切换边界是 TLS 连接：已经完成握手的连接继续使用旧上下文，切换后接受的新连接使用新
上下文。这不是对既有连接“中途换证书”的虚假热切换。根 API 的 `capabilities.tls` 提供
当前代次、最近结果和错误；标准错误流同时输出 `runtime_tls_reload` 结构化审计事件。失败
状态明确为 `reload_failed_old_context_retained`，成功代次才递增。

配置 `--tls-rotation-audit` 后，每次启动、成功轮换和失败回滚都会先写入版本化、有界（默认
64 条）的原子替换审计文件。记录只包含时间、代次、结果以及旧/新叶证书的 SHA-256 指纹，
不记录证书路径、私钥路径或任何密钥材料。文件固定为 `0600`，内容带摘要；重启时恢复最近
记录并从下一代次继续。文件类型、权限、schema 或摘要损坏会在建立监听前失败关闭，避免
把无法证明审计连续性的服务误报为正常。该摘要用于发现损坏，并不是抵抗已取得目录写权限
攻击者的数字签名。

进程级测试使用两套临时自签名证书并发发起握手，验证观测结果只可能是完整旧证书或完整
新证书；还验证新连接切换、旧连接边界，以及篡改基线后的失败回滚。上述结果只属于软件
进程演练，不代表真实 CA、反向代理或掉电环境证据。
