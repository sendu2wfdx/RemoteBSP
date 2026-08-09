# RemoteBSP Studio

这是本地运行的第一版图形工具，包含：

- 板卡与运动轴引脚配置；引脚按 GPIOA/GPIOB/GPIOC 分组，固定占用和重复选择会被隐藏。
- 每个轴可使用独立 EN，或显式复用任意前置轴的 EN；共享组自动继承引脚和有效极性。
- 每个轴可独立设置 DIR 正常或反相，临时改变机械正方向不需要调整接线。
- 数字 IO 可配置逻辑名称、引脚、输入/输出、内部上下拉、有效电平、输出故障
  安全电平和输入消抖；已知板级接口会锁定原理图决定的属性。
- Mock MCU 的 GPIO 电平、步进位置、队列与故障状态可视化；每个轴以独立步进
  电机图显示，旋转箭头按当前位置更新。
- PWM 与 WS2812 使用两个独立资源区，可分别添加、删除多个实例。PWM 配置只保存
  逻辑名称、预设硬件端点、引脚、固定频率、默认占空比和有效极性。端点绑定
  定时器通道与候选引脚；同一定时器组的PWM必须使用相同频率。
- WS2812 配置保存通用定时位流通道、引脚、灯珠数量、色序和复位时间；GUI 只表达配置草案，
  RGB→GRB 与真实波形由 Linux API 和 MCU 定时位流后端分别负责。
- 独立“实时控制”页集中放置 GPO 电平、PWM 启停/实时占空比和灯带颜色操作；
  当前仅修改本地预览，后续通过 toolbusd IPC 连接真实对象。
- 每块板预置四组TIM3 PWM端点和三组TIM1+DMA灯带端点。当前公共固件只实现
  第一组，其余组合在界面中明确显示“后端待验证”，并阻止无效清单通过资源检查；
  随固件后端完成后可直接把状态切换为“已实现”。
- 资源清单 JSON 导出，包含 `motion.axes`、`gpio.resources`、`pwm.resources` 与
  `timed_bitstream.resources`；当前仅用于预览，
  写入 EEPROM/Flash 的事务流程仍在待办中。

直接查看演示界面：

```bash
cd /mnt/d/Documents/RemoteBSP
python3 gui/server.py
```

浏览器打开 `http://127.0.0.1:8765`。

连接正在运行的 Mock MCU：

```bash
./build-wsl/mock_mcu/mock_mcu vcan0 classical \
  --visual-state /tmp/remotebsp-mock-state.json

python3 gui/server.py --state /tmp/remotebsp-mock-state.json
```

状态文件只由 Mock MCU 写入，GUI 只读，因此关闭页面不会影响总线或远端节点。
数字孪生会只读显示 PWM 频率、占空比和运行状态，并把符合 WS2812 时序的 Mock 位流
解码为逻辑像素。正式控制通路后续只会通过 toolbusd 本地 IPC 接入，不会让 GUI
直接访问 CAN。
