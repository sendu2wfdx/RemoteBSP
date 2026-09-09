# RemoteBSP 成熟度证据基线

本目录保存可由程序验证的成熟度事实，不是发布评分或营销比较。

- `remotebsp-maturity-v1.schema.json`：v1 机器契约；允许未来增加维度并在证据完备后
  进入 `ready`，但不会把当前十项基线变成永久上限。
- `remotebsp-maturity-v1.json`：截至清单日期的保守基线。
- `validate_maturity.py`：只使用 Python 标准库的严格验证器。

运行：

```sh
python3 maturity/validate_maturity.py
```

每个维度分别记录 `implemented`、`automated_test`、`cross_compiled` 和
`hardware_verified`，状态含 `absent`、`partial`、`verified` 与 `not_applicable`。
`not_applicable` 只表示该层不适用，不能折算为已验证。Mock 只能作为自动测试证据；
交叉编译说明也不能进入实体硬件证据。

v1 验证器仅在以下条件同时成立时允许把整体比较改为 `allowed=true,status=ready`：

1. 清单中没有任何未关闭 blocker；
2. 每个维度的实现与自动测试均为 `verified`；
3. 交叉编译和实体证据为 `verified` 或明确 `not_applicable`；
4. 存在仓库内可定位的 `comparative_benchmark`，记录明确 Klipper 版本、相同硬件、
   相同负载、共同指标、原始数据与复现方法。

当前清单保持 `blocked`。局部功能更丰富、Mock 通过、交叉编译成功或少量板卡实测，
都不能单独推出“RemoteBSP 整体超过 Klipper”。
