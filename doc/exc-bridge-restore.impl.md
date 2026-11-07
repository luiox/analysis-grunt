# exc_bridge_restore 实现笔记

## 数据流

`ScanResult{sites, rejects, bridgeClasses, shapeMiss, covered, handlerCollide, xInvalid, rebindBlocked, classesSkipped}` → `DirtySet{uris, methods, bridges, pins, insns, failed}` → `ProofResult{ok, failed, residual}`。Era 不消费（v3-only 形态——v2 Controlflow 无异常桥面，形状锚自证家族）；无旁证注入面（判据全容器自证）。拒绝桶 `Reject{uri, methodIndex, anchorPos, why}`，why ∈ `shape / covered / handlerCollide / xInvalid / rebind`。

## 算法（源锚点）

生成面逐字锚来自 grunt3@e41e852
`obfuscator/process/transformers/controlflow/ControlflowJump.kt` 的
`FlowExceptionBridgeInserter`（skidfuscator 同源）：

- trap 块：`new ExceptionBridgeInternalName (=java/lang/RuntimeException)` +
  `DUP` + `INVOKESPECIAL <init> ()V` + `ATHROW`（FlowThrowJump 生成面）；
- handler 块：`POP` + `FlowGotoJump(Synthetic)` → 原边目标 X；
- 改边：`edge.to = trap`（GOTO / IFxx 臂 / switch case 臂全体改指）；
- 异常区：`{protectedBlocks={trap}, handler, catchType=ExceptionBridgeFrameValue
  (=RuntimeException), priority 追加尾}`、`maxBridgesPerMethod=4`；
- 导出面：JvmFlowExporter `emitExceptionRegions` 按连续 run 出表（单块保护区
  → 表项恰罩 trap 四条）；`emitGoto` 仅 `mode==Fallthrough && nextBlock==target`
  省略 → trap 头位置落入变体；handler 布局前驱恒显式 GOTO 守卫（位置落入
  只发生在 trap 头）。

## Scan（planMethod：scan/rewrite 共用一套判据）

单方法内先做全表锚位预解析（posStarts/posEnds/posHandlers），再两轮：

1. **形状钉**（逐表项）：catchType 一跳解析 = `java/lang/RuntimeException`；
   保护区 [start,end) 恰 4 条（end 锚 exclusive，end 哨兵 = 方法末合法）且逐字
   `NEW RTE; DUP; INVOKESPECIAL RTE.<init> ()V; ATHROW`（Methodref 三元组
   owner/name/desc 全钉）；handler 恰 2 条 `POP; GOTO|GOTO_W`。X 局部门：
   goto 靶可解析非方法末 + handler goto 无字段引用（跳入 POP 空栈，校验器
   拒）+ 非头受害者（DUP/INVOKESPECIAL/ATHROW/POP/goto）无字段引用。任一
   缺席 → `shapeMiss`（真实 try 区反误伤面在这桶）。
2. **覆盖门**：其他表项保护区 [start,end) 覆盖 trap 头或 handler 头（区间
   exclusive 端语义）→ `covered`（派发歧义，生成面自恰时为 0）。
3. **handler 碰撞门**：其他表项 handler 锚在本桥六受害者位置上任一 →
   `handlerCollide`（擦即侵占他人 handler）。
4. **X 落点门**（两轮后判，xUnion 就绪后）：X 位置 ∈ xUnion（**全量形状命中
   站点的受害者并集**——被判拒的桥原样保留，X 进保留块同样是改指错位）→
   `xInvalid`。
5. **位置落入门**：trap 头 = 方法头（入口位置边）或布局前驱非无条件终结符
   （GOTO/GOTO_W/switch/return 族/ATHROW/RET/JSR 之外）→ `needsPin`，
   钉点 = 前驱（方法头变体 = insertAtStart）。
6. **吸附安排**：eraseUnion = **已采纳站点**受害者位置并集（被判拒站点不在
   擦除面，锚不吸附）；存活表项（非本桥表项）handler 锚 ∈ eraseUnion →
   整方法拒（`rebindBlocked`）；start 锚 ∈ eraseUnion → 前移吸附（越界 =
   整方法拒——start 不能落方法末哨兵）；end 锚 ∈ eraseUnion → 前移吸附
   （越界落方法末哨兵，合法）。

## Rewrite（句柄制）

- 按类聚类站点 → 方法级 `roundTripSafe` 前置门 →
  `mutateMethodWithFrames`（promote 一次 → DOM 改写 → prepare → lower →
  Strict 帧重算正统写回）。**plan 在 mutate lambda 内做**（此刻零改动，位置
  ↔ 句柄一一对应；scan 侧 plan 只记账）——单 promote 纪律下 plan 与改写落
  同一 unit。
