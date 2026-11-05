# Grunt v3（Grunteon）obf 行为核心描述

> 适用对象：Grunt 混淆家族 v3 代（Kotlin 重写的 "Grunteon"）。本仓知识源锚定 `grunt3` 分支本地 HEAD `e41e852`；`origin/grunt3`（`a088924`）领先 13 个 fix 级提交，差异点见 §0.4。
> 知识来源：Grunt 源码仓（只读）`grunt3` 分支 `transformers/` 树 39 个 transformer（35 个常规 + 4 个 `@HiddenTransformer`）逐个源码盘点，另加 grunt-glsl 插件 transformer 与 NativePipeline 独立执行阶段；`analysis-grunt` 仓现有 residue 面做处理现状对照。
> jasm 样例为按行为描述自写的完整可汇编样例，全部经 mjt-py SDK `class_validate_jasm`（libjhasm 汇编 + 校验门）逐类验证，口径与 v2 文档 §0.3 相同；判据锚点以行内注释标注，样例中的类名/方法名/密文与常量数值只作形状示意，不是判据。
> 与 morpher 仓 `mjt-deobf/doc_ai/obf-forms/grunt-obfuscation-forms.md` 的分工：那边是**跨家族形态库**（artifact 侧形态判据与还原链序），本文是**本工具行为账**（v3 逐 transformer 行为 + 配置面 + 本仓 residue 现状 + 闭环缺口），同面判据以那边为准、不重复定义。
> 排版：正文一句话/一段落一整行，不做 80 列硬断；代码块与表格内自然换行。
> 日期：2026-11-06。

---

## 0.1 家族底座

v3 是 Kotlin 重写的管线：`Grunteon` 持 `transformers: List<Pair<Transformer<*>, TransformerConfig>>`，`PipelineBuilder` 用指令式 DSL（`Seq` / `Pre` / `Post` / `SeqForEach` / `ParForEach` + `ScopeValue` 全局/局部作用域状态）把各 transformer 的 `buildStageImpl` 编成执行计划，按 `ExecutionStage` 三段跑：`JvmObfuscation` → `NativeObfuscation`（NativePipeline，见 §5）→ `FinalOutput`（JarDumper + 可选 mapping dump）。Transformer 基类带 `Category` 排序约束（如 Optimization 先于 Encryption/Controlflow/AntiDebug/Redirect/Renaming，由 `before(...)` 声明）、`@Stability` 稳定级、`@CreditMultiplier` 计费乘数与 `@HiddenTransformer` 隐藏标记；配置是 `TransformerConfig` data class + `@SettingName`/`@SettingDesc`/区间注解（JSON 序列化），类级过滤统一走 `ClassFilterConfig`。

四个 `@HiddenTransformer`：`ControlflowFlatteningSSA`（exp/，SSA 摊平实验面）、`FlowIRRoundTrip` / `SSARoundTrip`（roundtrip/，IR 往返 infra transformer）、`ArithmeticSubstitute`（encrypt/，v2 ArithmeticEncrypt 同构，标 deprecated 沿用）。**Era 现状**：与 v2 文档 §0.1 同口径——本仓 `probeEra` 只有 Kasumi 有可靠判据，Grunt3 无可靠判据（`U+200E` 尾缀两代同源、调用点模板无稳定差异），v3 行为判据一律按 S1/S2 通用语义事实写、不挂 Era 门。

**v3 新面（相对 v2）**：一是 IR 化——`grunt-ir` 提供 Flow IR（`flow/`：FlowBytecode/FlowFrame/FlowGraph/JvmFlowExporter/JvmFlowImporter/FlowVerifier）与 SSA IR（`ssa/`：SSAFunction/SSARegion/SSAValue/JvmSSAExporter/JvmSSAImporter），控制流摊平在 IR 上做再导回字节码；二是 hierarchy 面——`ClassHierarchy`/`FieldHierarchy`/`MethodHierarchy` + `ClassHierarchyFlowTypeHierarchy` 供 CFF 的 `FlowAnalyzer=Hierarchy` 模式做帧重建的精确类型判定；三是 native 化管线——独立执行阶段把方法体编译进 C++ 库（§5）；四是插件体系——`PluginManager`/`GruntPlugin`，grunt-glsl 即插件形态的 GLSL shader 混淆（§8）。`grunt-index`（类索引库）/ `grunt-backend`（作业调度 Web 服务）/ `grunt-bootstrap`（LaunchClassLoader 运行壳）是基础设施模块，不产 obf 形态，登记不展开。

## 0.2 行为清单总表（39 transformer + 插件 + 阶段）

等级与现状列口径同 v2 文档 §0.2（S1 语义后果 / S2 结构事实 / S3 预留面；已处理 / 部分 / 预留 / 无处理 / 清理面 / 伴随面 / 检测-only）。

