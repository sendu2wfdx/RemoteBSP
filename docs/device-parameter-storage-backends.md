# 设备参数存储后端契约

设备参数协议、Remote Core、toolbusd 和 Studio API 只依赖
`rbsp_device_param_backend`，不感知内部 Flash、外部 EEPROM 或 Mock 介质。
后端契约版本当前为 1，初始化时失败关闭并检查：

- 固定两个等大擦写域，容量必须等于 `2 × erase_size`，每域至少 512 字节；
- `program_size` 必须为 2 的幂，并能整除镜像头及最后提交标记的写入边界；
- 擦除态必须声明为 `0xFF`；介质类型必须是 internal Flash、external EEPROM 或 Mock；
- 写语义必须在“仅允许 1→0”与“字节可重写”中二选一；
- 必须支持最后写入 commit marker 的提交策略。commit marker、CRC 和代次共同决定镜像有效性。

内部 Flash 后端声明先擦后写、仅 1→0；外部 EEPROM 契约允许字节重写，但仍须实现按逻辑域
恢复到 `0xFF` 的 `erase` 操作。这里的 erase 是存储状态机接口，不限定 I²C/SPI 命令或厂商
芯片行为。驱动必须在自身内部处理页写、写周期等待、ACK polling 等介质细节，不能泄漏到协议层。

Mock EEPROM 使用同一双页镜像和最后提交算法。测试验证正常写入/重启与内部 Flash 的代次、
读取结果一致，并在编程失败后重启回退到旧完整镜像；还验证旧契约版本和互相矛盾的写能力会被
拒绝。该 Mock 只证明软件契约及断电恢复语义，不代表任何实体 EEPROM、电气接口或写周期验收。
# 写入寿命预算与坏页边界

存储层默认最多接受 10000 次已提交代数；产品可以在初始化后用
`rbsp_device_param_store_set_commit_budget` 设置更保守的非零上限，但不能低于介质
中已经恢复的 generation。预算耗尽会在擦除前失败，因此设备参数不能用于高频遥测、
运行日志或周期心跳。generation 是随快照持久化的保守写入计数；运行期 attempts、
successful commits、I/O failures 和 bad-page mask 可通过 health 接口观测。

目标页任一 erase/program/提交后校验失败都会将该页标记为本次启动期坏页，后续写入
在访问介质前失败关闭，当前有效页保持不变。普通 boot 重扫不会清除该隔离；真正重启
创建新 store 后可根据介质事实重新尝试，防止一次瞬态故障永久锁死。坏页标记目前不
持久化，也不代表厂商耐久指标或实体寿命检测结果。

