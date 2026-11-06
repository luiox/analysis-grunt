# dispatcher_restore 实现笔记

## 数据流

`ScanInput{}`（无旁证注入面——判据全容器自证）→
`ScanResult{sites, rejects, dispatchers, dispatcherRejected, keyNotConst,
keyNotInTable, padMismatch, dispatcherBlocked, classesSkipped}` →
`DirtySet{uris, sites, classes, failed, skippedByRoundTrip, rewrittenStatic,
rewrittenVirtual}` → `ProofResult{ok, failed, residual}`。Era 不消费
（v3-only 形态，grunt2 无此 transformer，形态锚自证家族）。

## 算法（源锚点）

生成面逐字锚来自 grunt3@e41e852
`obfuscator/process/transformers/redirect/InvokeDispatcher.kt`
`buildStageImpl`：

- 分发器头：`LABEL(startLabel); ILOAD(maxLoadOp);
  TableSwitchInsnNode(startCase, endCase, startLabel.node, labels...)`——
  default 即方法头；desc
  `"(" + repeat(maxLoadOp) { "Ljava/lang/Object;" } + "I)$returnDesc"`。
- case 体：`VarInsnNode(ALOAD, stack)` + `CHECKCAST` 逐参 + 唯一
  `INVOKESTATIC|INVOKEVIRTUAL` + `+InsnNode(returnType)`；virtual 分支的
  实参从槽 0 起连载（无显式 receiver load——源码 quirk，case 门两侧都不
  依赖它）。
- 调用点：原调用指令 `handle.callHandle.owner = classNode.name;
  .name = methodNode.name; .desc = methodNode.desc`（opcode 不变）+
  `addNulls`（= maxLoadOp - 实参数）个 `ACONST_NULL` + key
  `accessKey.toInsnNode()`。
- 分组面：每组 ≥2 调用点、`MaxHandles=10`/`MaxParams=10`、primitive 参调
  用点跳过——desc 形 `(Object×N; I)R` 与键表 case 数 ≥2 作为形状门
  （`knowledge::kGruntDispatcherMinCases`）。

## Scan

- **分发器识别**（`buildDispatcher`，候选预筛 static + desc 形）：
  1. **头锚**：首条 ILOAD 恰为 key 槽 N（`intLoadSlotOf`）+ 第二条
     TABLESWITCH + `resolveLabel(default)` 回跳方法首条指令 + 方法无异常
     表；任一缺席 → 整表拒绝（`dispatcherRejected`，`分发器形状不符：`前
     缀记账）。
  2. **键表**：`lo = switch.a`，case 数 = `targets.size()`（连续区间，下
     标 = key - lo）；case 数 < 2 拒。
  3. **逐 case 直线转发体**：从 case 目标起直线走——允许 ALOAD 家族 /
     CHECKCAST / NOP，恰一个 INVOKESTATIC|INVOKEVIRTUAL（多候选 →
     `Ambiguous`，成员引用不可解析 → 拒），收尾必须 ret 型 return；中段
     含标签（合并点）→ 拒；转发目标返回类型与键表 ret 恒等互证。
- **调用点反查**（相 2，分发器方法自身跳过）：`INVOKESTATIC owner=self +
  desc 形` 命中已识别键表 →
  1. **key 位**：调用点回走 NOP 跳过（squeeze 遗留口径），途中标签 =
     合并点 fail-closed；key 非常量（含 NumberBasicEncrypt 藏形）→
     `keyNotConst`；不在键表区间 → `keyNotInTable`。
  2. **null 跑互证**：key 前连续 ACONST_NULL 计数（cap = N+1 封顶），
     `padCount = N - 目标消费`（virtual = 实参槽 + receiver）；null 跑
     ∉ [padCount, N] → `padMismatch`（调用点 null 跑长于补位数 = 原实参
     尾带 null 字面量，合法——Site 记 `padCount`，擦末段留前段）。
- **promote 纪律**：每方法只 promote 一次（`promoteMethod` 重复调用幂等
  但**重建 CodeUnit**，旧 Insn* 全作废）——scanClass 预循环单点持有，
  `buildDispatcher` 复用调用方视图绝不自行 promote（boar 正例罐实证：
  双 promote 后相 2 用旧视图 UAF → SEH 硬崩）。

## Rewrite

- 按类聚类站点，方法级 `roundTripSafe` 前置门 →
  `mutateMethodWithFrames`（promote 一次 → DOM 改写 → prepare → lower →
  Strict 帧重算正统写回）。