- `rewriteMethod` 执行序（任一步失败整方法回滚，fail-closed）：
  1. **入口一次定位**：plan 全部位置 → DOM 句柄（trap 头 / 六受害者 /
     X / 钉前驱 / 本桥三标签），复核 handler goto 靶 = X（防 plan-DOM 漂移）；
     此后插入/擦除不使句柄失效，位置账退场。
  2. **存活表项锚吸附**（先于一切删除——Boundary 标签保护锚点，
     `editor.bindLabel` 移锚；end 哨兵 = anchor kNoInsn）。
  3. **逐站**：`replaceAllUses(trapHead, X)`（全体 jump/switch 字段直连，
     旧 ControlFlow 标签随改写销毁）→ 位置落入钉（`createLabel(X)` +
     `GOTO X` 插前驱尾/方法头，插入前定靶——kNoLabel 靶过不了句柄校验）→
     本桥三标签 `destroyLabel`（表项删除后标签失主，必须先销毁否则
     eraseBatch 拒收）。
  4. **表项剔除**：本桥异常表项从 `cu->exc` 移除。
  5. **单批 `eraseBatch`**（六受害者/站，kRejectIfUsed 全有或全无）。
- 诊断面：`GRUNT_EXC_BRIDGE_TRACE=1` 打 rewrite-fail/method-fail 逐站行；
  帧链失败走 `[grunt.Frame]`（rewrite_toolbox 共享面，`GRUNT_FRAME_TRACE`）。

## Proof

类数不增 + 脏类可重解析（`proof_tools::classesIntact`，效力门挂时间片）+
residual 复扫（脏类集上重跑 planMethod，仍通过形状钉 + 互证门的站点数；
语料单罐门要求 0）。**注意 residual 口径 = 脏类集**——整类全方法失败时不
进 dirtySet，其站点由外部 census 复核（罐 A 实测：独立字节级 census 复扫
r3 bridge=0，无遗漏类）。

## 语义要点（为什么是恒等还原）

- **改道的逆**：原边 B→X 被改 B→trap，trap 无条件 ATHROW → 本桥表项（表
  尾，前表项不罩 trap 头时先匹配不到）→ POP → GOTO X。全体前驱直连 X 后，
  trap/handler 块与表项成为纯冗余，擦除不改变任何可观察行为；位置落入边由
  GOTO 钉保形（钉点 = 原落点的同址语义）。
- **X 不需要可达性证明**：X 是原边原始目标，原程序可达（census X_unreach=0
  实证）。
- **异常表唯一语义变化 = 删除本桥表项**：存活表项只动锚（吸附），区间覆盖
  的指令集在「擦除块上锚前移」语义下逐条守恒；擦除块内部 POP/GOTO 不抛
  （异常表面对它们本无派发意义）。

## 语料账本（本批实测）

- **boar 对照罐**（`exceptionBridgeChance=0.0`）：sites=0，rejects=1111 全
  shape 桶；类条目 CRC 与输入逐条全等（对照零站点 pass `grunt.DispatcherRestore`
  产物与本报文产物逐字节全等——jar 级差异仅 CLI writer 的 MANIFEST 重建 +
  12 目录桩，纯 writer 行为）。no-op 门成立。
- **罐 A**（供给链 `IndyRestore,StringPoolFold,ConstantCall` 之上）：
  scan sites=13566 / classes=726，rejects=4702（shape=4702 其余桶 0）；
  rewrite classes=726 methods=4451 bridges=13442 pins=1448 insns=80652
  failed=124 proof=ok residual=0。failed=124 站 / 31 方法全部为 Strict 帧
  重算 type conflict（`GRUNT_FRAME_TRACE`：`strict frame compute failed
  (type conflict)`，DecompilerCrasher 畸形 SMT 邻接面；DOM 改写随 lower
  已提交、帧保留 lower 形、`-Xverify:all` 全绿）。复验：
  r3 复扫 sites=0；独立 census（`mjt/tmp_corpus/census_r1.py`，校验器式
  可达普查，与本仓实现完全独立）r3 **bridge=0** / bridge_classes=0 /
  parse_fail=0；类数 3074 不增；`-Xverify:all` 失败集与输入全等
  （227 NCDFE，VerifyError=0）；双跑（r3/r4）逐条目 CRC 全等。
- **链序实证**：`…ConstantCall,ExcBridgeRestore,CffUnflatten`（r5）：
  CffUnflatten islands-solvable=1573 → rewrite islands=1543（无本 pass 链
  1281，**+262**）；rejects 收窄 foreign-ref 487→292、shape-mismatch
  3073→1866、key-processor 5013→6155（桥摘后新显形岛按 processor 桶如实
  计）；failed=30 residual=30（CffUnflatten 既有 fail-closed 面）；r5
  `-Xverify:all` 失败集与输入全等（227 NCDFE，VerifyError=0）。
- **对比基线（boar 台账）**：boar 罐异常桥 0（config chance=0.0），无对照
  正例；罐 A 为首个异常桥正例罐（`doc/corpus-ledger.md` §1）。