| # | 类别 | 行为 | 一句话 | 等级 | 本仓现状 |
|---|---|---|---|---|---|
| 1 | Optimize | MethodInliner | 同类小方法内联 | 清理面 | 清理面 |
| 2 | Optimize | ClassShrink | 删 inner classes/未用标签/NOP/方法 Signature | 清理面 | 清理面 |
| 3 | Optimize | KotlinClassShrink | 删 Kotlin Metadata 注解与 Intrinsics 检查 | 清理面 | 清理面 |
| 4 | Optimize | DeadCodeRemove | 删无用指令序列（load+POP 族） | 清理面 | 清理面 |
| 5 | Optimize | EnumOptimize | values() 摘 clone + CHECKCAST | 清理面 | 清理面 |
| 6 | Optimize | SourceDebugInfoHide | 删 source file 信息与行号 | 清理面 | 清理面 |
| 7 | Optimize | StringEqualsOptimize | equals/equalsIgnoreCase 换比较 | S2 | 无处理 |
| 8 | Encryption | NumberBasicEncrypt | 数值藏形（走廊/L2I+IXOR/基 32/位形四式） | S1 | 已处理（NumberFold 全覆盖） |
| 9 | Encryption | StringArrayedEncrypt | String[] 池加密（carray 密文直存） | S1 | 无处理 |
| 10 | Encryption | ArithmeticSubstitute（Hidden） | 算术指令恒等替换 | S2 | 无处理 |
| 11 | Controlflow | ControlflowJump | junk 分支/mangled IF/opaque predicate 门/异常桥 | S1/S2 | 部分（TrampolineSqueeze/PredicateFold/NumberFold/DeadCode 处理静态可裁决子集；processor 门与异常桥无面） |
| 12 | Controlflow | ControlflowFlattening | Flow IR 摊平（dispatcher 岛/state key/fake case） | S1/S2 | 无处理 |
| 13 | Controlflow | ControlflowFlatteningSSA（Hidden） | SSA IR 摊平（exp 实验面） | S1/S2 | 无处理 |
| 14 | Controlflow | FlowIRRoundTrip（Hidden） | 方法体经 Flow IR 往返 | 伴随面 | 伴随面（infra） |
| 15 | Controlflow | SSARoundTrip（Hidden） | 方法体经 SSA IR 往返 | 伴随面 | 伴随面（infra） |
| 16 | Redirect | InvokeDispatcher | 同返回类型调用点合并进 tableswitch 分发器 | S2 | 无处理 |
| 17 | Redirect | InvokeProxy | 方法调用换 static 转发代理 | S2 | 已处理（ProxyInline） |
| 18 | Redirect | FieldAccessProxy | 字段访问换 getter/setter 代理 | S2 | 已处理（FieldAccessProxy） |
| 19 | Renaming | ClassRenamer | 类改名 | 清理面 | 无处理（改名面） |
| 20 | Renaming | MethodRenamer | 方法改名 | 清理面 | 无处理（改名面） |
| 21 | Renaming | FieldRenamer | 字段改名 | 清理面 | 无处理（改名面） |
| 22 | Renaming | LocalVarRenamer | 局部变量改名/删表 | 清理面 | 清理面 |
| 23 | Renaming | MixinClassRenamer | Mixin 感知类改名 | 清理面 | 无处理（改名面） |
| 24 | Renaming | MixinFieldRenamer | Mixin 感知字段改名 | 清理面 | 无处理（改名面） |
| 25 | Renaming | MappingApplier | 应用外部 mapping（替代改名） | 伴随面 | 伴随面 |
| 26 | Miscellaneous | DeclaredFieldsExtract | `ConstantValue` 搬进 `<clinit>` 写（仅 static） | S1 | 已处理（ConstValuePromote） |
| 27 | Miscellaneous | ParameterObfuscate | 私有方法引用参数宽化 Object + 头部 checkcast | S2 | 无处理 |
| 28 | Miscellaneous | TrashClassGenerator | provider 化垃圾类生成 | S2 | 无处理 |
| 29 | Native | NativeCandidate | 按规则给方法挂 native 候选注解 | 检测-only | 无处理 |
| 30 | Native | NativePreProcessor | indy/condy/MH 调用点桥接同类 helper | 伴随面 | 伴随面（native 前置） |
| 31 | AntiDebug | RuntimeMaterial | 分布式材料扰动（shareA/shareB/canonicalKey/poison） | S2 | 无处理 |
| 32 | AntiDebug | AntiLLM | 四渠道 LLM 诱饵噪声 | S2 | 无处理 |
| 33 | Other | ReferenceObfuscate | 调用点 indy 化 simple/heavy + `@Metadata` 注解 | S1 | 预留（IndyFold S3 面） |
| 34 | Other | FakeSyntheticBridge | 伪 `ACC_SYNTHETIC`/`ACC_BRIDGE` 撒标志 | S2 | 已处理（FlagsStrip） |
| 35 | Other | DecompilerCrasher | Signature 属性填 massive 串 | S2 | 无处理 |
| 36 | Other | ReflectionSupport | 改名后重映射反射串 | 伴随面 | 伴随面 |
| 37 | Other | ShuffleMembers | 打乱成员/注解/异常表顺序 | 清理面 | 清理面 |
| 38 | Other | Watermark | 水印字段/方法（Grunt/Gruntpocalypse/Grunteon） | S2 | 无处理 |
| 39 | Other | PostProcess | Manifest/YML/JSON 主类重映射、收尾清理 | 清理面 | 清理面 |
| P | 插件 | GlslObfuscator（grunt-glsl） | GLSL shader 资源混淆（改名/内联/#include） | 资源面 | 无处理（非字节码面，登记） |
| S | 阶段 | NativePipeline（NativeObfuscation） | 方法体 native 化 + loader 类 + 库资源嵌入 | 检测-only | 无处理（不可静态还原，见 §5） |

## 0.3 样例验证口径

v3 共 11 个样例文件（v3-01 ~ v3-10，其中 native 面两个类拆 v3-09/v3-09b），10 个过 relaxed 语义校验档，1 个（string-carray）因校验器对 `aaload` 家族的已知误报走 `validation=off` 通道；与 v2 文档 16 个样例合计 27/27（25 relaxed + 2 off）。误报模式与 `<init>` 形状规避的完整记录见 v2 文档 §0.3，此处不重复。

## 0.4 origin/grunt3 领先的 13 个提交（差异标注）

本地锚定 `grunt3@e41e852`；`origin/grunt3@a088924` 领先 13 个 fix 级提交，内容：renamer exclusions 修正、native 库 CRC 修复、record 组件名保留、DeclaredFieldsExtract 收紧为仅 static `ConstantValue`（本文按收紧后口径描述）。其余为 fix 级，不引入新行为面；后续语料升级时按此清单复核差异即可。

---

## 1 加密面

### 1.1 StringArrayedEncrypt（String[] 池加密）

**做什么**。逐类收集非空字符串 `ldc` 建每类一个 `String[]` 池字段 + 随机名解密方法 + `arrayInit` 静态方法，`<clinit>` 头部插池分配前导，调用点原地换 `GETSTATIC pool; idx; AALOAD`——骨架与 v2 StringEncrypt 同构（v2 文档 §1.1），差别两点：`Carray=true`（默认开）时密文以 `char[]` 直存——`arrayInit` 逐项 `NEWARRAY T_CHAR` + 逐码元 `CASTORE` 写密文，免去 `ldc` 密文串与 `toCharArray`，池里不再出现明文形态的字符串常量；`InvokeDynamics=true`（默认开）把 `makeConcatWithConstants` indy 调用点先替换成 `ldc` 模板串 + 每类一个 concat wrapper 方法再一并进池。解密器 desc 固定 `([CJI)Ljava/lang/String;`（char[] + long seed + int key），体是带循环的 char 级 XOR 流。

**配置面**：`Carray=true` / `InvokeDynamics=true` / `Exclusion`。

**判据分级**：S1——与 v2 StringEncrypt 同级：池读三元组（`GETSTATIC` String[] + 常量下标 + `AALOAD`）编译期可建映射，读取点换 `ldc` 明文是语义恒等；carray 形态下密文在 `arrayInit` 的 `CASTORE` 序列里，仍全为装载期常量。

**本仓现状**：无处理（还原缺口 #1 的 v3 面，缺口清单 P1）。morpher obf-forms §3.1 的池读三元组判据两代共用。

