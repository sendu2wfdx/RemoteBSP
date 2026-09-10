# Runtime 持久控制审计

本文定义 Runtime 写控制面的持久审计边界、同步语义和运维恢复流程。它描述的是
Linux 主机上的软件证据，不是安全认证，也不能替代实体输出、真实掉电文件系统、网络
边界或生产部署验收。

## 1. 三类记录各自负责什么

RemoteBSP 有三种容易混淆、但用途不同的记录：

| 记录 | 负责回答 | 持久性与失败策略 |
|---|---|---|
| `SecurityAuditRecord v1` | 哪个 HTTP 身份访问了哪类 REST 路径，结果类别是什么 | 默认保存在进程内有界环形缓冲，外部输出为非阻塞、尽力而为；输出失败不能破坏只读请求 |
| `ControlAuditJournal` | 哪个已认证身份何时提交了哪类控制意图，最终结果是否已证明 | 控制路径同步追加并验证完整性；关键记录不能持久化时，新的控制变更失败关闭 |
| `toolbusd` operation ledger | 某个 GPIO Write/Release operation 是否为 `pending`、`committed`、`rejected` 或 `unknown` | 是操作结果、幂等重放和作用域阻断的权威来源 |

持久控制审计不能替代 operation ledger。审计记录证明 Runtime 观察到的操作者、意图和
结果；它不能仅凭一条 `terminal` 记录重新执行或撤销设备操作。发生响应丢失、进程崩溃
或两份记录不一致时，必须以 `toolbusd` 的 operation 查询和安全恢复状态为准。

普通读取、公开存活探针和被拒绝的非控制 HTTP 请求继续进入
`SecurityAuditRecord v1`。除非调用方另外配置可靠外部采集，这些普通请求记录不具备
跨进程持久性。不能把“控制审计已持久化”表述成“所有 Runtime 请求都已可靠落盘”。

## 2. 威胁模型与完整性边界

`ControlAuditJournal(directory, key_file)` 使用显式目录和独立密钥文件。记录采用封闭、
规范化的版本化编码，以单调序号和前一条认证摘要形成 HMAC-SHA256 链。每个新段必须继承
上一段的链头，因此可检测受保护记录的内容修改、插入、删除、乱序和段间替换。启动扫描
必须先验证版本、序号、链、段关系和提交边界，验证完成前不能开放控制变更。

该机制的能力有明确上限：

- HMAC 是共享密钥完整性认证，不是数字签名，也不提供不可否认性；
- 能读取密钥的主体可以生成有效记录，控制服务账号或同机 `root` 的攻击者不在此机制的
  防伪边界内；
- 如果没有外部保存的可信链头、单调计数器或远端归档，整套日志和密钥一起回滚到旧的
  有效快照可能无法由本机扫描发现；
- 记录内容不是加密存储。目录访问权限承担机密性边界，因此记录只保存完成追溯所需的
  稳定身份、请求 ID、操作类别、受控目标标识、operation ID 和结果，不保存 API 明文
  密钥、认证头、幂等键、原始 URL、请求体或响应体；
- 墙上时间只用于运维排序，不是可信时间戳、MCU 时间或硬件同步精度证据；
- HMAC 链只能证明日志内部一致性，不能证明物理 GPIO 已达到某个电平，也不能证明远端
  MCU 固件未被替换。

若部署要求抵抗主机管理员、服务密钥泄露或整库回滚，应把已验证链头周期性提交到独立
受控系统，并使用硬件密钥、签名、可信时间或 WORM 存储补充。当前本地日志不能替代这些
部署措施。

## 3. 记录状态机

控制审计使用三种追加动作。下列接口名表达稳定语义；参数的具体封闭结构以实现和测试为
准，不应由插件或网页自行拼接日志记录。

### 3.1 `append_intent(...)`

在任何可能产生远端副作用的下游调用之前，同步追加 `intent`。记录至少关联：

- 审计 schema 与记录类别；
- 随机 `request_id` 和认证得到的 `key_id`；
- 控制动作类别，例如租约登记、本人释放、监督撤销或 GPIO 写入；
- 由 HMAC 请求摘要关联的稳定目标范围，以及可用时的
  `lease_id`、`operation_id`；
- journal 序号和墙上时间表示。

只有 `intent` 已完整写入并完成要求的同步后，Runtime 才能进入下游 mutation。预写失败
表示“本次调用尚未获准越过审计边界”，必须直接失败，不能调用 `toolbusd`，也不能通过
异步补记来绕过。

