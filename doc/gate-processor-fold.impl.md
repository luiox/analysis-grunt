# gate_processor_fold 实现笔记

## 数据流

`ScanResult{sites, rejects, totalShape, targetOutside, targetNotStatic, targetSynchronized, targetNoCode, argsNotConstant, forwardShape, tlrWindow, classInitPresent, evalBadState, evalStepLimit, valueKindUnsupported, evalNotTrue, classesSkipped}` → `DirtySet{uris, sites, attempted, failed, skippedByRoundTrip, failedByReason}` → `ProofResult{ok, failed, residual}`。Era 不消费（v3-only 形态——v2 Controlflow 无门点 processor 面，ActionDesc (III)I 形状锚自证家族）；无旁证注入面（判据全容器自证）。拒绝桶 `Reject{callerOwner, callerMethod, callSrcPc, reason}`，reason ∈ `target-outside / not-static / synchronized / no-code / args-not-const / forward-shape / tlr-window / clinit-present / eval-bad / step-limit / value-kind / not-true`。

## 算法（源锚点）

生成面逐字锚来自 grunt3@e41e852 `ControlflowJump.kt` 的 guard 块发射与
`OpaquePredicateProcessorRegistry.kt` / `IntExpression.kt`：

- 门点发射：`INT(left); INT(right); INT(salt); INVOKESTATIC proc(III)I;
  compareValue.emit(); IF_ICMPxx`——proc 是生成面注册的方法（ActionDesc 恰
  为 `(III)I`），compareValue 走 IntConstChain 常量走廊；
- proc 体：槽位解码链（`ILOAD n; <IntConstChain>; IXOR; ISTORE n` × 3）+
  BitAction（SWAP/DUP_X1/DUP2/DUP_X2/POP 栈形）+ PredicateStep（AddSaltRotate/
  RotateBySalt emit `INVOKESTATIC java/lang/Integer.rotateLeft(II)I`）+ IRETURN；
- 恒真保证：生成器 `require(comparison.test(result, compareValue.value))`。

## Scan

两段：形状钉（全容器）→ 求值（按目标 owner 分组）。

1. **形状钉**（`scanMethod`）：`INVOKESTATIC` 描述符恰 `(III)I`（其余 desc
   静默跳过，不计桶）→ 逆向三实参 `const_args::collectArgs`（NOP 跳过，全
   known i32）→ 前窗解析 `parseForwardWindow`：
   - 走廊 = `[INT; (INT; IMUL|IAND|IOR|IXOR)*]` 栈机折叠（JVM 栈语义，交换
     律 + 结合律下左折即真值），NOP 跳过（squeeze 遗留落点）；
   - 终点恰为 `IF_ICMPEQ..IF_ICMPLE` ∧ 折叠栈恰剩 1 个 i32 = compareValue；
   - 停点具名识别：`ThreadLocalRandom` 引用（owner 或 desc 命中）→
     `TlrWindow` 桶（RandomBound 窗口变体，运行期随机排除），其余停点 →
     `ForwardShape`；
   - 目标解析（容器方法索引，constant_call 同源 keyOf）：容器外 / 非 static /
     synchronized 逐桶；目标有码检查在求值段做（loc.index 是目标类下标，
     scan 会话是调用者类——错位检查是单测钉过的缺陷面）。
2. **求值**（`evaluateSites`）：按目标 owner 分组 → 目标类会话上
   `classInitNeutral` 门（非中性时只留同类调用——JVMS 5.5 同类豁免，
   constant_call 同源）→ `SimExecutor.invoke` 跨方法求值（编排层递归穿透
   `__grt$indy$` 转发器；结果缓存 + 深度 6 + 步数预算）→ known i32 出值 →
   `gateVerdict(op, R, compareValue)` 必须 taken（恒真自证门）。
   **mock 面（最小申报）**：只接管 `java/lang/Integer.rotateLeft(II)I`
   （PredicateStep 盐旋转步生成面；jsim 核心/num 包无 JDK 数学面，缺它整桶
   EvalBadState——罐 A 链上 4282 站点全在此）；JVM 语义 = ishl/iushr 位移
   掩码 5 位 `(i << d) | (i >>> (32 - d))`（d &= 31，d=0 恒等）；unknown 按
   未知口径传播不编值。**刻意不装 str/num/reflect/indy 扩展包**：TLR /
   material / indy 蹬床面必须在这里 fail-closed 显形，装包会把它们误放进
   折叠面（罐 A 实测 eval-bad=3686/6139 正是这些面）。

## Rewrite（TAKEN 形，predicate_fold 同款契约）

- 按类聚类 → 方法级 `roundTripSafe` 前置门 → promote 一次 → DOM 改写 →
  lower 全部候选方法（任一失败整类放弃，半改写态不外泄）。