**样例**（carray 形态；本样例走 `validation=off` 通道，误报记录见 v2 文档 §0.3）：

```jasm
.super java/lang/Object
.class public super GtStrArray {
    .field private static pool [Ljava/lang/String;

    .method static <clinit> ()V {
        code: {
            iconst_1                          // 池分配前导（判据锚点，同 v2）：
            anewarray java/lang/String        //   {池长; ANEWARRAY String;
            putstatic GtStrArray.pool [Ljava/lang/String;
            invokestatic GtStrArray.arrayInit ()V
            return
        }
    }

    .method private static arrayInit ()V {
        code: {                               // carray 密文直存：NEWARRAY T_CHAR
            getstatic GtStrArray.pool [Ljava/lang/String;
                                              //   + 逐码元 CASTORE（判据锚点：
                                              //   密文不在常量池，在数组写序列）
            iconst_0
            bipush 3
            newarray char
            dup
            iconst_0
            ldc 64206
            castore
            dup
            iconst_1
            ldc 64207
            castore
            dup
            iconst_2
            ldc 64208
            castore
            ldc 889226191L                    // seed(long) + key(int) → dec
            ldc 17202
            invokestatic GtStrArray.dec ([CJI)Ljava/lang/String;
            aastore
            return
        }
    }

    .method private static dec ([CJI)Ljava/lang/String; {
        code: {
            aconst_null                       // 解密器体（骨架）：带循环的 char
            areturn                           //   XOR 流，判据不依赖算法细节
        }
    }

    .method public static read ()Ljava/lang/String; {
        code: {
            getstatic GtStrArray.pool [Ljava/lang/String;
                                              // 判据锚点（读取点三元组）：GETSTATIC
            iconst_0                          //   钉 String[] + 常量下标 + AALOAD
            aaload
            areturn
        }
    }
}
```

### 1.2 NumberBasicEncrypt（数值藏形，四式）

**做什么**。把方法体里的数值常量装载换成等价运算跨度，int/long/float/double 四类各带 chance 开关（默认全开、全 1.0），`MaxInstructions=16384` 防超限，`DynamicStrength` 按方法余量调强度。相对 v2 NumberEncrypt 明显收敛：**只有四式**——int 长走廊（`INT neg; I2L; INT obf; I2L; LXOR; L2I`）、int 短走廊（`LDC neg.toLong(); L2I; INT obf; IXOR`）、long 基 32（`LDC "…"; LDC 32; Long.parseUnsignedLong(String,I)J; LDC key; LXOR`）、float/double 位形（bits 值 `IXOR` 后过 `Float.intBitsToFloat` / `Double.longBitsToDouble`）。v2 的 IAND/LAND 位拼模板与 arrayed long[] 池在 v3 **不存在**。

**配置面**：`Integer/Long/Float/Double` + 各自 `Chance` / `MaxInstructions` / `DynamicStrength` / `Exclusion`。

**判据分级**：S1——四式全是纯常量封闭求值（走廊 XOR、基 32 严格解析、位形换算 JDK 语义确定），折回单条常量装载是精确逆。

**本仓现状**：已处理——`grunt.NumberFold` 的词汇表恰好覆盖这四式（走廊两式 + 基 32 + 位形包装），是 v3 侧全对齐的 residue。v2 的 IAND/LAND 模板缺口不适用于 v3。

**样例**（四式并列；`17202 ^ 16602 = 1000`、`17202 ^ 17179 = 41`、`1069547520` = 1.5F 的 IEEE 位形）：

```jasm
.super java/lang/Object
.class public super GtNumberBasic {
    .method public static intCorridor ()I {
        code: {
            ldc 17202                         // int 长走廊 1/6：INT(neg)
            i2l                               // 2/6：I2L（升 long 域，判据锚点）
            ldc 16602                         // 3/6：INT(obf)
            i2l                               // 4/6：I2L
            lxor                              // 5/6：LXOR
            l2i                               // 6/6：L2I → = ldc 1000
            ireturn
        }
    }

    .method public static intShort ()I {
        code: {
            ldc 17202L                        // int 短走廊 1/4：LDC(neg.toLong())
            l2i                               // 2/4：L2I 截断
            ldc 17179                         // 3/4：INT(obf)
            ixor                              // 4/4：→ = ldc 41
            ireturn
        }
    }

    .method public static longBase32 ()J {
        code: {
            ldc "938ljo"                      // long 基 32 式 1/5：LDC 密文串
            ldc 32                            // 2/5：radix = 32（判据锚点）
            invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J
                                              // 3/5：基 32 无符号解析
            ldc 123457026754817L              // 4/5：LDC key
            lxor                              // 5/5：LXOR → 明文 long
            lreturn
        }
    }

    .method public static floatBits ()F {
        code: {
            ldc 1069547520                    // 位形式 1/4：int 域 bits（= 1.5F）
            ldc 17202                         // 2/4：INT(key)
            ixor                              // 3/4：IXOR → 明文 bits
            invokestatic java/lang/Float.intBitsToFloat (I)F
                                              // 4/4：判据锚点——位形换算后按
                                              //   float 常量发 ldc
            freturn
        }
    }
}
```

### 1.3 ArithmeticSubstitute（Hidden）：算术恒等替换

v2 `ArithmeticEncrypt` 的 v3 同构（`encrypt/ArithmeticSubstitute.kt`，标 `@HiddenTransformer` 沿用 deprecated 面）：`IXOR`/`IAND`/`IOR`/`INEG` 等二元整算术指令换成封闭恒等序列，`Chance=0.3`、`MaxInstructions=16384`、`DynamicStrength`。判据 S2、本仓无处理，与 v2 文档 §1.3 同一缺口（缺口清单 P2），样例形状沿用 v2 文档 §1.3 所述替换族，不另附样例。

## 2 控制流面

### 2.1 ControlflowJump：junk 分支 / mangled IF / 异常桥

**做什么**。v2 Controlflow 的 v3 对应物，按选项组合三件套：

1. **junk 分支**（`Chance=0.25`，`MaxBranchesPerMethod=4`）：向方法插入不可达垃圾块——垃圾值计算 + 垃圾出口（`JunkTerminalThrowChance=0.2` 直接 `ATHROW` 收尾，否则 `SharedJunkExitChance=0.65` 多个垃圾块共享一个出口块）。
2. **mangled IF**（`MangledIfChance=0.25`，`MaxMangledIfsPerMethod=4`）：真实分支条件改写成 opaque predicate 门——谓词处理器（`OpaquePredicateProcessorRegistry` 注册的可插拔处理器，生成的同类 static helper 形如 `(III)I`）用紧邻常量算出编译期确定的值，`IF_ICMPxx` 恒真/恒假落真目标；`MangledFakeLoopChance=0.35` 让门落空侧构成假循环；`DispatcherLandingJunkChance` 给分发器落点再加垃圾块。
3. **异常桥**（`ExceptionBridgeChance`，默认 0，`MaxExceptionBridgesPerMethod=4`）：把一段线性区间包进合成 try 区间——区间内正常流被改为 `ATHROW` 合成异常，handler `POP` 掉异常后 `GOTO` 真目标——CFG 边从「正常跳转」伪装成「异常边」。

