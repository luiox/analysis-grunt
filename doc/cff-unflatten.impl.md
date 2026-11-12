# cff_unflatten 实现笔记

## 数据流

`ScanInput{}`（无旁证注入面——判据全为容器自证事实）→
`ScanResult{islands, rejects, dispatchers, islandsSolvable, keyProcessor,
spanUnsolvable, foreignRef, statePolluted, shapeMismatch, blocksShared,
excBlocked, classesSkipped}` → `DirtySet{uris, islands, classes, failed,
skippedByRoundTrip, entryRestored, entryTrampoline, scrambles}` →
`ProofResult{ok, failed, residual}`。三段签名与全部残留同形；Era 不消费
（v3-only 岛形自证家族，SSA 变体挂账缺口 #4 余量）。

## 算法（源锚点）

grunt3@e41e852 `FlowControlFlowFlattener.kt`（逐行对照，jasm 现场互证
boar-main.jar `net/spartanb312/obf/AD.a`）：

- 每方法 fresh state 槽 `method.locals.nextSlot`；每出口一座岛
  `emitDispatcher`（`ILOAD s; TableSwitchInsnNode(lo, hi, default,
  labels)`，default 即岛首真块）；
- 每条入边一座桥 `emitInlineStateProgram` / `StateOp.emitTo`（Add/Sub/
  Xor/Mul/Div/Math.max 闭链，DivConst 除数 `nextDivisor` 恒正）；
- Mixed 模式 `emitProcessorStateProgram`：`[INT; ISTORE; ILOAD; INT salt;
  INVOKESTATIC proc (II)I; ISTORE]`（proc = `__grt$indy$` 蹦床）；
- scramble 假桥键出窗 goto default。

## Scan（七相，单岛 fail-closed 整岛拒）

- **相 1 候选**：`[ILOAD s; TABLESWITCH]` 两条块，槽进 `bySlot` 多岛索引
  （per-frame 异常 handler 桥 = 同槽第二岛）。
- **相 2 槽 confinement**（污染连坐整槽）：IINC 直拒；每个 `ISTORE s` 从写
  点回走 span（`evalSpanTail`：Inline 闭链记账 / Processor 夹心记账放行 /
  其余越界）；`ILOAD s` 只许岛头位与 processor 夹心（前视夹心与
  `evalSpanTail` 同形判据）。**分桶按面**：DivZero span（除 0、
  `INT_MIN / -1`——守卫看累计运行值 v，IMUL/IADD 回绕可把 v 推到
  INT_MIN）→ `spanUnsolvable`（README 口径「/0」）；其余越界读写 →
  `statePolluted`。
- **相 3 岛形 + 头引用分类**：键窗连续（targets 静态编码）、default 可解；
  **alias face**：default ∈ case 目标（InvokeDispatcher 原生头缺席即拒，
  case 自环同拒）；岛头顺序落入（前驱未终结且 layoutAdjacent）→
  ForeignRef；头引用逐边归桶——`parseStateSetter` 可解 → 桥（终结 GOTO
  或条件分支；条件形/junk 前缀桥 `keep` 保留块体）、不可解且不触槽 →
  边载体候选（终结限单目标 GOTO/条件分支——switch 的 `setTarget` 改的是
  default 字段，与臂重指语义面不同，不可入载体）。
- **相 4 边载体解析**：唯一常量供给前驱传递 state（字段引用 + 顺序落入
  双路，链深封顶）——junk 门无状态，键承自上游桥/载体。
- **相 5 岛间共享块连坐**：case/default 目标跨岛 → 双岛 `blocksShared`。
- **相 6 入口桥认领**：blocks[0] 直连岛头 → 直接认领；否则解析为
  state-setter 后顺终结链追岛/载体（入口桥多岛认领冲突全拒
  `MultiEntry`）。入口真块头落异常保护区严格内含 → `entryTrampoline`
  退蹦床（range 起点即目标块头不算交叠：start 标签随 moveRange
  `kHeadFollows` 平移）。
- **相 7 异常表面 + positional 落入守卫**：handler 锚本岛可整擦桥头 →
  `handlerRebinds`；end 锚被擦块头 → 前移；start 锚被擦块 →
  `ExcBlocked` 拒。**位置边守卫**（`usersOf` 只看字段引用，落空臂/顺序
  落入是位置边）：受害者被擦后位置边改落邻接后继——前驱是本岛已归类
  条件用户且受害者是岛头 → 钉（`fallPins`，补 GOTO 指该用户 case 目标，
  与原「落空臂经岛头分发」同址）；其余位置落入整岛拒 `ForeignRef`。
  **邻接谓词 `layoutAdjacent`**：`prev.orderPos + prev.insns.size() ==
  blk.orderPos`（orderPos 是块入口 dom 序位置，`+1` 只对单指令前驱成立）。

