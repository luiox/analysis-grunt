# gate_processor_fold：ControlflowJump 门点 processor 折叠

## 形态

Grunt v3（`ControlflowJump`，grunt3@e41e852）往真分支边上插「processor 门」（guard / mangled-gate 块，生成器 `OpaquePredicateProcessorRegistry`）。生成面逐字：

```java
bipush <left>
bipush <right>
bipush <salt>
invokestatic <proc> (III)I      // 门点 processor（ActionDesc 恰为 (III)I，与门点同类）
<compareValue 常量走廊>          // compareValue.emit()：[INT; (INT; OP)*]，OP ∈ imul/iand/ior/ixor
if_icmpxx  Lreal                // 真分支臂（taken）；fallthrough = junk 臂
```

proc 体（生成面 `IntExpression` / `BitAction`）：三条槽位解码链（`iload n; 常量走廊; ixor; istore n` × 3）+ BitAction 栈算术（SWAP / DUP_X1 / DUP2 / DUP_X2 / POP 形）+ `PredicateStep` 盐旋转步（`INVOKESTATIC java/lang/Integer.rotateLeft(II)I`）+ `ireturn`，全程纯算术。`RandomBound` 变体的 proc 体额外引用 `ThreadLocalRandom`（运行期随机）。ReferenceObfuscate（chance=1.0）按 `@GENERATED_METHOD` 跳过生成方法，所以门点 proc 调用**从不被 indy 包装**——多数门点的调用目标是直线 proc 体，部分经 NativePreProcessor 的 `__grt$indy$` 蹬床（IndyRestore 还原后 = 纯 iload 前缀 + 单 invoke + ireturn 的转发器）。

生成器带 `require(comparison.test(result, compareValue.value))`：门点必须恒走真分支，否则混淆器自己的 payload 都到不了。所以「proc(left,right,salt) 与 compareValue 按分支语义比较 = taken」是**混淆器自证的语义事实**——只要 proc 体能在常量实参下静态求值，分支走向就是确定的。

## 为什么可以静态还原

三个证据面叠起来，分支走向就是静态事实：

- **实参全常量**：left/right/salt 是紧邻纯常量装载（生成面 INT）；compareValue 是 IntConstChain 常量走廊（imul/iand/ior/ixor 全交换律 + 结合律，JVM 栈语义左折即真值）。
- **proc 体封闭纯算术**：生成面判据（解密链 + BitAction + rotateLeft，无堆/无字段/无随机——RandomBound 例外，见边界）。跨方法求值器（jsim 编排层，引擎单方法、编排递归）在常量实参下跑出唯一确定的返回值；转发器（蹬床）经编排层递归穿透。
- **恒真自证**：求值结果 R 与 compareValue C 按 `if_icmpxx` 语义比较必须 = taken（生成面 require 保证）。判非 taken = 求值与生成面脱钩（mock 语义走样 / 目标错解 / 随机体），fail-closed 拒绝——罐 A 实测全量站点 not-true = 0，判据余量充足。

折叠 = 把恒真比较换成 `[ICONST_0, IFEQ→Lreal]` 恒真条件跳转（PredicateFold TAKEN 形同款契约）：不插无条件 GOTO（HotSpot 线性走查要求无条件跳转后段首带帧，被擦延续段在原 SMT 中无帧）；条件跳转让延续段保持 fallthrough 可达，帧面零改动。junk 臂原地保留成不可达码，交下游 DeadCode 清理。

## 与 ConstantCall / PredicateFold 的分工

- `ConstantCall` 折「全常量实参 + 纯算术目标」的**调用点**（调用 → 常量装载），它的求值器装了 str/num/reflect/indy 扩展包 + Math.max 接管；门点 proc 调用是它的站点子集，罐 A 全链实测约 5814 个门点调用被它先折成常量——但**比较面它不碰**（折叠后的 const-vs-const 比较是 PredicateFold 的 face）。
- `PredicateFold` 折「操作数全常量」的**比较**，但看不见藏在前窗常量走廊后面的 proc 调用（操作数不是紧邻常量装载）。
- 本残留 = 门点专用：形状钉（(III)I ActionDesc + 前窗走廊）+ 自带求值器（最小 mock 面：只接管 rotateLeft，刻意不装扩展包——TLR / material / indy 面必须在这里 fail-closed 显形）+ 比较折叠一次完成。

## 链序

**`grunt.GateProcessorFold` 排在 `grunt.ConstantCall` 之后、`grunt.CffUnflatten` 之前**（罐 A 实证链：`IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,GateProcessorFold,CffUnflatten`）：