**判据分级**：S1（门操作数全为紧邻常量/processor 体常量封闭、静态可裁决的子集）/ S2（垃圾块、异常桥的结构事实）。

**本仓现状**：部分。TrampolineSqueeze 吃蹦床 `goto`；PredicateFold 吃常量可裁决的分支/switch 门；NumberFold 解锁藏形实参区；DeadCode 清不可达垃圾块。**缺口**：`IF_ICMPxx` 门落在 processor 方法调用上时（操作数不是紧邻常量而是 `INVOKESTATIC` 返回值）PredicateFold 不吃——需跨方法读 processor 体；异常桥面（合成 try + `POP; GOTO handler`）无还原面。缺口清单 P2。

**样例**（processor 门 + 异常桥两形并列）：

```jasm
.super java/lang/Object
.class public super GtCfJump {
    .method public static gated ()I {
        code: {
            iconst_3                          // processor 门（判据锚点）：实参全为
            iconst_5                          //   紧邻常量 → INVOKESTATIC processor
            iconst_1                          //   → IF_ICMPxx 恒真门；processor 体
            invokestatic GtCfJump.pred (III)I //   也全为常量封闭（可静态求值）
            ldc 13                            // pred(3,5,1) = 13，恒真落 Lreal
            if_icmpeq Lreal
            iconst_0                          // 落空侧垃圾（不可达）
            pop
        Lreal:
            ldc 7
            ireturn
        }
    }

    .method static pred (III)I {
        code: {                               // opaque predicate processor：
            iload 0                           //   -( (a+b)*c ) + 21，全常量封闭
            iload 1                           //   （判据读体内常量封闭性，不读
            iadd                              //    方法名/标志）
            iload 2
            imul
            ineg
            ldc 21
            iadd
            ireturn
        }
    }

    .method public static bridged ()I {
        exceptions: {
            { Lts, Lte, Lh, java/lang/RuntimeException }
        },
        code: {
        Lts:                                  // 异常桥（判据锚点）：区间 [Lts,Lte)
            new java/lang/RuntimeException    //   被合成 try 包住，正常流 ATHROW
            dup                               //   合成异常，handler POP 后 GOTO
            invokespecial java/lang/RuntimeException.<init> ()V
            athrow                            //   真目标——CFG 边伪装成异常边
        Lte:
        Lh:
            pop
            goto Ltarget
        Ltarget:
            ldc 9
            ireturn
        }
    }
}
```

### 2.2 ControlflowFlattening：Flow IR 摊平

**做什么**。方法体先经 Flow IR 往返（`JvmFlowImporter` 导入，`FlowAnalyzer=Hierarchy` 用 hierarchy 面做帧重建），在 IR 上把基本块序列摊平成 dispatcher 岛结构再导回字节码：每个摊平区域一个局部 state 变量 + `tableswitch` dispatcher（`Ld` 岛），各真实块尾改写 state 并 `GOTO` dispatcher；`FakeCasesPerDispatcher=1` 加假 case、`JunkCases=true`（`JunkCaseChance=0.35`）加垃圾 case；state key 可经 `CffKeyProcessorRegistry` 的 key processor（IntExpression 等价算术跨度）藏形。`IncludeMethodEntry/ExceptionBlocks/Constructors(false)/SyntheticBridgeMethods/DefaultHelperMethods` 控制覆盖面，`MinFlattenedBlocks=2`、`MaxDispatcherIslands` 控制粒度，`VerifyBytecode=true` 导回后 ASM 复验。

**判据分级**：S1（dispatcher 键表与 state 传播在块序已知时编译期可重建——单 dispatcher 岛可反摊平）/ S2（多岛、key processor 藏形、fake/junk case 识别是结构事实）。

**本仓现状**：无处理。还原缺口（缺口清单 P1）：dispatcher 岛识别 + 单岛反摊平（state → 块序重建）+ **alias-face 剪枝**（同一真实块经多个 state 键别名进入时，键表里只有一个是活键，其余是 alias face，剪掉才能恢复线性块序）；key processor 藏形由 NumberFold 解锁。morpher obf-forms §3.7 已给 dispatcher 岛形态判据。

**样例**（单 dispatcher 岛 + fake case + alias face）：

```jasm
.super java/lang/Object
.class public super GtCff {
    .method public static run (I)I {
        code: {
            ldc 100                           // state 初始化（判据锚点：方法头
            istore 1                          //   ISTORE 常量 → GOTO dispatcher）
            goto Ld
        Ld:
            iload 1                           // dispatcher 岛：ILOAD state +
            tableswitch {                     //   tableswitch（判据锚点：default
                min: 100,                     //   回跳 dispatcher 头，键表连续）
                max: 102,
                cases: { Ld, Lb0, Lfake },
                default: Ld
            }
        Ld_alias:                             // alias face（判据锚点）：GOTO 回
            goto Ld                           //   dispatcher 的别名块——P1 剪枝对象
        Lb0:
            iload 0                           // 真实块：原逻辑 + 尾部改写 state
            iconst_3                          //   （istore 101）→ GOTO dispatcher
            imul
            istore 2
            ldc 101
            istore 1
            goto Ld
        Lfake:
            iconst_0                          // fake/junk case（不可达或死态）
            pop
            goto Ld
        }
    }
}
```

### 2.3 ControlflowFlatteningSSA（Hidden）：SSA 摊平实验面

与 §2.2 同构但走 SSA IR（`SSAAstBuilder` → `SSAControlFlowFlattener`/`SSARegionControlFlowFlattener` → `JvmSSAExporter` 导回），`@HiddenTransformer` 实验面。保守跳过构造器（uninitializedThis 不能被 dispatcher 分派）、synthetic bridge、`$default` 方法与未初始化 NEW 跨控制流的方法；预算门 `MaxInstructions=1000`/`MaxLocals=64`/`MaxRegionBlocks=8`/`MaxEstimatedDispatcherArgs=64` 超限跳过。判据与还原面同 §2.2（SSA 摊平导回后仍是 dispatcher 岛形态），本仓无处理，语料命中前按 §2.2 的缺口一并考虑。