## Rewrite（执行序；失败整方法回滚回炉）

1. 可整擦 goto 桥：前驱引用 `replaceAllUses` 直连目标块（alias-face 剪枝
   = scramble/leftover goto 全改指 default），块体入 victims。
2. 保留桥/边载体/蹦床 goto：臂字段 `setTarget` 重指真实块。
   2b. `fallPins` 逐个落钉：`createLabel(目标头)` → **先设靶再插**
   （`g.target = lbl` 后 `insertAfter`——kNoLabel 靶过不了 DOM 插入的
   句柄校验，`InvalidHandle`）。
3. 异常表：handler `bindLabel` 重绑、end 锚前移。
4. 入口块归位（`entryTrampoline`/`entryKeep` 退避不移动）：目标块未终结
   → 老后继补 GOTO（4a）；前驱位置落入 → 补 GOTO 钉住（4b）；纯形入口
   桥整块入 victims（4c）；`moveRange` 头插 + `kHeadFollows`（4d）。
   预算全部句柄在插入前定案（插入不改句柄，moveRange 只认句柄）。
5. 受害者终检：岛头残余引用的引用者必须全体在 victims 里（在批桥不算
   遗漏，引用者落批外即分类遗漏整岛回滚）→ dispatcher 入 victims。
6. `eraseBatch`（重复句柄按并集）+ `mutateMethodWithFrames` 整体重算帧。
7. 回滚路径：改写失败在干净会话重放；重放开不了会话整类放弃（半落
   会话无帧 = VerifyError 面，绝不落盘）。

## Proof

类数不增 + 脏类可重新解析（`proof_tools` 同门）+ **residual**：改写后
容器里仍「岛全形状可反」的岛数（scan 同判据 countOnly 重算）——
failed 岛原样保留即如实计入。语料单罐门要求 residual = 0；boar 全链罐
residual=41 与 `failed=41` 同源（processor 相邻面遗留，见语料账本）。

## 语料账本（单罐验收）

- **来源**：`D:/WorkSpace/Grunt/samples/boar-main.jar`（Grunteon v3 全链
  3065 类，config `samples/config-on_260620.json`；jar 不进仓）。wave2c
  正例罐 = boar-main-origin + 仅 InvokeDispatcher `chance=1.0`
  （Grunteon 3.0.0 build 260705 自产，775 类，283 分发器/1207 调用点）。
- **boar 全链罐单跑**（`grunt.CffUnflatten`，`GRUNT_TRACE=1`）：
  `scan: dispatchers=9880 islands-solvable=1522 rejects=8358
  (key-processor=6201 span-unsolvable=0 foreign-ref=285 state-polluted=0
  shape-mismatch=1872 blocks-shared=0 exc-blocked=0) skip-classes=0`；
  `rewrite: classes=653 islands=1481 scrambles=298 entry-restored=297
  entry-trampoline=259 failed=41 roundtrip-skip=0 proof=ok residual=41`。
  拒绝桶分布：processor 74.2% / 形状 22.4% / 岛外引用 3.4%，其余桶 0。
  failed=residual=41：整岛保留即如实计入 residual，proof 门 ok。
- **DispatcherRestore 联动（boar）**：组合链
  `grunt.CffUnflatten,grunt.DispatcherRestore`（--list 实证链序）上
  DispatcherRestore 仍 `dispatchers=0`（5827 desc 形候选全数形状拒）。
  归因量化（双 trace join + 独立 jar 全量头形扫描二轮实证）：5827 候选
  中**0 个**命中已解岛、**5784 个**根本没有 CFF 岛、仅 43 个有岛（23
  processor 门 / 18 形状 / 2 岛外引用）。**二轮修正**：候选实为 desc 形
  状与 `__grt$indy$` indy 蹬床及 NumberBasicEncrypt 数值走廊方法碰撞
  （anti-候选），**非 post-Redirect junk 前缀**（jar 内 `[iload N;
  tableswitch]` 头形 = 0，grunt3 源零处向方法首条前插指令，前批引述的
  `[iload_0; ldc_w; i2l; …]` 实拍样张是数值走廊方法非分发器）；罐内
  真分发器全部被 CFF 摊平成岛。**真实解锁路径 = IndyRestore 还原 proc
  体内 indy → ConstantCall 折叠纯算术 proc → 本 pass processor 桥可解
  → DispatcherRestore**。