### 3.2 `append_terminal(...)`

当下游给出可证明的确定结果后，追加与原 `intent` 关联的终态。终态必须区分成功提交和
确定拒绝，不能用一个含糊的 `success=false` 同时表示“未执行”和“结果未知”。只有终态
完整同步后，HTTP 才能把经过审计的成功返回给客户端。

如果下游已经产生 operation，终态应携带规范 `operation_id`，以便审计记录与权威账本
交叉查询。`append_terminal` 不得改写原记录；后续恢复结论以新记录追加。

### 3.3 `append_unknown(...)`

下游调用已经开始，但 Runtime 无法证明最终结果，或者重启扫描发现只有 durable intent
而没有可验证终态时，追加 `unknown`。它表示必须协调，不表示失败，更不表示可以直接
重发。可查询的 GPIO Write/Release 应继续使用原 operation ID 查询。`unknown` 是该 intent
在审计日志中的最终记录；后续查询不会回写或篡改它。若需要执行恢复动作，恢复动作必须
以新的 HTTP request 和新的 intent 留痕，并关联原 operation ID。

租约登记并不天然具有与 GPIO operation 相同的跨进程结果账本。登记阶段出现未知结果时，
只能依赖 daemon 世代绑定、有限 TTL、显式释放和部署恢复流程，不能套用 GPIO 的
exactly-once 或作用域自动解冻承诺。

允许的逻辑迁移为：

```text
intent -> terminal(committed | rejected | released | failed)
intent -> unknown
```

同一 intent 只能追加一个 terminal 或 unknown。恢复动作必须建立新 intent 并引用原
operation，不得删除、覆盖或伪装成第一次就成功。

## 4. 同步点与失败关闭

| 失败位置 | 下游是否允许开始 | 对外语义 |
|---|---|---|
| 启动校验、密钥或目录校验失败 | 否 | 保持读取/健康能力可用时只关闭 mutation；不得以空日志继续 |
| `append_intent` 写入或同步失败 | 否 | 确定未越过审计边界，返回稳定的后端不可用错误 |
| 下游在调用前给出本地确定拒绝 | 否 | 追加 rejected 终态后返回原拒绝；终态无法同步则报告审计不可用 |
| 下游已开始且结果未知 | 已开始 | 尽力追加 unknown，固定为不可直接重试，并引导查询原 operation |
| 下游已确定完成，但 terminal 同步失败 | 已开始 | 不得返回“已审计成功”，不得删除 operation 结果或盲目回滚；保留原 operation ID 并进入协调路径 |
| 运行中发现链、序号或段损坏 | 新请求不得开始 | 关闭后续控制变更，保留现场供恢复；只读路径不得伪报控制可用 |

`possibly_committed` 必须来自下游是否可能已经越过副作用边界，而不是由审计文件是否写成
来猜测。terminal 记录失败不能把一个已提交 operation 改成 rejected，也不能擦除
`toolbusd` 已持久化的 unknown/scope-blocked 状态。反过来，完整的审计 terminal 也不能
把 operation ledger 的 unknown 强行提升为 committed。

若 unknown 自身因磁盘错误无法持久化，服务仍必须在内存健康状态中保持控制不可用并向
运维报告原 request/operation 关联；不能因为“错误记录也写不进去”而继续接受新写入。

## 5. 文件、权限和进程锁

生产部署应让 Runtime 使用专用非 `root` 服务账号，并满足以下边界：

- journal 目录由服务账号拥有，只允许该账号访问；密钥文件为受控普通文件且只允许该
  账号读取；
- 目录、密钥、段文件、manifest 和锁文件不得来自符号链接。实现若要求普通文件 link
  count、所有者或 POSIX mode 的精确值，部署必须按该要求创建，不能依赖服务启动后修复
  一个原本不可信的路径；
- 密钥只通过 `key_file` 路径加载，不放入命令行明文、环境日志、Studio 工程或 Git；
- journal 与 operation ledger 使用不同目录和不同格式，不能互相覆盖；
- 单实例进程锁在整个 journal 生命周期内持有。第二个 Runtime 不能同时写同一目录，
  也不能通过复制锁文件规避检查；
- 锁只协调遵循协议的进程，不能阻止特权进程直接修改文件；
- 临时文件、段切换和 manifest 更新必须在同一受控目录完成，并按实现要求同步文件与
  目录，避免只同步数据却丢失目录项。

