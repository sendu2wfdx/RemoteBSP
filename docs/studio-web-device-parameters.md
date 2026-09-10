# Studio Web 受控设备参数变更

设备参数 Web 写入与恢复默认关闭。启动 Studio 服务时必须显式指定
`--enable-device-parameter-write`，并同时配置 toolbusd、
`--parameter-audit-dir` 和 `--parameter-audit-key-file`；缺少任一项都会拒绝启动该能力。
网页不能提交 socket、命令、文件路径或审计密钥。

写入和恢复都分为“预检”和“执行”两个阶段。预检读取当前设备快照，核对小写 UUID、
参数代数、参数 ID 和有界值；恢复还会校验备份版本、目标 UUID、SHA-256 完整性、重复项
及当前设备定义。预检不写设备，返回最多五分钟有效的一次性令牌和值的脱敏摘要。

执行时必须精确填写 `WRITE_DEVICE_PARAMETERS`。服务端消费令牌后再次读取 UUID 和代数，
拒绝跨设备或过时预检。随后先通过 `ParameterAuditStore` 原子保存审计意图，再调用
`DeviceParameterManager` 的 CAS 写入或恢复，读取新快照并保存成功、失败或部分失败终态。
审计只包含参数 ID、长度和值摘要，不包含参数明文或 Base64。

执行失败后令牌立即失效。页面要求重新读取设备状态和代数，不会自动重试，也不会把
未知或部分完成状态显示为成功。本功能的软件测试使用 Mock 节点；没有连接和执行实体
测试时，不能据此宣称真实板卡参数已经写入。