- 单站点（`rewriteSite`）：srcPc + INVOKESTATIC 重定位 → 形状复核（身份/
  key/null 跑重钉，scan 之后字节可能已被同方法前站改写，不盲改）→ 调用
  指令**原地换**真目标（srcPc 保留，opcode 换 case 转发形，成员 intern
  成真目标）→ 跨度 [NOP 夹层; 末段 padCount 个补位; key]
  `eraseBatch` 擦除（全有或全无；kRejectIfUsed 拒收即整体还原放弃）。
- 失败步有 `rewrite-fail` 逐站诊断（`GRUNT_DISPATCHER_RESTORE_TRACE` 门
  控）；帧链失败有 `[grunt.Frame]` 诊断（`GRUNT_FRAME_TRACE` 门控，
  rewrite_toolbox 共享面）。

## Proof

类数不增 + 脏类可重解析（`proof_tools::classesIntact`）+ residual 统计
（countOnly 复扫：仍「分发器识别通过 + 调用点全形状命中键表」的站点数；
语料单罐门要求 0）。

## 语料账本

- **boar-main.jar（3065 类，Grunteon 全链产物）——本罐不覆盖**：单跑
  `grunt.DispatcherRestore` dispatchers=0、sites=0。取证：(a) config 顺
  序——`config-on_260620.json` InvokeDispatcher 排 index 10，
  ControlflowFlattening/ControlflowJump/ReferenceObfuscate 在 17/18/24，
  Grunteon 按 config 顺序执行（`Grunteon.create` 直 map
  `config.transformers`；orderRules 只是事后断言）；(b) 容器侧——5827 个
  desc 形 static 候选全数形状拒绝（4425 头锚第二条非 TABLESWITCH +
  1402 首条非 key 槽 ILOAD）。**归因修正**（cff_unflatten 收口批实证，
  修正本条旧记「CFF 摊平所致」）：与 `grunt.CffUnflatten` 解岛台账
  join 后 5827 候选中 **0 个**命中已解岛、**5784 个**根本没有 CFF 岛
  （仅 43 个有岛：23 processor 门 / 18 形状 / 2 岛外引用）——头锚缺席
  由 post-Redirect junk 前缀所致（头锚实拍 `[iload_0; ldc_w; i2l; …]`
  数值垃圾前缀，上游 junk/数值藏形在 Redirect 之后运行）；TrampolineSqueeze
  单跑（4729 处）不改变该拒绝分布，`CffUnflatten,DispatcherRestore`
  组合链同样 0 分发器。会命中的 config 产物：InvokeDispatcher 排在
  Controlflow 之后（或 CFF 关闭）且调用点未被 ReferenceObfuscate indy 化
  （或已经 IndyRestore 还原）；解锁面属 junk 前缀/processor 族
  （缺口 #4 余量），不在 CffUnflatten 语义面内。
- **正例罐**（boar-main-origin.jar + 仅 InvokeDispatcher `chance=1.0`，
  Grunteon 3.0.0 build 260705 自产，775 类）：识别 283 分发器 / 52 形状
  拒；站点 1181（keyNotConst=0 keyNotInTable=0 padMismatch=0）；rewrite
  205 类 1081 站点回填（static=1081，virtual=0，failed=100）；
  proof=ok，residual=49。failed/residual 归因：宿主方法 Strict 帧重算
  type conflict——**无编辑帧链探针复现同一冲突**（原方法既有形态，与改
  写无关；`roundTripSafe` 只测 promote→lower 不测 rewire，查不出该形
  态），fail-closed 保留不手剪 SMT。
- **组合链**（正例罐，`grunt.StringPoolFold,grunt.IndyRestore,
  grunt.DispatcherRestore` 与 `grunt.IndyRestore,grunt.DispatcherRestore`
  两链）：corpus-regress 绿，JVM `-Xverify:all` 基线同挂零新增，类数
  775 → 775。

## unittest

`test/unittest/dispatcher_restore_test.cpp`：jasm 夹具真值往返（真键表 →
真目标映射，判据不读方法名）13 案——SCAN（static/virtual/N=0 三形态、
null 字面量实参站点、key 不在表、key 非常量、default 落别名块、case 体
多候选、补位互证不符、key 位合并点标签）+ REWRITE（原地回填 + 分发器与
键表保持在场、末段补位擦除留 null 实参、拒绝面字节逐位不动）+ PROOF
（类数/重解析/residual 归零、未改写时 residual 如实计）。