- 上游 `IndyRestore` 把 `__grt$indy$` 蹬床体内的 indy 还原成直线转发（蹬床可求值面扩大）；
- 上游 `ConstantCall` 先折纯算术 proc 调用（5814 个门点调用 → 常量，其比较面留给 PredicateFold，v3 链未编入）——本残留吃的是 ConstantCall 求值不动的余量（rotateLeft 体），上游数字逐项不动（罐 A 复测：四上游 pass 数字与锚点逐项全等）；
- 下游 `CffUnflatten` 岛还原数字不受影响（门点在 CFF 岛结构之外，罐 A 复测 islands 1543 不变）。

**无硬链依赖**：ReferenceObfuscate 跳过生成方法，门点 proc 调用从不被 indy 包装——本残留单跑（无任何上游）在罐 A 原罐上即可折叠 7643 站点（含 ConstantCall 位置的 3361 个 + 上游不回归的 rotateLeft 体 4282 个）。推荐位置挂在 ConstantCall 之后是为了**不动既有锚点**（单跑会让 ConstantCall 的 24479 锚点位移——门点调用被本残留先折）。

## 边界（不还原的面）

- **RandomBound / TLR（运行期随机，明确排除）**：窗口变体（TLR.current 取数进前窗）在前窗停点识别入 `tlrWindow` 桶；proc 体引用 ThreadLocalRandom 的不在容器内、无 mock，求值 `EvalBadState` 拒绝。运行期随机面永久排除。
- **material-key 桶（禁碰面）**：RuntimeMaterial 类里的门点蹬床体内 indy 走材料解密器（key = decryptKey ^ materialKey()，运行期材料），IndyRestore keyMissing 未还原，蹬床体内仍是 INVOKEDYNAMIC，求值 `EvalBadState` 拒绝。解锁属 material_key_restore / indy_restore 本体（禁碰）；链序实验已证罐 A 上不可达（MaterialKeyRestore 窗口判据被 K 走廊打破，material-classes=0，见 corpus-ledger）。
- **上游 ConstantCall 专用求值面**：约 2453 个门点 proc 体引用 Math.max / 扩展包语义（str/num/reflect/indy），本残留刻意不装包 fail-closed（`EvalBadState`）；这些调用已被上游 ConstantCall 折叠，无 restoration 损失。
- **indy-wrapped proc 体**：罐 A 链上面 3576 个门点目标体仍是 indy 形（material 之外的 indy 蹬床），求值 `EvalBadState`——indy 蹬床的求值接管属 indy_fold 面。
- 拒绝面逐桶记账（target-outside / not-static / sync / no-code / args / forward / tlr / clinit / eval-bad / step-limit / value-kind / not-true），字节不动，trace 可见（`GRUNT_TRACE=1` 桶计数，`GRUNT_GATE_PROCESSOR_FOLD_TRACE=1` 逐条）。

## 语料实证（罐 A，`doc/corpus-ledger.md` §1）

- **boar 全 native 对照罐**：scan shape=13732 sites=0（rejects：clinit=7534 + eval-bad=6198 + tlr=2343 + forward=2456 + args=105——proc 体随 NativePipeline 出 jar / indy 化，全数 fail-closed）；类条目 CRC 与输入逐条全等（jar 级差异仅 CLI writer 的 MANIFEST 重建）——no-op 门成立。
- **罐 A 原罐单跑**：shape=13782，**sites=7643/7643 折叠**（classes=721，failed=0，residual=0），eval-bad=6139、tlr=2361、forward=2379、args=104，not-true=0；类数 3074 不增；`-Xverify:all` 失败集与输入全等（227 NCDFE 外部依赖，VerifyError=0）。
- **罐 A 组合链**（`IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,GateProcessorFold,CffUnflatten`）：上游四 pass 数字与锚点逐项全等（31196 / 19512 / 24479 / 13442）；本 pass shape=7968 sites=4282/4282（classes=720）failed=0 residual=0（eval-bad=3686 = material/indy 体 3576 + TLR 体 110；tlr=1090；forward=1119；args=3479）；CffUnflatten islands=1543 不变；`-Xverify:all` 失败集与输入全等；双跑产物逐条目 CRC 全等。
- **census 对拍**（独立字节级普查 `census_gate.py` 口径，r1 供给链产物）：raw (III)I 13656 / const-fed 10177 / 门点 8747（plain 7968 + tlr 779）/ plain-foldable 4282 / verdict true 4282——本 pass 链上 scan shape=7968、折叠 4282、not-true=0 与普查逐项同源（TLR 体 110 在普查为 eval-success 但 verdict-false，本 pass 正确落 EvalBadState——普查的 Python 求值器对 TLR 面过宽，fail-closed 口径以本 pass 为准）。
- 单测：264/264（基线 251 + 13 新增，jasm 夹具现场汇编）。