### 2.4 FlowIRRoundTrip / SSARoundTrip（Hidden）：IR 往返 infra

无混淆语义的 infra transformer：把方法体经 Flow IR / SSA IR 导入再导出（`VerifyExportedMethod=true` ASM BasicInterpreter 复验），供其他面组合或做规范化。本仓无对应 residue（伴随面）；产物形态与输入等价，无需处理。

## 3 重定向面

### 3.1 InvokeDispatcher：tableswitch 分发器

**做什么**。把同返回类型的多个方法调用点合并进一个分发器：按返回类型分组（每组 ≥2 个调用点、`MaxHandles=10`、`MaxParams=10`、参数含 primitive 的调用点跳过），生成分发器方法 `PRIVATE + STATIC`、名 `dispatcher_<10 随机字符>`、desc `(N × Object; I) ret`（N = 组内最大参数槽位数）；体为 `ILOAD key; TABLESWITCH`（键连续 `startCase..endCase`，**default 回跳分发器方法头**）+ 各 case `CHECKCAST` 实参后转发真实目标；调用点原地补 `ACONST_NULL` 对齐缺参 + 压 access key 常量 + `INVOKESTATIC 分发器`。

**配置面**：`Chance=0.3` / `Max params=10` / `Max handles=10` / `Exclusion`。

**判据分级**：S2——分发器表 + 连续键 + default 回跳头 + case 转发关系全是结构事实，可静态重建 key → case → 真目标映射。

**本仓现状**：无处理。还原缺口（缺口清单 P1）：调用点反查——按调用点 key 常量在分发器键表定位 case、取转发目标直接回填真实 invoke 并删分发器；morpher obf-forms §3.3 已给形态判据。

**样例**：

```jasm
.super java/lang/Object
.class public super GtDispatcher {
    .method private static dispatcher_xKq9Tz (Ljava/lang/Object;Ljava/lang/Object;I)V {
        code: {
        Lhead:                                // 判据锚点：tableswitch 的 default
            iload 2                           //   回跳方法头（生成器防死循环的
            tableswitch {                     //   习惯形，两代同款结构事实）
                min: 247157911,
                max: 247157912,
                cases: { Lc0, Lc1 },
                default: Lhead
            }
        Lc0:
            aload 0                           // case 转发：CHECKCAST 实参 →
            checkcast java/io/PrintStream     //   真实 invoke（key → 目标映射
            aload 1                           //   编译期可重建）
            checkcast java/lang/String
            invokevirtual java/io/PrintStream.println (Ljava/lang/String;)V
            return
        Lc1:
            aload 0
            checkcast java/io/PrintStream
            aload 1
            checkcast java/lang/String
            invokevirtual java/io/PrintStream.print (Ljava/lang/String;)V
            return
        }
    }

    .method public static callSite ()V {
        code: {
            getstatic java/lang/System.out Ljava/io/PrintStream;
            aconst_null                       // 调用点（判据锚点）：ACONST_NULL
            ldc 247157912                     //   对齐缺参 + 压 key 常量 +
            invokestatic GtDispatcher.dispatcher_xKq9Tz (Ljava/lang/Object;Ljava/lang/Object;I)V
                                              //   INVOKESTATIC 分发器
            return
        }
    }
}
```

### 3.2 InvokeProxy / FieldAccessProxy：代理转发

v3 的方法/字段代理与 v2 MethodScramble/FieldScramble 同构（`Chance` 概率、`Outer=true` 生成外置伴生类、`GetStatic/PutStatic/GetField/PutField` 四向开关），判据 S2、本仓已处理（`grunt.ProxyInline` / `grunt.FieldAccessProxy`，判据不依赖方法名/标志/家族名）。行为描述与完整样例沿用 v2 文档 §3.1（getter/setter/方法代理 + 调用点改回），不重复。

### 3.3 ReferenceObfuscate：调用点 indy 化（simple/heavy + `@Metadata`）

**做什么**。v2 InvokeDynamic 的 v3 对应物：非接口类、版本 ≥ V1_7、未被排除的调用点换成 `invokedynamic`。simple 形：BSM `PUBLIC+STATIC+SYNTHETIC+BRIDGE`、desc `(Lookup,String,MethodType,String,String,String,Integer)` 七参，静态参数 `[encOwner, encName, encDesc, isVirtual]` 三段串密文 + 虚/实标志。heavy 形：BSM `PRIVATE+STATIC+SYNTHETIC+BRIDGE`、desc `(Lookup,String,MethodType,String,Integer,Integer,Integer)` 七参，静态参数 `[encData, magic1, magic2, isVirtual]`，真目标编进 owner 类的 `@Metadata` 注解——注解类由生成器现场造（`implements java/lang/annotation/Annotation` + `@Retention(RUNTIME)` + `@AnnotationDefault`，成员 `mv`(int，默认 100) / `d1`/`d2`(String[])，类名 `MetadataClass` 配置项，默认 `net/spartanb312/grunteon/internal/Metadata`）；per-method `data1 = 59557 * q + magic2`（**`data1 % 59557 == magic2` 完整性互证**）加密后进 `d1`，真载荷 `data2` 经 `magic1` 加密进 `d2`；heavy BSM 运行时 `getAnnotation(Metadata.class)` → `mv()` 版本检查 → `d1()/d2()` 取值解密。`ReobfBSM=true`（默认）让 BSM 体再过 Controlflow（NativePipeline 开启时自动关以免 native 侧膨胀）；`MassiveString=true`（默认）给 helper 方法巨名。

**配置面**：`Chance=0.3` / `MetadataClass` / `ReobfBSM` / `MassiveString` / `GeneratedHelperNativeCandidate(+Ratio)` / `Exclusion`。

**判据分级**：S1——BSM 参数结构与 `@Metadata` 互证（`data1 % 59557 == magic2`）是语义后果：混淆器要保链接就必须让解密产物与调用点吻合；还原判据按 BSM 参数结构 + 注解值直接解密（key/magic 全部静态在场）。

**本仓现状**：预留。`grunt.IndyFold`（S3，libjsim 模拟 BSM 求真目标）按通用模拟面预留，BoogerV2 语料实测 foldable=0；按 BSM 结构直接解密 + `@Metadata` 注解解密的还原面未建（缺口清单 P1，morpher obf-forms §3.4 已给改写口径）。Kasumi 的 `IndyDecode`（8 参 BSM 指纹）与本行为互斥。

**样例**（simple/heavy 站点 + 双 BSM 骨架 + `@Metadata` 注解）：