- **解锁链落地批（processor 键解锁链，ConstantCall 扩面后实链复测）**：
  链 `IndyRestore,ConstantCall,CffUnflatten,DispatcherRestore` 上
  ConstantCall 7→**687**（413 类），本 pass `key-processor=6201→**6121**`
  （消化 80：**66 座岛转 solvable 进改写**、14 座转 foreign-ref 桶）、
  `islands-solvable=1522→1588`、改写 `islands=1481→1547`、
  failed=residual=41 不变；**DispatcherRestore 仍 0**——5827 候选中
  23 个 processor 门岛全部属 native action 面（boar 配置
  `keyProcessorNativeCandidateRatio=1.0`，19701 座 `(II)I` proc 蹬床的
  6117 个恢复目标 100% `ACC_NATIVE` 无体，算术已随 native pipeline 出
  jar；三路取证见 doc/constant-call.impl.md 语料账本），Math.max 蹬床
  面消化的 80 座岛没有一座落在分发器方法上，头锚不出场。加挂
  NumberFold/PredicateFold 变体验证同样 0——瓶颈是 native 墙，不是
  链序。DispatcherRestore 的 boar 解锁需 native action 静态语义（无
  native 库无解），如实挂账。双跑产物逐条目 CRC 全等、类数 3065 不增、
  JVM `-Xverify:all` 基线同挂零新增、origin 罐照旧零站点静默全等。
- **wave2c 正例罐**（缺口 #3 语料复用）：单跑
  `dispatchers=290 islands-solvable=0 rejects=290`（全数形状不符 =
  InvokeDispatcher 原生头反误伤面），产物逐条目 CRC 与输入全等
  （775 类不增）——全零静默实证。组合链
  `CffUnflatten,DispatcherRestore`：DispatcherRestore
  `dispatchers=283 sites=1181 rewrite 205 类 1081 站点（static=1081）
  failed=100 proof=ok residual=49`，与 ec31267 台账逐项一致——本 pass
  对 DispatcherRestore 正例面零干扰。
- **门结果**（boar + 正例罐两罐 corpus-regress，链 `CffUnflatten,
  DispatcherRestore`）：exit 0；类数 3065→3065 / 775→775；VerifyJar
  `-Xverify:all` 输入/输出失败集合逐类相同（227 基线同挂 =
  kotlin/lwjgl 缺依赖环境面，零新增）；双跑产物逐条目 CRC 全等
  （zip 时间戳外无差异）。
- **unittest**（`cff_unflatten_test.cpp`，18 案）：SCAN 10（单岛/多岛同
  槽/边载体门/异常 handler 桥/槽污染/processor 桥/**span 除零**/岛间共
  享块/分发器头反误伤/异常 start 锚）+ REWRITE 5（单岛擦除+scramble 剪
  枝/junk 门载体/双岛 handler 重绑/**无假臂门紧邻岛头落空臂钉**/拒绝面
  七桶字节逐位不动）+ PROOF 2 + JVM 实跑 1（`-Xverify:all` 真跑对照真
  值）。226/226（基线 208@ec31267 + 16 + 新补 2）。
- **修复面**（前任半成品审出，unittest 回归钉住）：IDIV 守卫看累计运行
  值（回绕把 v 推到 INT_MIN）；junk 前缀 scramble 桥保留（`keep =
  cc || span起点>0`）；switch 终结载体拒入（setTarget 只改 default 字
  段）；**layoutAdjacent 邻接谓词**（`orderPos + insns.size()`，`+1`
  只对单指令前驱成立——相 3 顺序落入 / 相 4 载体前驱 / 相 7 落空臂 /
  步 4 前驱四处同病）；**GOTO 插入前定靶**（kNoLabel 过不了 DOM 插入
  句柄校验，2b/4a/4b 三处同病——此前无测试踩到插入路径）；entryKeep
  junk 前缀入口不 moveRange；回炉失败整类放弃（半落会话不落盘）；
  DivZero 槽级分桶（spanUnsolvable 口径）。

## 已知边界

- processor 键（`__grt$indy$` proc 体）fail-closed：解锁 = NumberFold
  先行（缺口 #4 余量），本 pass 不评 proc 体。
- SSA exp 变体（LookupSwitch + per-case stub + passArgs 编组）不同构，
  挂账缺口 #4 余量；v2 Controlflow junk 分支面无 state 槽摊平，不在
  家族内。
- DispatcherRestore 的 boar 头锚缺席**非 junk 前缀面**——归因见上方
  语料账本「DispatcherRestore 联动（boar）」条的二轮修正：desc 形候选
  与 `__grt$indy$` indy 蹬床 / NumberBasicEncrypt 数值走廊方法碰撞
  （anti-候选），jar 内 `[iload N; tableswitch]` 头形 = 0；剩余瓶颈是
  native action 键墙。本 pass 不动 junk（保留形桥的 junk 前缀必须保持
  执行）；junk 摘除属 NumberFold/未来 junk 面批次。
- 分发器原生头（`default 回跳方法头`）整体不在岛家族：alias face 缺席
  拒绝即天然反误伤面，无需另设方法名判据。