首次部署应由密钥管理流程生成高熵随机密钥，在 Runtime 未运行时完成目录和文件权限设置，
再把路径交给服务管理器。已有 journal 绝不能用新随机密钥“试着打开”；错误密钥应失败
关闭。现提供 `python3 -m runtime_api.control_audit_admin` 离线管理入口：

- `anchor-export` 先完整打开并验证 journal，再以 `O_EXCL` 导出含链头、记录数、活动段、
  密钥标识和 HMAC 的 JSON。`exported_at_ms_untrusted` 明确来自不可信主机时钟；把该文件
  复制到独立系统才构成“外部保存”，本工具不会虚构远端服务、可信时间或 WORM；
- `anchor-verify` 要求显式提供 1～16 把受信密钥，按 `key_id` 选择并复验 HMAC。未知、
  重复、越界或错误密钥全部失败关闭；
- `key-rotate` 只允许停机执行。它先用旧密钥验证完整旧 journal，确认没有未决 intent，
  再把旧目录原样封存为独立世代，并用新密钥创建新的空活动世代；旧段不重写、不用新密钥
  重签。交接收据同时由新旧 HMAC 认证；
- `rotation-verify` 必须同时持有并信任新旧密钥，才能验证交接收据。最多接受 16 把密钥，
  避免无界扫描或含糊的“尝试所有密钥”。

例如：

```bash
python3 -m runtime_api.control_audit_admin anchor-export \
  --journal-dir /var/lib/remotebsp/control-audit \
  --key-file /run/credentials/control-audit.key \
  --output /secure-transfer/control-head.json --label shift-a
python3 -m runtime_api.control_audit_admin key-rotate \
  --journal-dir /var/lib/remotebsp/control-audit \
  --current-key-file /run/credentials/control-audit.key \
  --new-key-file /run/credentials/control-audit-next.key \
  --archive-root /var/lib/remotebsp/control-audit-archive
```

轮换后服务管理器必须明确改用新密钥路径。仅原地替换密钥文件、在运行中热加载或删除旧
密钥都不属于保证。旧密钥需按保留策略受控保存，才能验证对应旧世代；交接收据证明的是
两把共享密钥对同一边界达成一致，不是数字签名或不可否认性。

## 6. 有界容量、分段和保留

所有段大小、段数量、单条记录长度、恢复扫描工作量和活动关联数量都必须有硬上限。日志
达到容量上限、无法创建新段或磁盘空间不足时，新的 mutation 应在 intent 之前失败关闭，
不能静默覆盖仍处于保留期或尚未解决的记录。

分段轮转必须延续 HMAC 链，完整同步新段和目录元数据后才能切换活动段。删除旧段属于运维
和保留策略动作，而不是普通请求的副作用；删除前应确认不存在未解决 intent，并把导出
范围、首尾序号、链头、文件哈希、时间和操作者写入独立归档记录。若实现支持自动压缩或
回收，也必须保留可验证的连续检查点，不能把断链解释为正常轮转。

本地保留是有界的，不等于无限历史、异地备份、集中审计或合规留存。生产容量应按峰值
控制请求率、保留周期和故障恢复时间预留，并监控剩余容量、活动段、最后同步序号、未解决
intent、验证失败和写入失败。接近上限时应先扩容或受控归档，不能等到控制入口被迫关闭。

## 7. 启动与人工恢复

### 7.1 正常启动

1. 在打开 HTTP 控制入口前，以指定密钥扫描 manifest 和全部保留段。
2. 校验文件类型、所有者、权限、链接、版本、长度、序号、段关系和 HMAC 链。
3. 识别没有 terminal/unknown 的 durable intent；在能够追加时把它恢复为 unknown。
4. 建立未解决事件索引，但不据此伪造 operation 结果。
5. 只有 journal 可写、同步能力正常且没有阻止继续的损坏时，才报告控制审计可运行。

只读 Runtime 可以在控制审计未配置或不可用时运行，但认证控制租约和 GPIO mutation 必须
保持关闭。能力端点应区分“已配置”和“当前可运行”，不能因为 HTTP 进程仍存活就报告控制
面健康。

### 7.2 损坏、错误密钥或磁盘故障

1. 停止 Runtime 控制服务，禁止自动删除、截断或创建空 journal。
2. 保留原目录、密钥文件、权限元数据和服务日志的只读副本；计算文件哈希并记录取得方式。
3. 检查是否只是目录权限、磁盘只读、空间耗尽、锁被存活进程占用或误用了密钥。不要修改
   原件来试错。