```jasm
.visible-annotation Metadata {
    mv: 100,                                  // 判据锚点（heavy 元数据）：mv 版本 +
    d1: { "\u00e9\u4e2d", "\u4e2d\u00e9" },   //   d1 自证槽（data1 % 59557 ==
    d2: { "\u00e9\u4e2d", "\u4e2d\u00e9" }    //   magic2）+ d2 真载荷密文
}
.super java/lang/Object
.class public super GtRefIndy {
    .method public static simpleSite ()J {
        code: {
            ldc 41L
            invokedynamic s (J)J { invokestatic, GtRefIndy.bsmSimple, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; } { "\u00e9\u4e2d", "\u4e2d\u00e9", "\u00e9\u00e9", 0 }
                                              // 判据锚点（simple 7 参）：静态参数
                                              //   = [encOwner, encName, encDesc,
                                              //   isVirtual]，真目标三件套在参数里
            lreturn
        }
    }

    .method public static heavySite ()Ljava/lang/String; {
        code: {
            aconst_null
            ldc "arg"
            invokedynamic h (Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String; { invokestatic, GtRefIndy.bsmHeavy, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/Integer;Ljava/lang/Integer;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; } { "\u00e9\u4e2d", 4182, 20786, 1 }
                                              // 判据锚点（heavy 7 参）：静态参数
                                              //   = [encData, magic1, magic2,
                                              //   isVirtual]，真目标在 @Metadata
            areturn
        }
    }

    .method private static synthetic bridge bsmSimple (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; {
        code: {                               // simple BSM（PUBLIC+STATIC+SYNTHETIC
            aload 3                           //   +BRIDGE；真实体含 try/catch + 解
            pop                               //   密三段 + findStatic/findVirtual +
            aconst_null                       //   ConstantCallSite，判据读 desc +
            areturn                           //   静态参数，不依赖体）
        }
    }

    .method private static synthetic bridge bsmHeavy (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/Integer;Ljava/lang/Integer;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; {
        code: {                               // heavy BSM（PRIVATE+STATIC+SYNTHETIC
            aload 3                           //   +BRIDGE）：运行时反射读 @Metadata
            ldc 207868107                     //   → 解密真目标（骨架示意：encData
            invokestatic GtRefIndy.dx (Ljava/lang/String;I)Ljava/lang/String;
            pop                               //   经 generalKey 解一层）
            aconst_null
            areturn
        }
    }

    .method static dx (Ljava/lang/String;I)Ljava/lang/String; {
        code: {
            aconst_null
            areturn
        }
    }
}
```

## 4 结构与杂项面

### 4.1 DeclaredFieldsExtract：常量字段显式化（仅 static）

v2 DeclareFields 的 v3 对应物，差异一点：v3 只处理 **static** 字段（`ConstantValue` 搬进 `<clinit>` 头部显式写，origin/grunt3 收紧了实例字段路径）。判据 S1（初始化器单次常量写 + 全容器无其他读写 → 回填 `ConstantValue`）、本仓已处理（`grunt.ConstValuePromote` 精确逆），行为与样例沿用 v2 文档 §4.2（static 形态），不重复。

### 4.2 ParameterObfuscate：私有方法参数宽化

**做什么**。私有方法的引用型参数类型宽化为 `Object`（desc 同步改写，调用点同步），方法头插 `CHECKCAST` 恢复真实类型。`OnlyPrivateMethod=true`（默认，注释标明对全量方法不稳定）。

**判据分级**：S2——宽化 + 头部 cast 是结构事实（私有方法 desc 的参数全 Object + 头部直线 cast 链）；还原 = desc 收窄 + 删 cast，语义恒等。

**本仓现状**：无处理（缺口清单 P3）。

**样例**：

```jasm
.super java/lang/Object
.class public super GtParam {
    .method private static biz (Ljava/lang/Object;I)Ljava/lang/String; {
        code: {
            aload 0                           // 判据锚点：引用参数宽化 Object +
            checkcast java/lang/String       //   方法头 CHECKCAST 恢复（直线 cast
            astore 2                         //   链可删、desc 可收窄）
            aload 2
            iload 1
            invokevirtual java/lang/String.repeat (I)Ljava/lang/String;
            areturn
        }
    }

    .method public static call ()Ljava/lang/String; {
        code: {
            ldc "ab"                          // 调用点 desc 同步宽化
            iconst_2
            invokestatic GtParam.biz (Ljava/lang/Object;I)Ljava/lang/String;
            areturn
        }
    }
}
```

### 4.3 TrashClassGenerator：provider 化垃圾类生成

v2 TrashClass 的 v3 对应物，重做为 provider 体系（`TrashClassProviderRegistry`，`Provider` + `ProviderOptions` 可插拔）：`ClassCount=32`，`Packages`（`MirrorInputPackages` 可镜像输入包名）、`ClassPrefix=Trash`、`MemberPrefix=t`、字典可选；每类随机 `Min/MaxFields(1~4)`、`Min/MaxMethods(2~6)`、`Min/MaxMethodSteps(2~8)`、`StringMethodChance=0.25` 混入字符串方法、`GeneratedMarkers=true` 带生成标记。判据 S2（零引用 + provider 形状），本仓无处理——垃圾类删除面（引用普查兜底）是缺口清单 P3，与 v2 垃圾类两式同一缺口。

### 4.4 FakeSyntheticBridge / Watermark / DecompilerCrasher / ShuffleMembers / ReflectionSupport

- **FakeSyntheticBridge**：与 v2 SyntheticBridge 同构（类级 `synthetic` + 方法 `bridge synthetic` 对；v3 无 v2 的 `FlattenMethods` 反向开关）。已处理（`grunt.FlagsStrip`），形状样例沿用 v2 文档 §4.3 所引 morpher obf-forms §3.8。
- **Watermark**：渠道收敛为两个——`FieldMark`（`private static` 水印字段）与 `MethodMark`（`LDC marker; ARETURN` 两指令方法），默认 names `Grunt/Gruntpocalypse/Grunteon`（v2 的注解/接口/自定义体渠道在 v3 无）。判据 S2，无处理（缺口清单 P3，可与 Kasumi `MetadataStrip` 水印面同构扩家族判据）。
- **DecompilerCrasher**：与 v2 Crasher 同面（Signature 属性 massive 串），S2，无处理（缺口清单 P3）。
- **ShuffleMembers**：成员/注解/异常表顺序打乱，清理面，登记即可。
- **ReflectionSupport**：改名后反射串重映射，伴随面，产物无独立形态。

## 5 Native 面（检测-only / 不可静态还原）

### 5.1 NativeCandidate：native 候选标注

