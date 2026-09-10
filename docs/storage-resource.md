# 通用 Storage 资源

第一阶段把 Storage 定义为版本化、定长、可擦除的块资源，而不是远端文件系统或键值数据库。静态资源清单决定资源 ID，`StorageContract` 给出总容量、擦除块、写对齐、单次传输上限和“写前擦除”语义。

原子操作只有 `StorageRead`、`StorageErase` 和 `StorageProgram`。读操作必须持有该资源的会话绑定共享或独占租约；擦除和写入必须持有独占租约。租约支持续期、显式释放、单调时钟到期和会话清理，任一失效都在触碰 BSP 前拒绝且不影响其他端点。所有操作同时受 32 位偏移溢出、容量、非零长度、单次最多 1024 字节、对齐以及 1～1000000 微秒超时边界约束。某个 Storage 后端失败只令本次请求返回 `ResourceFailed`，不会改变其他 Storage 资源。

确定性 Mock 的容量为 4096 字节，擦除块为 256 字节，写对齐为 4 字节，擦除态为 `0xFF`，写入只允许位从 1 变成 0。它用于软件纵切和故障隔离测试，不代表已完成 STM32 实体 Flash、EEPROM 或外部存储验证。

该接口适合配置包、校准之外的低频应用数据和小型日志快照，不得用作高频遥测通道。设备身份、序列号、ADC 校准等设备参数仍由专用双页存储负责；本资源不提供目录、文件名、磨损均衡、厂商协议或事务型文件系统。

CLI 提供 `storage-contract`、`storage-read`、`storage-erase` 和 `storage-program`。调用读写前先通过 `resource-acquire` 获取相应租约，并在完成后通过 `resource-release` 释放。

## 第二阶段：统一资源模型与固件边界

默认 Mock 板卡现在公开一个 `0x08000000` Storage 端点。它进入统一
`ResourceEnum`、`ResourceDescribe`、`ResourceStatus` 和能力位，数字孪生状态文件
同时给出 `storage.supported` 与资源 ID。Mock 的块内容仍只存在于 Storage BSP，
不会读写设备参数存储。

STM32 公共 Remote Core 提供默认关闭的 `CONFIG_REMOTEBSP_STORAGE` 骨架，包括静态
资源表、合同、Read/Erase/Program 回调和资源级故障状态。能力发布采用失败关闭：只有
资源表非空、端点合同全部合法且三个操作回调齐备时才枚举资源并置位能力；只打开
Kconfig 不会公布虚假端点。F072、F103、G431 的 CI 配置只验证这套空板级表骨架能够
交叉编译，不代表任何实体 Flash、EEPROM 或外部存储已经验收。

板级实现必须划出专用应用块区域，并证明其擦除单元与设备参数双页没有任何重叠。
在具体板卡内存布局、掉电行为和磨损策略完成验证前，正式板卡配置与 Studio 候选继续
关闭。
