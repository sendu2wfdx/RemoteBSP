# 正式固件配置

本目录只保存可长期维护的 MCU 基础配置和名称明确的板卡/启动布局配置：

- `stm32f072rbt6_defconfig`、`stm32f103cbt6_defconfig`、`stm32g431cbu6_defconfig`：MCU 基础配置；
- 含 `mellow_fly_d5`、`weact_bluepill_plus` 或 `weact_core` 的文件：已知板卡配置；
- 含 `katapult` 的文件：同一板卡的 Bootloader Flash 布局；
- G431 的 `usb` 文件：与 CAN-FD APP 互斥的 USB Vendor Bulk APP。

临时接线、运动轴组合和压力测试配置不得加入本目录，应放在
`firmware/tests/configs/` 或构建目录。正式产品的STEP/DIR/EN、限位、TMC、GPIO、
UART、PWM和WS2812接线由RemoteBSP Studio工程保存为资源清单，默认编译进板卡
专用固件；不要为每种产品接线长期增加手写defconfig组合。

Studio 固件构建会同时生成 `.config` 与 `remotebsp_static_resources.h`。当前 C 表先
覆盖普通 GPIO 的编码引脚和启动安全模式，固件启动配置与运行时 GPIO 白名单直接
消费它；表内板型与 `.config` 不一致时编译失败。UART、运动槽、PWM、WS2812仍由
Kconfig 编译期映射提供，不存在运行时资源覆盖路径。I2C/SPI 实体端点尚未验收，
Studio 仍拒绝为它们生成实体固件，不能用数字孪生目录推断真实 AF/DMA/电气能力。