按 `Rules`（`NativeCandidateScanner` 消费的规则列表）匹配方法，挂 `Annotation`（默认 `NATIVE_INCLUDED`）候选注解，`LogMarkedMethods` + `LogLimit=256` 输出清单。本身不改行为，是 NativePipeline 的选取面。检测-only：本仓无处理，识别 = 注解 + 规则形状计数。

### 5.2 NativePreProcessor：动态特性桥接

把 JVM 动态特性调用点桥接进同类 helper 方法，给 native 代码生成一个统一调用面：`invokedynamic` 调用点 → `INVOKESTATIC __grt$indy$N`（helper 内持一个裸 3 参 BSM 的 indy）、constant dynamic → `__grt$condy$N`、`MethodHandle.invoke/invokeExact` 调用 → `__grt$mh$N`；`ProcessGeneratedMethods=true` 让 Grunteon 自己生成的 helper 也参与桥接。判据 S2（`__grt$` 前缀 + 裸 BSM 桥接是结构事实），本仓无独立 residue（伴随面，native 前置）。

**样例**（v3-10：调用点经 helper 桥接）：

```jasm
.super java/lang/Object
.class public super GtPrep {
    .method public static site (I)I {
        code: {
            iload 0                           // 判据锚点：调用点被桥进 helper
            invokestatic GtPrep.__grt$indy$0 (I)I
            ireturn
        }
    }

    .method private static synthetic __grt$indy$0 (I)I {
        code: {
            iload 0                           // helper 内持裸 3 参 BSM 的 indy
            invokedynamic f (I)I { invokestatic, GtPrep.bsm, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }
            ireturn
        }
    }

    .method private static synthetic bsm (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; {
        code: {
            aconst_null                       // 裸 BSM（无静态参数）——native 阶段
            areturn                           //   会把它替换成 native 解析
        }
    }
}
```

### 5.3 NativePipeline（NativeObfuscation 阶段）：方法体 native 化

**做什么**。独立执行阶段（非 transformer）：`NativePipelineRunner` 串 `NativeCandidateScanner`（收集候选注解方法）→ `NativeCompiler`/`NativeCppBackend`（经 `NativeJvmCppMethodTranslator` / `NativeSsaDirectTranslator`（SSA IR 直译）/ `NativeJvmExceptionDispatchPlanner` 把方法体译成 C++）→ `NativeCommitter`（`makeNative`：方法挂 `ACC_NATIVE` 并清空字节码体；`<clinit>` 改写为调 `registerNativesForClass` 的代理方法并挂 `LambdaForm$Hidden` + `jdk/internal/vm/annotation/Hidden`；接口类 clinit 与接口方法有专门重写路径）→ `NativeLoaderClassFactory`（生成 loader 类：native `registerNativesForClass(I,Class)V` + `private static volatile boolean loaded` + `normalizeOs`/`normalizeArch` + `selectNativeLibraryResource` 按平台选路径 + `load()` 幂等加载）→ 库以 jar 资源嵌入（`/grunteon/native/<os>/<arch>/grunteon.{dll,so,dylib}` 形路径）。

**如实标注**：**检测-only / 不可静态还原**——方法体已离开字节码域（`ACC_NATIVE` + 空体），静态还原无对象；本仓可做的是识别与计数（native 方法 + loader 类形状 + 库资源路径），行为重建只能反编译/运行库本体。形态还原 = 识别三类锚点（`ACC_NATIVE` 空体方法、loader 类形状、库资源），登记为缺口清单的检测-only 项。

**样例**（native 目标类 + loader 类，两文件）：

```jasm
// native 目标类：方法 ACC_NATIVE 空体 + 原调用面透传
.super java/lang/Object
.class public super GtNative {
    .method public static native biz (I)I {
    }                                     // 判据锚点：ACC_NATIVE + 空方法体
                                          //   （体已进 C++ 库，字节码域无对象）

    .method public static bizStub (I)I {
        code: {                           // 透传桩（真实产物中原调用点保留）
            iload 0
            invokestatic GtNative.biz (I)I
            ireturn
        }
    }
}
```

```jasm
// loader 类：平台选择 + 幂等加载 + registerNatives
.super java/lang/Object
.class public final super GtNativeLoader {
    .field private static volatile loaded Z

    .method public static native registerNativesForClass (ILjava/lang/Class;)V {
    }                                     // 判据锚点：loader 的 native 注册入口

    .method private <init> ()V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            return
        }
    }

    .method private static selectResource ()Ljava/lang/String; {
        code: {
            ldc "/grunteon/native/windows/x64/grunteon.dll"
            areturn                          // 判据锚点：库资源路径形
        }                                    //   /grunteon/native/<os>/<arch>/…
    }

    .method public static load ()V {
        code: {
            getstatic GtNativeLoader.loaded Z   // 幂等门（volatile loaded）
            ifne Ldone
            iconst_1
            putstatic GtNativeLoader.loaded Z
            invokestatic GtNativeLoader.selectResource ()Ljava/lang/String;
            invokestatic GtNativeLoader.load0 (Ljava/lang/String;)V
        Ldone:
            return
        }
    }

    .method private static load0 (Ljava/lang/String;)V {
        code: {
            aload 0
            invokestatic java/lang/System.load (Ljava/lang/String;)V
            return
        }
    }
}
```

## 6 反调试面

### 6.1 RuntimeMaterial：分布式材料扰动

**做什么**。给类注入运行时完整性材料：per-class 四个静态字段（源注释口径：`shareA`/`shareB`(long) + `canonicalKey`(long) + `poison`(int)），`<clinit>` 注入类级扰动（`Clinit=true`）、`<init>` 注入实例路径扰动（`Constructors=true`，`ConstructorLimit=8`）。clean 扰动让 `shareA += delta; shareB -= delta`（原始和在等价类内滑动，canonical 不变）；可疑扰动（`DetectSources`/`DetectTokens` 检测调试器/篡改）置 sticky poison 并扰动 canonicalKey；canonical 操作符为 `CANONICAL_SHARE_SUM_STICKY_POISON`（canonicalKey = shareA + shareB 与 poison 的组合）。`DraftMaterialMetadata=true` 给 ReferenceObfuscate 输出材料草稿（canonicalKey + poison 作 heavy BSM 的 KDF 输入——两 transformer 联动）。判据 S2（材料字段组 + clinit/init 扰动形状），本仓无处理（缺口清单 P3，检测/摘除面）。

**样例**（字段以 int 简化示意；真实字段形为 long shareA/shareB + canonicalKey + int poison）：

