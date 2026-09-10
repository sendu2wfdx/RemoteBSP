# Runtime 受控告警规则

Runtime 的 CPU/ISR 负载展示告警不再依赖前端硬编码阈值。规则采用版本 1 的封闭 JSON
模型，字段仅包含规则 ID、固定指标、单位、比较符、触发阈值、恢复阈值、级别和连续样本
去抖数。当前只接受 `cpu_load_permille`、`isr_load_permille`，单位固定为
`permille`，比较符固定为 `greater_or_equal` 或 `less_or_equal`；不接受表达式、脚本、
插件或任意字段。

未配置 `--alert-rule-store-dir` 时使用内置安全默认规则，规则更新只在当前进程有效。
配置专用目录后，Runtime 使用固定文件 `alert-rules-v1.json` 持久化；目录与文件不得为
符号链接，POSIX 下必须由服务用户独占。写入使用临时文件、`fsync` 和原子替换。格式损坏
会被隔离且服务拒绝隐式回退，避免悄然放宽告警边界。

读取使用 `GET /api/v1/alert-rules`，需要 `runtime.read`。更新密钥还必须显式拥有
`runtime.alert_rules.write`，并分两步完成：

1. 向 `POST /api/v1/alert-rules/preflight` 提交 `expected_revision` 和完整 `rules`；
2. 使用返回的单次短时令牌，向 `POST /api/v1/alert-rules/apply` 提交
   `confirmation_token` 和精确确认短语 `APPLY_ALERT_RULES`。

应用前会再次比较 revision；令牌过期、重复使用、并发版本变化、未知字段、错误单位、
无迟滞区间或超过 16 条规则均失败关闭。告警只有连续达到 `debounce_samples` 次才激活，
并越过独立恢复阈值后解除；测量不可用时不会把缺失值当作零。
