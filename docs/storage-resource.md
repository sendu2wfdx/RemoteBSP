# 通用 Storage 资源第一阶段

第一阶段把 Storage 定义为版本化、定长、可擦除的块资源，而不是远端文件系统或键值数据库。静态资源清单决定资源 ID，`StorageContract` 给出总容量、擦除块、写对齐、单次传输上限和“写前擦除”语义。

原子操作只有 `StorageRead`、`StorageErase` 和 `StorageProgram`。读操作必须持有该资源的共享或独占租约；擦除和写入必须持有独占租约。所有操作同时受 32 位偏移溢出、容量、非零长度、单次最多 1024 字节、对齐以及 1～1000000 微秒超时边界约束。某个 Storage 后端失败只令本次请求返回 `ResourceFailed`，不会改变其他 Storage 资源。

确定性 Mock 的容量为 4096 字节，擦除块为 256 字节，写对齐为 4 字节，擦除态为 `0xFF`，写入只允许位从 1 变成 0。它用于软件纵切和故障隔离测试，不代表已完成 STM32 实体 Flash、EEPROM 或外部存储验证。

该接口适合配置包、校准之外的低频应用数据和小型日志快照，不得用作高频遥测通道。设备身份、序列号、ADC 校准等设备参数仍由专用双页存储负责；本资源不提供目录、文件名、磨损均衡、厂商协议或事务型文件系统。

CLI 提供 `storage-contract`、`storage-read`、`storage-erase` 和 `storage-program`。调用读写前先通过 `resource-acquire` 获取相应租约，并在完成后通过 `resource-release` 释放。
