# Runtime 持久化恢复软件演练

本演练在 Linux 临时目录和真实独立进程中检查三条持久化路径：Runtime 事件历史、
控制审计日志和 `toolbusd` operation ledger。它验证的是进程退出、文件截断、尾部垃圾、
损坏隔离与失败关闭的软件行为，不是断电、存储控制器缓存、文件系统日志或实体介质证明。

自动测试覆盖以下边界：

- 事件历史由一个进程原子保存，测试截断活动 JSON；新进程隔离损坏副本、从空历史重新
  建立有效快照，并确认没有遗留临时替换文件。
- 控制审计段追加未被 manifest 承诺的尾部后，新进程按已签名边界截断尾部并把悬空
  intent 恢复为 `unknown`；若再截断 manifest 已承诺的记录，新进程必须拒绝打开日志。
- operation ledger 创建并同步 pending 后被截断；真实 `toolbusd` 进程仍可启动只读诊断面，
  但 operation 查询及新控制变更返回后端不可用，不会初始化空账本掩盖损坏。

运行方式：

```bash
python3 -m unittest runtime_api.tests.test_persistence_recovery_process
ctest --test-dir build-release -R toolbusd_operation_ledger_process_tests \
  --output-on-failure
```

正式掉电验收仍需在目标 Linux 主机和预期文件系统/存储介质上，通过可控断电设备反复切断
供电，并保存内核、文件系统与原始介质证据。本软件演练不得替代该验收，也不得据此宣称
掉电安全已经完成。
