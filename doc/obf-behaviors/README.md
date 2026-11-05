# obf-behaviors：grunt 行为账目录

本目录是 analysis-grunt 的 **grunt 混淆行为账**：把 Grunt 家族两代（v2 Gruntpocalypse / v3 Grunteon）的每个混淆行为逐个写成「做什么（字节码形态 + 可汇编 jasm 样例）/ 配置面 / 判据分级 / 本仓 residue 现状」，并汇总行为 × 处理状态的总表与闭环缺口清单，供后续还原批次排期。

**与 morpher 仓 obf-forms 的分工**：morpher 仓 `mjt-deobf/doc_ai/obf-forms/grunt-obfuscation-forms.md` 是**跨家族形态库**（artifact 侧形态判据与还原链序，13 个已验证形态样例），本目录是**本工具行为账**（逐 transformer 行为 + 配置面 + residue 现状 + 缺口闭环）；同面判据以 morpher 为准、不重复定义，行为级现状以本目录为准。

**文档索引**：

- `grunt-v2-behaviors.md`——v2（`grunt2` 分支 `36c0eef`，tag 线 2.0.0 ~ 2.4.5）35 个 transformer 行为账 + 16 个已验证 jasm 样例。
- `grunt-v3-behaviors.md`——v3（`grunt3` 分支 `e41e852`，Grunteon）39 个 transformer（35 常规 + 4 `@HiddenTransformer`）+ GlslObfuscator 插件 + NativePipeline 阶段的行为账 + 11 个已验证 jasm 样例 + origin/grunt3 领先 13 提交差异标注。
- jasm 样例验证口径与已知校验器边界（`xaload` 族误报、`<init>` 栈帧分析缺陷）见 v2 文档 §0.3：**27/27 通过（25 relaxed + 2 off 通道）**。
- Kasumi 家族专面在 `doc/kasumi-verify.md`（K0~K4），不属本目录。
- residue 逐面文档（number-fold / proxy-inline / predicate-fold / …）在 `doc/` 根目录，本 README 的状态列以它们为事实源。

## 1 v2 行为 × 处理状态总表（32 行覆盖 35 transformer，Kr 变体并行）

| 行为 | 等级 | 状态 | residue / 缺口 |
|---|---|---|---|
| DeclareFields | S1 | 已处理 | ConstValuePromote（判据互为镜像） |
| FieldScramble | S2 | 已处理 | FieldAccessProxy |
| MethodScramble | S2 | 已处理 | ProxyInline |
| SyntheticBridge | S2 | 已处理 | FlagsStrip |
| NumberEncrypt | S1 | 部分 | NumberFold（缺 IAND/LAND 位拼模板与 arrayed 池 → 缺口 #8） |
| ConstPoolEncrypt | S1 | 部分 | NumberFold + ConstValuePromote 折伴生类（缺调用点回填 → 缺口 #5） |
| Controlflow | S1/S2 | 部分 | TrampolineSqueeze / PredicateFold / NumberFold / DeadCode / SeedStrip（缺 junk 调用残留、陷进环 → 缺口 #10/#14） |
| InvokeDynamic | S1 | 预留 | IndyFold（S3，语料 foldable=0；直接解密还原 → 缺口 #2） |
| StringEncrypt | S1 | 无处理 | 缺口 #1 |
| ConstBuilder | S1 | 无处理 | 缺口 #6 |
| ArithmeticEncrypt | S2 | 无处理 | 缺口 #7 |
| MethodExtractor | S2 | 无处理 | 缺口 #9 |
| StringEqualsRedirect | S2 | 无处理 | 缺口 #11 |
| Crasher | S2 | 无处理 | 缺口 #13 |
| Watermark | S2 | 无处理 | 缺口 #13 |
| ClonedClass / TrashClass | S2 | 无处理 | 缺口 #14 |
| HWIDAuthenticator | 检测-only | 无处理 | 缺口 #13（检测/摘除面） |
| NativeCandidate | 检测-only | 无处理 | 登记即可（外部管线选取面） |
| MethodRename(+Kr) / FieldRename(+Kr) / ClassRename / Mixin 改名 | 清理面 | 无处理 | 改名还原走 mapping 旁证，不在 residue 面 |
| SourceDebugRemove / KotlinOptimize / EnumOptimize / DeadCodeRemove / LocalVariableRename / ShuffleMembers / Shrinking / PostProcess | 清理面 | 清理面 | 还原侧无动作 |
| ReflectionSupport | 伴随面 | 伴随面 | 产物无独立形态 |

## 2 v3 行为 × 处理状态总表（39 transformer + 插件 + 阶段）

