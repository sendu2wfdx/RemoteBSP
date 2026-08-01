# 固件输出目录

构建成功后会生成以下文件：

- `remotebsp-stm32f103cbt6.elf/.hex/.bin`
- `remotebsp-stm32f103-bluepill-pb8-pb9.elf/.hex/.bin`
- `remotebsp-stm32g431cbu6.elf/.hex/.bin`
- `remotebsp-stm32f103-motion-5axis-tmc2209.elf/.hex/.bin`
- `remotebsp-stm32f103-bluepill-motion-5axis-tmc2209.elf/.hex/.bin`
- `remotebsp-stm32g431-motion-2axis-tmc2209.elf/.hex/.bin`
- `remotebsp-stm32g431-motion-5axis-tmc2209.elf/.hex/.bin`
- `katapult-stm32f103_dual.bin`
- `katapult-stm32g431_dual.bin`
- `remotebsp-*-katapult.bin`：从 `0x08002000` 运行的 APP 在线升级包
- `remotebsp-*-katapult-dual-factory.bin`：从 `0x08000000` 烧录的
  双模式 Bootloader+APP 合并包

二进制文件不纳入 Git，避免不同工具链生成的产物污染源码历史。
