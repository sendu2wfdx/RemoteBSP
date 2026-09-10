# Runtime 持久化恢复软件演练

本演练在 Linux 临时目录和真实独立进程中检查三条持久化路径：Runtime 事件历史、
控制审计日志和 `toolbusd` operation ledger。它验证的是进程退出、文件截断、尾部垃圾、
损坏隔离与失败关闭的软件行为，不是断电、存储控制器缓存、文件系统日志或实体介质证明。

自动测试覆盖以下边界：

- 事件历史由一个进程原子保存，测试截断活动 JSON；新进程隔离损坏副本、从空历史重新
  建立有效快照，并确认没有遗留临时替换文件。
- 控制审计段追加未被 manifest 承诺的尾部后，新进程按已签名边界截断尾部并把悬空
  intent 恢复为 `unknown`；若再截断 manifest 已承诺的记录，新进程必须拒绝打开日志。
- 控制审计在真实临时文件系统中继续覆盖 HMAC 链中段损坏、多个小段轮转后的链头重放、
  独立进程锁竞争与锁释放后重启。轮转建段失败会立即毒化当前 writer，后续写入失败关闭；
  重启只清理 manifest 未承诺的空段，并把已经同步的悬空 intent 收敛为 `unknown`。
- 损坏演练使用三个独立 journal 目录：截断或链损坏只使对应控制域拒绝启动，未损坏目录
  仍可读取和重启。Runtime 的公开快照/健康读取不以 journal 可写为前提，但所有认证控制
  mutation 必须保持关闭，不能把读取面存活解释成控制面健康。
- operation ledger 创建并同步 pending 后被截断；真实 `toolbusd` 进程仍可启动只读诊断面，
  但 operation 查询及新控制变更返回后端不可用，不会初始化空账本掩盖损坏。

运行方式：

```bash
python3 -m unittest runtime_api.tests.test_control_audit_filesystem_drill
python3 -m unittest runtime_api.tests.test_persistence_recovery_process
ctest --test-dir build-release -R toolbusd_operation_ledger_process_tests \
  --output-on-failure
```

演练判定必须保留异常类别：manifest 已承诺边界之外的尾部允许自动裁剪；边界以内的短读、
HMAC 不匹配、段缺失或不连续一律是损坏，不能通过放宽断言或重建空目录变成“恢复成功”。
进程锁演练必须由两个独立 Python 进程竞争同一目录，不能只用同进程线程替代。

正式掉电验收仍需在目标 Linux 主机和预期文件系统/存储介质上，通过可控断电设备反复切断
供电，并保存内核、文件系统与原始介质证据。本软件演练不得替代该验收，也不得据此宣称
掉电安全已经完成。
