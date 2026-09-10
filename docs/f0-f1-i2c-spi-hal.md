# STM32F072/F103 单板 I2C/SPI HAL

F072 与 F103 现已接入和 G431 相同的原子总线事务契约，但正式配置仍默认关闭。
仅测试配置启用以下固定端点：

- I2C1：PB6=SCL、PB7=SDA，100 kHz，静态 7 位设备地址默认为 `0x48`；
- SPI1：PA5=SCK、PA6=MISO、PA7=MOSI、PA4=CS，Mode 0、MSB first；F072
  时钟为 6 MHz，F103 为 9 MHz。

事务超时是整个请求的统一预算，不会逐字节重新计时。I2C 组合读支持 repeated
start；只有请求显式授权恢复时，失败后端才执行九个 SCL 脉冲、STOP 和重新初始化。
SPI 在配置输出前先把 CS 写为高电平，事务结束和任何错误路径都会释放 CS；错误后
执行 abort/deinit/reinit，重配失败会锁存为后端故障，后续请求不会继续访问故障外设。

测试配置位于 `firmware/tests/configs/stm32f072_bus_hal_defconfig` 与
`stm32f103_bus_hal_defconfig`。它们只用于交叉编译和软件验证，不代表实体板卡已经通过
电气、时序或总线兼容性测试，也不应直接作为量产固件烧录。

关闭 `REMOTEBSP_BUS` 后，板级 BUS 源码以及 I2C/SPI HAL 驱动不会加入目标，保持
Flash、RAM 与中断资源可裁剪。生产代码的通用超时、恢复、错误锁存和 CS 路径由主机
HAL 桩测试覆盖；两颗 MCU 的寄存器复用差异由各自交叉编译覆盖。