4. 对每个未解决 intent，使用 request ID、operation ID 和稳定 scope 查询 operation
   ledger。`unknown`/`expired_unknown` 继续按作用域阻断和人工协调处理，不能重发原动作。
5. 从可信备份恢复时，同时验证备份链头、密钥世代和备份后的 operation 变化。仅恢复
   audit 而回退 operation ledger，或反过来，都会产生新的证据断层。
6. 无法保留连续链时，应把旧目录封存为一个明确结束的世代，在独立运维记录中说明原因、
   首尾摘要和审批，再初始化新世代。不得把新空目录描述成原链已修复。
7. 先以只读方式启动并验证健康状态，再由获准运维人员开放 mutation。恢复动作本身应写入
   新世代审计或独立不可变运维记录。

错误密钥、链中段损坏、记录矛盾、manifest 不一致或已有实例持锁均应失败关闭。只有实现
明确允许的尾部未完成写入，才可以回退到上一条完整提交记录；这种回退仍要把最后一个
durable intent 视为 unknown，不能当作从未发生。

## 8. 验证证据层级

持久控制审计至少应覆盖以下纯软件测试：

- 规范编码、HMAC 链、单调序号、跨段链和确定性重放；
- 重启、合法尾部截断、中段篡改、记录删除/乱序、manifest 不一致和错误密钥；
- 不安全目录或文件 mode、错误所有者、符号链接、异常硬链接和双进程锁；
- intent/terminal/unknown 的状态迁移、并发请求线性化和未解决 intent 恢复；
- intent 同步失败确保下游零调用；下游开始后的 terminal 同步失败确保不盲目重试或
  覆盖 operation ledger；
- 磁盘满、容量耗尽、短写、`EIO`、文件同步和目录同步失败；
- 停机密钥轮换、旧世代原样复验、新旧双 HMAC 交接、错误/重复/超过16把可信密钥拒绝；
- 链头独占导出、篡改检测，以及不可信主机时间的明确标注；
- 记录字段封闭、长度上限及 API 密钥、认证头、幂等键、请求体不落盘；
- 真实 Runtime、`remote-cli`、`toolbusd` 与 USB Mock/vcan 的进程级控制闭环。

其中真实文件系统边界由
`python3 -m unittest runtime_api.tests.test_control_audit_filesystem_drill`
自动演练。它覆盖未承诺尾部裁剪、已承诺截断、链中段损坏、分段轮转、独立进程锁、
轮转创建失败后的 writer 毒化，以及锁释放后的重启恢复；详细判定见
[Runtime 持久化恢复软件演练](runtime-persistence-recovery-drill.md)。

这些证据必须按层级表述：

- Python/C++ 单元测试和故障注入属于主机软件证据；
- USB Mock、Mock MCU 和 vcan 进程测试属于 Mock/自动测试证据；
- STM32 交叉编译不适用于 Runtime journal，也不能转化为硬件证据；
- 临时文件系统上的崩溃与同步故障注入不等于生产存储介质真实断电测试；
- 只有带环境、文件系统、挂载参数、内核、硬件、原始日志和判定阈值的实体演练，才能作为
  对应部署或硬件证据。

测试通过可以证明已覆盖的同步和恢复状态机，不能据此宣称公网安全、抵抗主机管理员、
符合某项审计法规，或整体成熟度超过 Klipper。

## 9. 非目标与仍需关闭的风险

本地持久控制审计不解决：

- TLS、反向代理信任、网络客户端限速和公网暴露安全；
- API 密钥热撤销、完整轮换、用户目录、动态角色或多租户隔离；
- CAN/CAN-FD/USB 总线消息认证、加密、防伪造或跨重启防重放；
- operation ledger 保留期之外的 exactly-once，或对 `expired_unknown` 的自动重放；
- 跨重启 Runtime 事件历史与可恢复增量遥测；认证 SSE 完整状态推送、慢客户端隔离和
  轮询降级已实现，但不提供跨重启续传保证；
- 集中日志、自动异地备份、数字签名、可信时间、WORM 和不可否认性；当前只提供可复制到
  外部系统的本地链头证据及离线复验，不声称已接入独立可信锚定服务；
- 实体掉电、物理 GPIO 电平、MCU 固件可信、总线洪泛、卡死资源或最坏安全停机时延。

因此，本功能完成后可以收窄 `runtime_control_plane` 和 `auth_and_threat_model` 中的
“持久审计完整性”缺口，但不能单独关闭任一成熟度 blocker，也不能把
`security_boundaries` 提升为 `verified`。