- `rewriteMethod` 逐站：
  1. 分支按 `branchSrcPc + branchOp` 重定位（跨会话稳定）；
  2. 操作数区复核：从分支逆向越过前窗（`isWindowInsn` 宽松形定位）找到门点
     调用，成员全等；`collectArgs` 三实参 `sameValue`；**正向重跑前窗解析**，
     终点分支 srcPc/op 与 compareValue 必须与 scan 全等（权威复核）；
  3. 裁决复核：`gateVerdict(site.branchOp, site.result, site.compareValue)`
     仍 taken（纯函数下恒成立，防御面）；
  4. 标签门：操作数区中段（右/salt/调用/窗内指令）或分支本体带标签 = 合并
     点，整站放弃；区域首条（left 锚）的标签安全；
  5. TAKEN 改写：锚（left 常量装载）原地改 `ICONST_0`（保 srcPc），分支本
     体原地换 `IFEQ`（目标 = 原分支目标，保 srcPc），其余受害者（right/
     salt/调用/窗内走廊）单批 `eraseBatch`（kRejectIfUsed——异常表/帧锚占
     用即拒收，整站回滚 anchor 与分支，不留半改写态）。
- 改写不新增合并点、不插无条件 GOTO（HotSpot 线性走查帧约束，
  predicate_fold 同款）；junk 臂原地保留成不可达码交下游 DeadCode。

## Proof

类数不增 + 脏类可重解析（`proof_tools::classesIntact`，效力门挂时间片）+
residual 复扫（脏类集上重跑全判据——形状 + 求值 + 恒真裁决，仍成站点数；
改写 fail-closed 放弃的站点如实入账，求值失败面从未进改写面不计入；罐 A
单罐门实测 0）。

## 语料账本（本批实测）

- **boar 全 native 对照罐**（`predicateProcessorNativeCandidateRatio=1.00`
  + NativePipeline 全开）：sites=0，rejects：clinit=7534 + eval-bad=6198 +
  tlr=2343 + forward=2456 + args=105（proc 体 100% 随 NativePipeline 出 jar
  ——(III)I 26999 中 18461 native，余为 indy 蹬床）；类条目 CRC 与输入逐条
  全等（jar 级差异仅 CLI writer 的 MANIFEST 重建，纯 writer 行为）。no-op
  门成立。
- **罐 A 原罐单跑**（无上游，`GRUNT_TRACE=1 GRUNT_SLICE_SEC=0`）：scan
  shape=13782 rejects（args=104 forward=2379 tlr=2361 eval-bad=6139
  not-true=0）；rewrite classes=721 **sites=7643/7643** failed=0 proof=ok
  residual=0；类数 3074 不增；`-Xverify:all` 失败集与输入全等（227 NCDFE，
  VerifyError=0）。ReferenceObfuscate 跳过 `@GENERATED_METHOD` → 门点调用
  从不被 indy 包装 → 单跑即可折叠（含上游 ConstantCall 位置的 3361 个）。
- **罐 A 组合链**（`IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,
  GateProcessorFold,CffUnflatten`）：上游四 pass 数字与锚点逐项全等
  （IndyRestore 31196 / StringPoolFold 19512 / ConstantCall 24479 /
  ExcBridgeRestore 13442 pins=1448 failed=124 residual=0）；本 pass scan
  shape=7968 sites=4282 rejects=9374（args=3479 forward=1119 tlr=1090
  eval-bad=3686 not-true=0 clinit=0）；rewrite classes=720
  **sites=4282/4282** failed=0 roundtrip-skip=0 proof=ok **residual=0**；
  CffUnflatten islands=1543 failed=30 residual=30 与锚链逐项相同（门点在
  CFF 岛结构之外，零干扰）；类数 3074 不增；`-Xverify:all` 失败集与输入
  全等；双跑产物逐条目 CRC 全等（3080 条目 diff=0）。
- **桶分解对拍**（census `census_gate.py` r1 供给链口径）：raw (III)I
  13656 = args 桶 3479（slot-fed）+ const-fed 10177；const-fed = 门点 8747
  （plain 7968 + tlr 779）+ 非门点 1430；plain 7968 = rotate 目标 4392 +
  indy 体目标 3576。本 pass 链上：折叠 4282（rotate 目标 verdict-true 面）+
  eval-bad 3686（= indy 体 3576 + TLR 体 110）+ tlr 1090（窗口变体 779 +
  TLR 邻接非门点 311）+ forward 1119（非门点走廊走样）——逐桶与普查同源；
  TLR 体 110 在普查为「eval-success + verdict-false」，本 pass 正确落
  EvalBadState（普查 Python 求值器对 TLR 面过宽，fail-closed 口径以本 pass
  为准）。
- **ConstantCall 分工量化**：5814 个门点调用被上游 ConstantCall 先折
  （= 原罐 shape 13782 − 链上 shape 7968）；其中 3361 本 pass 亦可求值
  （上游已折，无重复面），2453 为 ConstantCall 专用求值面（str/num/reflect/
  indy/Math.max 扩展包语义，本 pass 刻意不装包 fail-closed）——其比较面
  （const-vs-const）是 PredicateFold 判据，v3 链未编入，如实挂账。
- 单测：264/264（基线 251 + 13 新增；jasm 夹具：正例求值/rotateLeft 哨兵/
  跨类/转发器递归/TLR 体/TLR 窗/判非恒真/实参非常量/前窗走样/容器外目标/
  clinit 暴露门/中段标签门/proof+residual 归零）。
