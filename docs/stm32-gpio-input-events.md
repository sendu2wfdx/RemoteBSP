# STM32 GPIO 输入事件后端

STM32F072RBT6、STM32F103CBT6、STM32G431CBU6 当前共用 Remote Core 的周期采样
事件引擎。板级后端只提供静态引脚准入、输入配置、GPIO 读取和单调时间；协议解析、
去抖、排队与发送全部位于主循环，不在中断中执行。

## 已落实边界

- GPIO 必须通过 Kconfig 生成的静态映射和 `gpio_resource_allowed` 校验；运行期间只创建
  绑定静态端点的会话对象，不改变引脚复用，也不动态借用其他 IO。
- 订阅只允许当前会话拥有的输入对象。边沿掩码、去抖微秒数和队列容量在入口校验；每对象
  队列受 `CONFIG_GPIO_INPUT_EVENT_QUEUE_CAPACITY` 硬上限约束。
- 候选电平连续保持到去抖期限后才更新稳定值；各 GPIO 的候选状态、序号、队列和丢弃计数
  彼此独立。队列满时丢弃最新事件并饱和累计计数，不阻塞其他资源。
- 链路暂不可发送时保留队首等待后续轮询。事件携带单调微秒时间、稳定边沿、对象序号和
  累计丢弃数；这不是逻辑分析仪或跨板同步测量证据。
- 输入事件状态 v2 在板级诊断回调存在时额外公开 `mailbox_dropped`、
  `hints_matched` 和 `hints_ignored`。计数均饱和、不回绕；旧固件的 v1 状态明确报告 EXTI
  诊断不可用，主机不得用零填充。`dropped_events` 仍只表示该 GPIO 的公共事件队列溢出，
  不与 ISR mailbox 丢弃数混为一谈。

## EXTI 与验证边界

正式后端默认采用周期采样。F103 的 PA0/EXTI0 与 G431 的 PC13/EXTI13 可由 Studio 输入
GPIO 显式选择；F072 没有公布 EXTI 端点。ISR 只清 pending 并向固定 mailbox 投递静态
引脚，主循环消费 hint 后仍由公共引擎完成采样、去抖、会话检查、排队和发送。

板卡能力目录 v3 已先加入独立 `exti.endpoints` 合同。端点必须引用允许输入的 GPIO，
`line` 必须等于引脚号，同一板卡每条 EXTI line 只能出现一次，并拒绝板级保留/AF 占用
引脚。两个端点已标记 `implemented` 并分别绑定独立 Kconfig 符号，但仍为
`enabled=false`；旧工程和未选择端点的工程不编译 mailbox、不配置 NVIC。输入 GPIO
切换引脚、改为输出或清除“采样方式”时，Studio 会清除不再合法的绑定；固件关闭对象或
清理会话前必须先停用 EXTI，失败时保留对象和所有权。

周期采样最小可检测脉宽受主循环最坏轮询间隔、临界区和去抖共同限制。主机桩测试覆盖稳定
边沿、队列溢出、丢弃计数和资源隔离；三板交叉编译只证明软件构建闭环，不能宣称实体最小
脉宽、IRQ 时延或抖动指标。

```bash
ctest --test-dir build -R '^embedded_core_tests$' --output-on-failure
cmake --build firmware/build-f072 -j32
cmake --build firmware/build-f103 -j32
cmake --build firmware/build-g431 -j32
```