| 行为 | 等级 | 状态 | residue / 缺口 |
|---|---|---|---|
| NumberBasicEncrypt | S1 | 已处理 | NumberFold 全覆盖（四式全对齐） |
| DeclaredFieldsExtract | S1 | 已处理 | ConstValuePromote |
| InvokeProxy | S2 | 已处理 | ProxyInline |
| FieldAccessProxy | S2 | 已处理 | FieldAccessProxy |
| FakeSyntheticBridge | S2 | 已处理 | FlagsStrip |
| ControlflowJump | S1/S2 | 部分 | TrampolineSqueeze / PredicateFold / NumberFold / DeadCode（缺 processor 调用门与异常桥 → 缺口 #10） |
| ReferenceObfuscate | S1 | 预留 | IndyFold（S3；直接解密 + `@Metadata` 还原 → 缺口 #2） |
| StringArrayedEncrypt | S1 | 无处理 | 缺口 #1 |
| InvokeDispatcher | S2 | 无处理 | 缺口 #3 |
| ControlflowFlattening（含 SSA exp） | S1/S2 | 无处理 | 缺口 #4 |
| ArithmeticSubstitute（Hidden） | S2 | 无处理 | 缺口 #7 |
| StringEqualsOptimize | S2 | 无处理 | 缺口 #11 |
| ParameterObfuscate | S2 | 无处理 | 缺口 #12 |
| TrashClassGenerator | S2 | 无处理 | 缺口 #14 |
| RuntimeMaterial | S2 | 无处理 | 缺口 #15 |
| AntiLLM | S2 | 无处理 | 缺口 #13 |
| Watermark | S2 | 无处理 | 缺口 #13 |
| DecompilerCrasher | S2 | 无处理 | 缺口 #13 |
| NativePipeline | 检测-only | 无处理 | 不可静态还原；识别 = native 方法 + loader 类 + 库资源计数（缺口 #15 检测项） |
| NativeCandidate | 检测-only | 无处理 | 登记即可（NativePipeline 选取面） |
| ClassRenamer / MethodRenamer / FieldRenamer / Mixin 改名 | 清理面 | 无处理 | 改名还原走 mapping 旁证 |
| MethodInliner / ClassShrink / KotlinClassShrink / DeadCodeRemove / EnumOptimize / SourceDebugInfoHide / LocalVarRenamer / ShuffleMembers / PostProcess | 清理面 | 清理面 | 还原侧无动作 |
| FlowIRRoundTrip / SSARoundTrip（Hidden） | 伴随面 | 伴随面 | IR 往返 infra，产物与输入等价 |
| NativePreProcessor | 伴随面 | 伴随面 | native 前置桥接（`__grt$indy$`/`__grt$condy$`/`__grt$mh$` 形状可识别） |
| ReflectionSupport | 伴随面 | 伴随面 | 产物无独立形态 |
| MappingApplier | 伴随面 | 伴随面 | 外部 mapping 应用 |
| GlslObfuscator（grunt-glsl 插件） | 资源面 | 无处理 | GLSL 文本改写，字节码 residue 不适用，登记 |

## 3 闭环缺口清单（按优先级，供后续还原批次）

**P1——还原主链**（直接决定 grunt 闭环对真实产物的可用性，都是语料高频面）：

1. **string-encrypt 还原面**：v2 `StringEncrypt` 与 v3 `StringArrayedEncrypt` 的池读三元组（`GETSTATIC String[]` + 常量下标 + `AALOAD`）折回 `ldc` 明文；seed/key/classKey 全静态在场，解密器按结构直接解（不走 ConstantCall）。v2/v3 同一 residue 承载。
2. **indy 直接解密还原**：v2 `InvokeDynamic` simple/heavy 与 v3 `ReferenceObfuscate` simple/heavy——按 BSM 参数结构直接解密三段串（v2 heavy 另需读 `@GruntMetadata`/v3 `@Metadata` 注解 `mv/d1/d2`，`data1 % 59557 == magic2` 互证），调用点回填真实 invoke；`IndyFold`（S3 模拟面）保留为兜底。
3. **InvokeDispatcher 调用点反查**：按调用点 key 常量在分发器键表定位 case、取转发目标回填真实 invoke、删分发器（default 回跳方法头是识别锚点）。
4. **CFF 反摊平**：v3 `ControlflowFlattening`/SSA 的 dispatcher 岛识别 + 单岛反摊平（state → 块序重建）+ **alias-face 剪枝**（同一真实块的多键别名只留活键）；key processor 藏形由 NumberFold 先解锁。

**P2——加密与控制流扩展面**：

5. **ConstPoolEncrypt 调用点回填**：伴生类 clinit 解密链先折（NumberFold 已可）、字段值回收，再把调用点 `GETSTATIC 伴生类.<字段>` 回填 `ldc` 明文。
6. **ConstBuilder 折回**：switch 全表静态求值（操作数全为 `ConstantValue` 字段 + 封闭 IXOR），真 case 产出折回单条 `ldc`。
7. **ArithmeticEncrypt/ArithmeticSubstitute 恒等简化**：封闭 int 算术恒等式简化链（IXOR/IAND/IOR/INEG 替换族 + 三元组整体替换）。
8. **NumberEncrypt v2 IAND/LAND 位拼模板扩面**：`first & second = v` 对（int/long）进 NumberFold 词汇表；v3 无此形态，仅 v2 语料需要。
9. **MethodExtractor 还原**：透传桩与伴生方法体合并、owner 类 accessor-getter/setter 内联、伴生类删除（面大价值中）。
10. **ControlflowJump processor 门与异常桥**：`INVOKESTATIC (III)I` processor 返回值参与的门需跨方法读 processor 体静态求值；合成 try + `POP; GOTO handler` 异常桥的 CFG 边还原。

**P3——清理、检测与基线面**：

11. **StringEquals 语义还原**：v2 `StringEqualsRedirect` / v3 `StringEqualsOptimize`（hashCode 比较/`==` 换回 `equals`，含 `equalsIgnoreCase` 面）。
12. **ParameterObfuscate 收窄**：私有方法 desc 参数收窄 + 头部 checkcast 删除。
13. **噪声与检测面清理**：Watermark（两代水印字段/方法）、Crasher/DecompilerCrasher（massive Signature 属性）、AntiLLM 四渠道噪声、HWID 校验调用摘除（删除需 CFG 副作用证明）。
14. **垃圾类删除面**：v2 ClonedClass/TrashClass + v3 TrashClassGenerator——零引用 + 生成面形状识别，删除需引用普查兜底（反射/资源文本引用）。
15. **probeEra Grunt2/Grunt3 可靠判据**：现 `probeEra` 只有 Kasumi 可靠；两代 `U+200E` 尾缀同源、模板无稳定差异——待语料积累出真正版本敏感的残留形状再补（era.hpp 明文记载）。
