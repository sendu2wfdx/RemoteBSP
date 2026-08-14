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

启用`CONFIG_REMOTEBSP_RUNTIME_CONFIG`时，构建系统会调用
`scripts/generate_factory_manifest.py`，把解析后的UART、运动槽、共享EN、TMC绑定、
PWM和定时位流默认值生成代数1的只读RBSM C数组。GPIO配置只有容量、没有静态引脚
映射，因此不会被生成器猜测；普通GPIO应由RemoteBSP Studio工程显式定义。当前
生成器仍从Kconfig兼容槽位产生出厂RBSM；后续由Studio工程直接生成完整只读RBSM
和对应`.config`。启用Flash A/B覆盖时，同一出厂RBSM也是两槽无效时的安全回退基线。