```jasm
.super java/lang/Object
.class public super GtMaterial {
    .field static rmA I
    .field static rmB I
    .field static rmKey I
    .field static rmPoison I

    .method static <clinit> ()V {
        code: {                               // 判据锚点：材料字段组 + clinit
            ldc 1708157141                    //   初始化（shareA/shareB 初始值 +
            putstatic GtMaterial.rmA I        //   canonicalKey + poison 置零）
            ldc -1024
            putstatic GtMaterial.rmB I
            ldc 20161186
            putstatic GtMaterial.rmKey I
            iconst_0
            putstatic GtMaterial.rmPoison I
            return
        }
    }

    .method public <init> ()V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            invokestatic GtMaterial.rmSample ()V   // 实例路径扰动挂钩
            return
        }
    }

    .method private static rmSample ()V {
        code: {
            invokestatic GtMaterial.rmKeyOf ()I   // 扰动读取（骨架）
            pop
            return
        }
    }

    .method static rmKeyOf ()I {
        code: {
            getstatic GtMaterial.rmKey I          // canonical 读取：key 组合
            getstatic GtMaterial.rmPoison I       //   poison（真实形为
            iadd                                  //   shareA+shareB+poison 组合）
            ireturn
        }
    }

    .method public static biz ()I {
        code: {
            bipush 7
            ireturn
        }
    }
}
```

### 6.2 AntiLLM：四渠道 LLM 诱饵

**做什么**。向产物注入面向 LLM/分析器的诱饵噪声，四渠道（配置逐项）：`SourceDebug=true`（`SourceDebugExtension` 追加 bait 块，`SourceDebugSize=1536` 字符/类）、`ClassAnnotations=true` + `MemberAnnotations=true`（不可见 AntiLLM 注解挂类与成员，`MaxAnnotatedMembersPerClass=6`）、`FakeResources=true`（生成假 markdown 分析资源，`ResourceCount=4`、`ResourceSize=4096` 字符）、`CarrierClasses=true`（生成无害 `public static` carrier 方法类，供 JunkCode 的 String 实参引用，`CarrierCount` 可配）。载荷池 `PoolSize=128`、单串上限 `MaxStringLength=384`、`UserPhrases`/`DefaultPhrases` 混入 LLM 敏感词、`FeedJunkCode=true` 把载荷串喂给 JunkCode 作加密/明文 `ldc` 候选。判据 S2（噪声内容与渠道形状），本仓无处理（缺口清单 P3 噪声清理面，与 Watermark/Crasher 同批）。

## 7 改名与清理面（要点账）

改名族（ClassRenamer / MethodRenamer / FieldRenamer / LocalVarRenamer / MixinClassRenamer / MixinFieldRenamer）：字典/前缀/`U+200E` 尾缀口径与 v2 家族同源（origin/grunt3 修了 exclusions 边界）；MappingApplier 走外部 mapping 应用（`MappingSource`/`NameMapping` infra）替代生成式改名。**本仓现状：无处理（改名还原走 mapping 旁证，不在 residue 面）**。清理族：MethodInliner（同类小方法内联，真优化）、ClassShrink（删 InnerClasses/未用标签/NOP/方法 Signature）、KotlinClassShrink（删 Kotlin Metadata + Intrinsics 检查）、DeadCodeRemove（删 load+POP 族无用序列）、EnumOptimize（values() 摘 clone+CHECKCAST）、SourceDebugInfoHide（删 source file/行号）、PostProcess（Manifest/YML/JSON 主类重映射 + 收尾清理）、ShuffleMembers（成员序打乱）——全部清理面，还原侧无动作，如实登记。StringEqualsOptimize（`equals`/`equalsIgnoreCase` 换比较，`IgnoreCase` 开关）判据 S2、本仓无处理（缺口清单 P3，与 v2 StringEqualsRedirect 同面）。

## 8 插件与基础设施面（要点账）

- **GlslObfuscator（grunt-glsl 插件，Category.Other）**：GLSL shader 资源混淆（非字节码面）——路径匹配（`**.glsl/.vert/.frag/...`）、`#include` 解析（`IncludeRoots`，`FailOnMissingIncludes` 可选 fail）、改名（locals/parameters/private functions，`PreserveNames` 保 `main`/`gl_*`）、内联（`InlineMaxStatements=8`、`InlineMaxCallSitesPerFunction=16`、`InlineMaxExpansionRatio=1.5`、全内联私有函数删除）——由 `GlslLexer/Parser/Analysis/InlinePass/RenamePass/PatchWriter` 一串 pass 执行，产 GLSL 文本改写。本仓无处理（登记：资源混淆面，字节码 residue 不适用）。
- **grunt-ir**：Flow IR 与 SSA IR 两套中间表示及 JVM 导入/导出/验证器——§2.2/§2.3 的摊平引擎与 §2.4 往返 transformer 的底座，本身不直接产 obf 形态。
- **hierarchy 面**：`ClassHierarchy`/`FieldHierarchy`/`MethodHierarchy` + `ClassHierarchyFlowTypeHierarchy`——CFF `FlowAnalyzer=Hierarchy` 模式与改名一致性检查消费。
- **grunt-index / grunt-backend / grunt-bootstrap**：类索引库 / 作业调度 Web 服务 / LaunchClassLoader 运行壳——基础设施，不产 obf 形态，登记不展开。

## 9 v3 行为 → residue 映射速查

```text
已处理：DeclaredFieldsExtract→ConstValuePromote、NumberBasicEncrypt→NumberFold（全覆盖）、
        InvokeProxy→ProxyInline、FieldAccessProxy→FieldAccessProxy、
        FakeSyntheticBridge→FlagsStrip
部分  ：ControlflowJump→TrampolineSqueeze/PredicateFold/NumberFold/DeadCode
        （processor 调用门、异常桥缺口）
预留  ：ReferenceObfuscate→IndyFold（S3，语料 foldable=0）
无处理：StringArrayedEncrypt、InvokeDispatcher、ControlflowFlattening（含 SSA）、
        ArithmeticSubstitute、StringEqualsOptimize、ParameterObfuscate、
        TrashClassGenerator、RuntimeMaterial、AntiLLM、Watermark、DecompilerCrasher、
        垃圾类、改名族
伴随面：FlowIRRoundTrip、SSARoundTrip、NativePreProcessor、ReflectionSupport、MappingApplier
检测-only：NativeCandidate、NativePipeline（不可静态还原）
清理面：MethodInliner、ClassShrink、KotlinClassShrink、DeadCodeRemove、EnumOptimize、
        SourceDebugInfoHide、ShuffleMembers、LocalVarRenamer、PostProcess
资源面：GlslObfuscator（GLSL，非字节码）
```
