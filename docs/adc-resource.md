# ADC 通用资源

ADC 第二阶段提供静态声明端点的原始码单次或有界批量采样。它不解释传感器、
电压或温度，不允许运行时改变引脚和 ADC 复用；这些信息仍由板卡专用 Kconfig
生成的资源目录决定。

协议版本为 1，包含 `AdcContract` 和 `AdcSample` 两个命令。合同公布分辨率、
参考电压、最大采样率和最大批量数。请求携带 ADC 资源 ID、总超时、样本间隔和
样本数，结果携带资源 ID、单资源递增序号、实际耗时和原始无符号码。

边界固定为每批 1～32 个样本，总超时不超过 1 秒。批量采样的声明时长必须落在
超时内，并且不得超过端点最大采样率。访问沿用资源合同及共享读租约；一个端点
参数错误或 BSP 失败只返回该请求错误，不改变其他 ADC 端点状态。

当前 Mock 清单把两路 ADC 纳入统一 `ResourceEnum`、`ResourceDescribe`、
`ResourceStatus`、能力合同和数字孪生可视化状态；Mock BSP 使用可重复的 12 位
序列验证协议、Remote Core、客户端和 CLI：

```text
remote-cli --node 1 adc-contract 0x05000001
remote-cli --node 1 adc-sample 0x05000001 8 100 2000
```

STM32 公共 Remote Core 已提供条件编译的 ADC 资源表、合同、采样回调、速率与
超时校验、单端点序号和失败锁存。F072、F103、G431 均有开启核心但不注册端点的
交叉编译门禁；没有板级后端时能力位和资源目录不会虚假公布 ADC。

以上仍只证明软件纵切和三类 MCU 的编译兼容，不代表 STM32 实体 ADC 已采样。
实体实现必须在 BSP 层接入经过 AF、参考电压和校准确认的静态通道；在确认前
Studio 不开放 ADC 候选，具体传感器业务也不能下沉到 MCU。

## 生产校准数据

生产校准工具只生成和核对数据，不读取 ADC，也不宣称板卡已经完成校准。新工件使用
`REMOTEBSP_ADC_CALIBRATION_V1`，其设备参数值为固定 64 字节小端格式：`ADCC`
魔数、格式版本、模式、通道、ADC 位宽、资源 ID、量程、参考电压、定点增益/整数
微伏偏移或 2～4 个严格递增校准点，以及覆盖前 60 字节的 CRC32。参数 ID 固定为
`0x1000 + channel`；工件 SHA 防止在生成后替换通道或资源，但仅凭工件不能证明该资源
就是目标板上的实体端点。

工件 JSON 使用规范化 SHA-256 绑定上述规格和编码值。写入前预检还必须读取现有设备
参数备份，验证 v2 备份 SHA-256，确认目标 UUID、generation、参数类型和长度兼容，
从未写入的 ADC 参数以 `type=5`、零字节 `ABSENT` 表示，预检明确允许该状态完成首次
写入；其他非 12/64 字节旧值仍拒绝。预检可通过 `--resource-evidence` 接收已经由上游
核验、绑定同一节点 UUID 的 `REMOTEBSP_ADC_RESOURCE_EVIDENCE_V1`，并逐项核对资源 ID、
通道、分辨率和参考电压。未提供时结果明确标记 `artifact_only`，不得作为实体合同证据。
通过后才输出可交给既有 `device-parameter-write` 的 Base64 值。真正写入仍要求显式
`WRITE_DEVICE_PARAMETERS`、运行节点 UUID、CAS generation 和 HMAC 审计。

```text
studio_cli.py adc-calibration-create --specification adc-spec.json --output adc-cal.json
studio_cli.py adc-calibration-validate --calibration adc-cal.json
studio_cli.py adc-calibration-preflight --calibration adc-cal.json --backup parameters.json
studio_cli.py adc-calibration-preflight --calibration adc-cal.json --backup parameters.json --resource-evidence adc-resource.json
```

旧版 12 字节增益/偏移/参考电压值继续允许读取、备份和恢复；新工具不会生成旧格式。
未知版本、额外字段、浮点数、越界量程、乱序点、CRC/SHA 不匹配或不完整备份均失败关闭。
