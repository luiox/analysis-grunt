# Grunt v2（Gruntpocalypse 2.x）obf 行为核心描述

> 适用对象：Grunt 混淆家族 v2 代（`grunt2` 分支，tag 线 2.0.0.240702 ~ 2.4.5.250307）。本仓知识源锚定 `grunt2` 分支 HEAD `36c0eef`（Merge PR #62，feat-betterconfig）。
> 知识来源：Grunt 源码仓（只读）`grunt2` 分支 `Transformers.kt` 注册表与 35 个 transformer 逐个源码盘点；`analysis-grunt` 仓现有 residue 面做处理现状对照。
> jasm 样例为按行为描述自写的完整可汇编样例，全部经 mjt-py SDK `class_validate_jasm`（libjhasm 汇编 + 校验门）逐类验证；判据锚点以行内注释标注，样例中的类名/方法名/密文与常量数值只作形状示意，不是判据。已知校验器边界如实注明（§0.3）。
> 与 morpher 仓 `mjt-deobf/doc_ai/obf-forms/grunt-obfuscation-forms.md` 的分工：那边是**跨家族形态库**（artifact 侧形态判据与还原链序），本文是**本工具行为账**（v2 逐 transformer 行为 + 配置面 + 本仓 residue 现状 + 闭环缺口），同面判据以那边为准、不重复定义。
> 排版：正文一句话/一段落一整行，不做 80 列硬断；代码块与表格内自然换行。
> 日期：2026-11-06。

---

## 0.1 家族底座

v2 是 ASM `ClassNode` 体系上的 transformer 管线：`grunt-main` 装载 jar 进 `ResourceCache`，按 `Transformers.kt` 注册表的 order 升序逐个跑，最终 `JarDumper` 写回。执行序分十档：0000 优化 → 0100 杂项一 → 0200/0400 控制流（加密前/后两个可选槽位）→ 0300/0500 加密 → 0600 重定向 → 0700 杂项二 → 0800 改名 → 0900 Mixin → 1000 indy 与杂项三 → MAX 后处理。

配置体系是 `Configs.kt` 的 `setting(...)` 声明（GUI / JSON 双面），每个 transformer 自带 `Exclusion`。全局开关有 parallel、dictionaryStartIndex 等。排除注解四个：`@DisableControlflow` / `@DisableInvokeDynamic` / `@DisableScramble` / junk-call 排除注解，PostProcess 收尾统一摘除。

**Era 现状（重要）**：本仓 `src/grunt/knowledge/era.hpp` 的 `probeEra` 目前只有 Kasumi 有可靠判据（命名空间 / 8 参 BSM 指纹 / `@Metadata` 注解三选一），**Grunt2 无可靠判据**——`U+200E` 尾缀两代都在（v2 `ClassRenameTransformer::suffix` 与 v3 `FieldRenamer::suffix` 同源），调用点模板与数字混淆形状两代间没有观测到稳定差异。v2 行为判据当前一律按 S1/S2 通用语义事实写、不挂 Era 门；等真正版本敏感的残留出现再补探测规则。

## 0.2 行为清单总表（35 个）

登记序号 = `Transformers.kt` 的 order。等级 = 还原侧判据分级（S1 语义后果 / S2 结构事实 / S3 预留面），清理面行为无还原判据不评级。现状列：**已处理** = 有 residue 消费其形态；**部分** = 形态的一个子集有面；**预留** = 判据按形态写好但语料无站点（S3）；**无处理** = 还原缺口（进闭环缺口清单）；**清理面** = 混淆器删信息/瘦身，无还原需求；**伴随面** = 服务于其他行为的中间动作；**检测-only** = 运行时面，静态只做识别与计数。

| # | 行为 | 一句话 | 等级 | 本仓现状 |
|---|---|---|---|---|
| 1 | SourceDebugRemove | 摘 SourceFile / SourceDebugExtension / 行号 | 清理面 | 清理面 |
| 2 | KotlinOptimize | 摘 Kotlin Intrinsics 检查与 Metadata 注解 | 清理面 | 清理面 |
| 3 | EnumOptimize | 摘 `values()` 的 clone + CHECKCAST | 清理面 | 清理面 |
| 4 | DeadCodeRemove | 删 load+POP 对 | 清理面 | 清理面 |
| 5 | MethodExtractor | 实例方法体搬进 `$Methods` 伴生类 | S2 | 无处理 |
| 6 | ClonedClass | 克隆随机类当垃圾类 | S2 | 无处理 |
| 7 | TrashClass | 生成并发容器子类垃圾类 | S2 | 无处理 |
| 8 | HWIDAuthenticator | 注入 HWID 校验类与验证调用 | 检测-only | 无处理 |
| 9 | DeclareFields | `ConstantValue` 搬进 `<clinit>`/`<init>` 写 | S1 | 已处理（const_value_promote 正向对） |
| 10 | ReflectionSupport | 改名后重映射反射串、收集黑名单 | 伴随面 | 伴随面 |
| 11 | StringEncrypt | `String[]` 池 + 解密器 + 调用点池读 | S1 | 无处理 |
| 12 | NumberEncrypt | 常量藏进等价运算跨度（classic 三式 + 位形包装 + arrayed 池） | S1 | 部分（NumberFold 覆盖 XOR 走廊/基 32/位形包装；IAND·LAND 位拼模板与 arrayed 池无面） |
| 13 | ArithmeticEncrypt | 算术指令换恒等替换序列 | S2 | 无处理 |
| 14 | Controlflow | 六步控制流混淆（假跳转/mangled if/switch 变体/junk） | S1/S2 | 部分（TrampolineSqueeze / PredicateFold / NumberFold / DeadCode / SeedStrip 处理静态可裁决子集） |
| 15 | ConstBuilder | int 常量换成 switch 常量构造器 | S1 | 无处理 |
| 16 | ConstPoolEncrypt | 常量搬进 `$ConstantPool` 伴生类 | S1 | 部分（伴生类 clinit 解密跨度可折可回填；调用点 GETSTATIC 回填无面） |
| 17 | StringEqualsRedirect | `equals` 换 hashCode 比较 | S2 | 无处理 |
| 18 | FieldScramble | 字段访问换 static getter/setter 代理 | S2 | 已处理（FieldAccessProxy） |
| 19 | MethodScramble | 方法调用换 static 转发代理 | S2 | 已处理（ProxyInline） |
| 20 | NativeCandidate | 给方法挂 native 候选注解（外部管线消费） | 检测-only | 无处理 |
| 21 | SyntheticBridge | 伪 `ACC_SYNTHETIC`/`ACC_BRIDGE` 撒标志 | S2 | 已处理（FlagsStrip） |
| 22 | LocalVariableRename | 局部变量改名/删表 | 清理面 | 清理面 |
| 23 | MethodRename / MethodRenameKr | 方法改名（Kr=谚文字典变体） | 清理面 | 无处理（改名面） |
| 24 | FieldRename / FieldRenameKr | 字段改名（Kr 变体同上） | 清理面 | 无处理（改名面） |
| 25 | ClassRename | 类改名（含 zip 条目名破坏） | 清理面 | 无处理（改名面） |
| 26 | MixinFieldRename / MixinClassRename | Mixin 感知改名 | 清理面 | 无处理（改名面） |
| 27 | InvokeDynamic | 调用点 indy 化（simple/heavy 双 BSM） | S1 | 部分 | IndyRestore（simple 直接解密回填，缺口 #2 闭环；heavy 待 Era::Grunt2 判据按保守口径关闭，material 类 fail-closed）+ IndyFold（S3 兜底） |
| 28 | ShuffleMembers | 打乱成员/注解/异常表顺序 | 清理面 | 清理面 |
| 29 | Crasher | Signature 属性填 massive 串砸反编译器 | S2 | 无处理 |
| 30 | Watermark | 水印字段/方法/注解/接口 | S2 | 无处理 |
| 31 | Shrinking | 删 InnerClasses/未用标签/NOP/方法 Signature/指定注解 | 清理面 | 清理面 |
| 32 | PostProcess | 删内部注解与 dummy 指令、修 manifest/plugin.yml 主类 | 清理面 | 清理面 |

## 0.3 样例验证口径

全部样例经 mjt-py SDK `class_validate_jasm` 逐类验证：690 字节占位 jar 中的占位类经 token 改名承载各样例目标类名（目标类必须存在），试汇编与校验门在容器深拷贝上进行。v2 16 个样例文件全部通过——15 个过 relaxed 语义校验档，1 个（string-pool）因校验器对 `aaload` 家族的已知误报走 `validation=off` 通道；连同 v3 文档的 11 个样例合计 27/27（25 relaxed + 2 off）。另两处校验器边界同样如实记录：

1. **`xaload` 族误报**：relaxed 校验器对 `aaload`/`iaload`/`aastore`/`castore` 的栈型判定存在误报（「array load with non-reference operand」「argument is not assignable to [C」），canonical 汇编本身正确（arrayref, index 的 JVM 标准栈序）。命中误报的样例以 `validation=off` 过汇编，形态按 JVM 语义手工核对。
2. **`<init>` 内 super 调用后的 `aload 0`**：libjhasm 栈帧分析器在「super 调用后再 `aload 0` + `putfield`」的合法 JVM 形状上报栈下溢（uninitializedThis 迁移处理缺陷）。样例规避该形状（实例字段写法以文字描述），不影响形态结论。

---

## 1 加密面

### 1.1 StringEncrypt（order 300）：字符串池加密

**做什么**。逐类收集全部非空字符串 `ldc`（反射黑名单串跳过、重复串共享槽位），生成三件套：`String[]` 池字段（随机名，接口类挂 `PUBLIC+FINAL`）、随机名解密方法、随机名 `arrayInit` 静态方法。`<clinit>` 头部插入池分配前导 `{池长; ANEWARRAY String; PUTSTATIC; INVOKESTATIC arrayInit}`；`arrayInit` 逐项把密文源、per-string `seed(long)` 与 `key(int)` 压栈调解密器后 `AASTORE`；调用点原地换 `GETSTATIC pool; <const idx>; AALOAD`。加密算法：`classKey(每类一个随机 int) ^ key ^ seed` 驱动的 char 级 XOR 流（字符逐个异或、内部状态随 `i & 0xFF` 旋转），解密器体是带循环的 char 数组处理——**不在**纯算术求值子集内，`ConstantCall` 吃不下。开关 `Arrayed`（密文以 `char[]` 直存，`NEWARRAY T_CHAR` 逐码元写入，免 toCharArray）；`ReplaceInvokeDynamics`（`makeConcatWithConstants` indy 先替换成 `ldc` + 每类一个 concat wrapper 方法，随后一并进池）。类版本 < V1_5 跳过。

**配置面**：`Arrayed=false` / `ReplaceInvokeDynamics=true` / `Exclusion`。

**判据分级**：S1——`owner.field[index] → 明文` 映射编译期可建（seed/key/classKey 全是装载期常量、解密器纯函数、store 序列固定），读取点换 `ldc` 明文是语义恒等；换代表现在加密算法内部（判据不读算法细节，只读池结构 + 调用点三元组）。

**本仓现状**：已处理——`grunt.StringPoolFold`（缺口 #1 闭环）：池读三元组折回 `ldc` 明文，池链三件套结构自证 + 自逆 XOR 流静态复算（见 doc/string-pool-fold.md）。Grunteon 全链产物把 arrayInit 调用藏进 indy 蹦床，该形态依赖上游 indy 还原（缺口 #2）接通。池读三元组形状判据在 morpher obf-forms §3.1 已定案。

**样例**（v2 literal 密文形态，`toCharArray` 进解密器，desc `([CJI)Ljava/lang/String;`）：

```jasm
.super java/lang/Object
.class public super GtStringPool {
    .field private static pool [Ljava/lang/String;

    .method static <clinit> ()V {
        code: {
            iconst_2                          // 池分配前导四件套（判据锚点）：
            anewarray java/lang/String        //   {池长; ANEWARRAY String;
            putstatic GtStringPool.pool [Ljava/lang/String;
                                              //   PUTSTATIC; INVOKESTATIC arrayInit}
            invokestatic GtStringPool.arrayInit ()V
            return
        }
    }

    .method private static arrayInit ()V {
        code: {                               // 逐项 store：GETSTATIC; idx; 密文源;
            getstatic GtStringPool.pool [Ljava/lang/String;
                                              //   seed(long); key(int); INVOKESTATIC
                                              //   dec ([CJI)String → AASTORE
            iconst_0
            ldc "\u00e9\u4e2d"                // 密文串（非 arrayed 形态走 toCharArray）
            invokevirtual java/lang/String.toCharArray ()[C
            ldc 889226191L                    // per-string seed
            ldc 17202                         // per-string key
            invokestatic GtStringPool.dec ([CJI)Ljava/lang/String;
            aastore
            getstatic GtStringPool.pool [Ljava/lang/String;
            iconst_1
            ldc "\u00e9\u4e2d"
            invokevirtual java/lang/String.toCharArray ()[C
            ldc 889226192L
            ldc 17203
            invokestatic GtStringPool.dec ([CJI)Ljava/lang/String;
            aastore
            return
        }
    }

    .method private static dec ([CJI)Ljava/lang/String; {
        code: {                               // 解密器 desc 固定 ([CJI)String；
            aconst_null                       //   体是带循环的 char XOR 流（骨架
            areturn                           //   示意，判据不依赖算法细节）
        }
    }

    .method public static read ()Ljava/lang/String; {
        code: {
            getstatic GtStringPool.pool [Ljava/lang/String;
                                              // 判据锚点（读取点三元组）：GETSTATIC
            iconst_0                          //   钉 String[] 描述符 + 紧邻常量
            aaload                            //   下标 + AALOAD → 折回 ldc 明文
            areturn
        }
    }
}
```

### 1.2 NumberEncrypt（order 301）：数值藏形

**做什么**。把方法体里的数值常量装载（`ICONST_*` / `IntInsnNode` / 数值 `LDC`，`NEWARRAY` 操作数除外）换成等价运算跨度。classic 加密器四式：

- int 长走廊：`<INT neg>; I2L; <INT obf>; I2L; LXOR; L2I`（neg ^ obf = 原值，符号扩展下低 32 位等价）；
- int 短走廊：`<LDC neg.toLong()>; L2I; <INT obf>; IXOR`；
- int 位拼对：`<first>; <second>; IAND`，`first = rnd1 & mask | v`、`second = rnd2 & ~mask | v`（`first & second = v`）；
- long 位拼对：同构 `LAND`；float/double 在整型跨度外包 `Float.intBitsToFloat` / `Double.longBitsToDouble` 位形换算。

`Intensity`（1~N 轮重复加密，输出会套娃）。`Arrayed` 开关：值收集进每类一个 `long[]` 池字段，调用点换 `GETSTATIC pool; idx; LALOAD`（float/double 加位形换算），池在 `<clinit>` 里由一次性解密方法整体填充（外层 key 经 classic 跨度加密）。另有 `MaxInsnSize` 防超限、`FloatingPoint` 开关。

**判据分级**：S1——跨度纯常量封闭求值（base-32 严格解析、XOR 封闭、位形换算 JDK 语义确定），折回单条常量装载是精确逆。

**本仓现状**：部分。`NumberFold` 覆盖长走廊、短走廊（`I2L..LXOR..L2I` / `L2I..IXOR`）、基 32 long、位形包装；**缺口**：int/long 的 IAND·LAND 位拼模板（跨度算子是 IAND/LAND，不在 NumberFold 词汇表）、arrayed 池形态。缺口清单 P2。

**样例**（五式合并呈现；常量按编码关系可逆，`17202 ^ 16602 = 1000`、`4649 & 10485803 = 41`）：

```jasm
.super java/lang/Object
.class public super GtNumberClassic {
    .method public static intCorridor ()I {
        code: {
            ldc 17202                         // int 长走廊 1/6：INT(neg)
            i2l                               // 2/6：I2L（升 long 域，判据锚点）
            ldc 16602                         // 3/6：INT(obf)
            i2l                               // 4/6：I2L
            lxor                              // 5/6：LXOR（低 32 位等价）
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

    .method public static intAndPair ()I {
        code: {
            ldc 4649                          // 位拼对 1/3：first = rnd&mask|v
            ldc 10485803                      // 2/3：second = rnd&~mask|v
            iand                              // 3/3：判据锚点：IAND 对 → = ldc 41
                                              //   （NumberFold 现词汇表不认 IAND——缺口）
            ireturn
        }
    }

    .method public static longAndPair ()J {
        code: {
            ldc 69525319247532032L            // long 位拼对 1/3
            ldc 1000542113792L                // 2/3
            land                              // 3/3：→ = ldc 1000000000000L
            lreturn
        }
    }

    .method public static floatBits ()F {
        code: {
            ldc 17202                         // 位形包装 1/7：int 域走廊
            i2l
            ldc 1069564722                    // 4/7：INT(bits ^ neg)
            i2l
            lxor                              // → 1069547520 = 1.5F 的 IEEE 位形
            l2i
            invokestatic java/lang/Float.intBitsToFloat (I)F
                                              // 7/7：判据锚点——位形换算后按
                                              //   float 常量发 ldc（NumberFold 已覆盖）
            freturn
        }
    }
}
```

### 1.3 ArithmeticEncrypt（order 302）：算术恒等替换

**做什么**。把二元整算术指令换成更长的恒等序列：`IXOR` 三种替换（如 `DUP2; IOR; DUP_X2; POP; IAND; ISUB`）、`IAND`/`IOR`/`INEG` 各有固定替换（`INEG → ICONST_M1; IXOR; ICONST_1; IADD` 等），另对 `ICONST_M1; IXOR; IAND`（补码与）与 `ISUB; ICONST_M1; IXOR`（减法补数）两个三元组整体替换。重复 `Intensity` 轮可嵌套生长。`Percentage` 控制替换率、`MaxInsnSize` 防超限。与 v3 的 `ArithmeticSubstitute` 同构（v3 标记 deprecated + Hidden）。

**判据分级**：S2——替换序列是封闭 int 算术恒等式，简化链可折叠；形状换代可换（是混淆器的实现选择面）。

**本仓现状**：无处理（缺口清单 P2；morpher obf-forms §3.2 末尾「同族 ArithmeticSubstitute 走通用简化链折叠」口径）。注意：v2 Controlflow 的 `ArithmeticExprBuilder` 生成面（`ArithmeticExpr.kt`）产生同族表达式，还原面同一。

### 1.4 ConstPoolEncrypt（order 501）：常量池伴生类

**做什么**。给每个类建一个 `<Class>$ConstantPool` 伴生类（`PUBLIC+FINAL`，默认挂排除 scramble 注解），把方法体里的 `ICONST_*` / 数值与字符串 `LDC` 换成 `GETSTATIC 伴生类.<随机字段>`；伴生类字段在自身 `<clinit>` 里回填——数值走 classic 跨度加密、字符串走 `LDC(encrypt(明文, key)); INVOKESTATIC (String;)String 单参解密器`（key 烧进解密器体）。开关按类型分（Integer/Long/Float/Double/String）、`HeavyEncrypt`（伴生类解密体再叠 NumberEncrypt/StringEncrypt）、`DontScramble`、`NativeAnnotation`（解密体标 native 候选，clinit 变 `INVOKESTATIC decrypt` 单调用）。

**判据分级**：S1——伴生类 clinit 的解密跨度可静态求值，`GETSTATIC → 常量` 回填语义恒等。

**本仓现状**：部分。伴生类 clinit 里的加密跨度可被 NumberFold 折成常量写、字段可被 ConstValuePromote 回填 `ConstantValue`（const_args 普查口径下伴生类是普通类）；**缺口**：调用点 `GETSTATIC → ldc 明文` 的回填 residue 未建（回填依赖解密链先行，缺口清单 P2，与缺口 #1 共链）。

**样例**（伴生类 + 调用点两类合并呈现）：

```jasm
// 伴生类（随机名 <Class>$ConstantPool）
.super java/lang/Object
.class public final GtConstPool$ConstantPool {
    .field public static s0 Ljava/lang/String;
    .field public static i0 I

    .method static <clinit> ()V {
        code: {
            ldc "\u00e9\u4e2d"                // 字符串回填 1/3：LDC(encrypt(明文, key))
            invokestatic GtConstPool$ConstantPool.dx (Ljava/lang/String;)Ljava/lang/String;
                                              // 2/3：判据锚点——单参 (String;)String
                                              //   解密器，key 以 LDC;IXOR 烧在体内
            putstatic GtConstPool$ConstantPool.s0 Ljava/lang/String;
            ldc 17202                         // 数值回填：classic XOR 走廊（S1 跨度）
            i2l
            ldc 16602
            i2l
            lxor
            l2i                               // → 1000
            putstatic GtConstPool$ConstantPool.i0 I
            return
        }
    }

    .method private static dx (Ljava/lang/String;)Ljava/lang/String; {
        code: {
            aload 0                           // 解密器体（骨架）：char 循环 + 
            invokevirtual java/lang/String.length ()I
            istore 1                          //   LDC key; IXOR（key 提取锚点）
            iconst_0
            istore 2
        Lloop:
            iload 2
            iload 1
            if_icmpge Ldone
            aload 0
            iload 2
            invokevirtual java/lang/String.charAt (I)C
            ldc 1337                          // 判据锚点：解密器体内 LDC int
            ixor                              //   + IXOR——key 就在这两条里
            i2c
            istore 3
            iinc 2 1
            goto Lloop
        Ldone:
            new java/lang/String
            dup
            invokespecial java/lang/String.<init> ()V
            areturn
        }
    }
}
```

```jasm
// 调用点（owner 类内，指令被换成 GETSTATIC）
.super java/lang/Object
.class public super GtConstPool {
    .method public static read ()I {
        code: {
            getstatic GtConstPool$ConstantPool.i0 I
                                              // 判据锚点：GETSTATIC 伴生类字段
                                              //   → 回填 ldc 明文（本仓缺口）
            ireturn
        }
    }

    .method public static name ()Ljava/lang/String; {
        code: {
            getstatic GtConstPool$ConstantPool.s0 Ljava/lang/String;
            areturn
        }
    }
}
```

### 1.5 ConstBuilder（order 500）：switch 常量构造器

**做什么**。按 `ReplacePercentage`（默认 10，防 method too large）把 int 常量装载换成 switch 构造器：常量本体先经 `xor()` 藏形（轻量形态两条 `GETSTATIC 数据类.<number_ 随机字段>; IXOR`，数据类字段是 `ConstantValue` 常量、常量池里看不到原值），算出的 key 进 `LookupSwitch`；命中 case 输出真值（再经 `xor()` 藏形），其余 case 输出错值（`v±1`、`v*2`、`0`、`1`、`v*5`、`v*10`）。`MaxCases` 控制假 case 数，`SplitLong` 把 long 拆高低 32 位各建一个构造器，`HeavyEncrypt` 加深 xor 层数，`SkipControlFlow` 对构造器体挂 dummy 防 Controlflow 二次混淆。

**判据分级**：S1——switch 全表编译期可静态求值（操作数全为 `GETSTATIC ConstantValue` 字段 + 封闭 IXOR），真 case 的产出即常量；调用点整段可折回单条 `ldc`。

**本仓现状**：无处理（缺口清单 P2）。注意其数据类的 `number_*` 常量字段是 ConstValuePromote 的正向形状（若数据类 clinit 有显式写则可回填；ConstBuilder 直接挂 ConstantValue 属性，无需回填）。

**样例**（switch 主形 + 数据伴生类，两类分列）：

```jasm
.super java/lang/Object
.class public super GtConstBuilder {
    .method public static pick ()I {
        code: {
            getstatic GtConstBuilder$Data.k0 I    // xor(key, true) 三元组 1/6：
            getstatic GtConstBuilder$Data.k1 I    //   GETSTATIC 池 + IXOR 逐层
            ixor                                  // 3/6
            getstatic GtConstBuilder$Data.k2 I
            ixor                                  // 5/6 → key = 100（判据锚点：
                                                  //   操作数全为 ConstantValue 字段
            getstatic GtConstBuilder$Data.k3 I    //   + 封闭 IXOR，静态可求值）
            ixor
            lookupswitch {                        // 判据锚点：真 case 键表编译期
                100: Ltrue,                       //   确定；命中 case 输出真值
                101: Lwrong1,
                default: Lwrong2
            }
        Ltrue:
            getstatic GtConstBuilder$Data.m0 I  // xor(value) → 1000（真值 case）
            getstatic GtConstBuilder$Data.m1 I
            ixor
            goto Lexit
        Lwrong1:
            getstatic GtConstBuilder$Data.w1 I  // 错值 case：value+1 形（还原面
            goto Lexit                          //   直接删整段，走真 case）
        Lwrong2:
            getstatic GtConstBuilder$Data.w2 I  // 错值 case：value*2 形
        Lexit:
            ireturn
        }
    }
}
```

```jasm
// 数据伴生类：字段全部是 ConstantValue 常量（jasm 不承载 ConstantValue 属性，
// 字段值以注释示意——number_<rand> 命名是生成面线索不是判据）
.super java/lang/Object
.class public super GtConstBuilder$Data {
    .field public static k0 I                       // = 17202
    .field public static k1 I                       // = 896412 ^ 17202 …（示意）
    .field public static k2 I                       // = 41
    .field public static k3 I                       // = …（key 链示意）
    .field public static m0 I                       // = 17202
    .field public static m1 I                       // = 16602（m0^m1 = 1000）
    .field public static w1 I                       // = 1001（value+1 错值）
    .field public static w2 I                       // = 2000（value*2 错值）
}
```

## 2 控制流面

### 2.1 Controlflow（order 400，可选 200 加密前）：六步控制流混淆

**做什么**。按 `Intensity`（1~3）整轮重复六步：

1. **switch 保护**（`SwitchProtect`，`ProtectRate=30`）：已有 switch 的键经随机 magic XOR 重映射，入口插 `INT magic; IXOR`，case 键全换 `key ^ magic`，原 default 外挂转发 label。
2. **switch 摘除**（`SwitchExtractor`，`ExtractRate=30`）：把 switch 摘成 `ISTORE slot` + 逐键 `ILOAD; INT key; IF_ICMPEQ case` 链 + `GOTO dflt`。
3. **假跳转**（`BogusConditionJump`，`GotoReplaceRate=80`）：`goto L` 换成「算术产出常量对的比较门」——`Action`（AND/OR/XOR）随机选一，`val3 = convert(val1, val2)` 编译期确定，`IF_ICMPxx` 恒真落真目标；落空侧塞 `JunkCode`（垃圾值 + 垃圾调用 + 按返回类型的死出口，初始化器内是 `ACONST_NULL; ATHROW`）。`UseLocalVar` 开关把操作数对之一过局部槽（四种压栈序 Type1~3）；`ArithmeticExprBuilder` 把 `IAND/IOR/IXOR` 换成生成的等价算术表达式（与 1.3 同族）；`JunkCode`/`MaxJunkCode` 控制垃圾段；`ExpandedJunkCode` 把垃圾调用池扩到全部类。
4. **mangled if**（`MangledCompareJump`，`IfReplaceRate=50` / `IfICompareReplaceRate=100`）：真实分支条件被改写为近似不等式门（`n4 ≈ n3 + dn*2` 类），配合 `ReverseExistedIf`（`ReverseChance=50`）反转已有 if。
5. **goto→switch**（`TableSwitchJump`，`SwitchReplaceRate=30`，`MaxSwitchCase` 1~10）：`goto` 换成「key XOR 后 tableswitch」结构，`TrappedSwitchCase`（`TrapChance=50`）让假 case 互相跳转成陷进环；`ChaosSwitch` 变体生成两个互指的 switch 环（Krypton 风格）。
6. **局部槽增量**：switch 结构 +5 槽、门结构 +1 槽。

`@DisableControlflow` 注解排除；类有 missing reference（hierarchy 查不到的引用）整类跳过。

**判据分级**：S1（比较门的操作数全为紧邻常量/局部常量链、可静态裁决的子集）/ S2（junk 调用与陷进环的识别是结构事实）。

**本仓现状**：部分。TrampolineSqueeze 吃蹦床 `goto`；PredicateFold 吃常量可裁决的分支/switch（含被 1/3 步产出的常量门——操作数区有标签即放弃）；NumberFold 解锁被藏形跨度夹住的实参区；DeadCode 清不可达垃圾；SeedStrip 吃 `int[]` 种子建表跨度。**缺口**：junk 调用残留（垃圾调用点本身不是常量可裁决形态）、陷进环与 ChaosSwitch 双 switch 环、ArithmeticExpr 表达式（同 1.3 缺口）。

**样例**（第 3 步门 + 垃圾出口 + UseLocalVar Type1 变体；switch 保护与摘除形态并入 switch 样例）：

```jasm
.super java/lang/Object
.class public super GtFlowGate {
    .method public static gated ()V {
        code: {
            ldc 1186                          // 门操作数 1/4：INT(val1)
            ldc 1024                          // 2/4：INT(val2)
            iand                              // 3/4：Action.AND → val3 = 1186 & 1024
                                              //   （UseLocalVar=Off 形；判据锚点：
                                              //   操作数全为紧邻常量 + 封闭算术）
            ldc 1024                          // 4/4：INT(val3)
            if_icmpeq Ltarget                 // 恒真 → goto Ltarget（语义恒等改写）
            ldc 1186                          // junk 出口（不可达）：垃圾调用
            ldc 1024                          //   前先压实参（generateTrashCall）
            invokestatic GtFlowGate.trash (II)I
            pop
            return                            // void 形死出口
        Ltarget:
            return
        }
    }

    .method public static gatedLocal ()V {
        code: {
            ldc 1024                          // Type1 变体 1/5：INT(val2)
            istore 0                          // 2/5：ISTORE(maxLocals)——操作数
                                              //   过局部槽（判据：槽内常量链）
            ldc 1186                          // 3/5：INT(val1)
            iload 0                           // 4/5：ILOAD(maxLocals)
            iand
            ldc 1024
            if_icmpge Lok                     // 恒真门（1024 >= 1024）
            ldc 1                             // junk 值
            pop
        Lok:
            return
        }
    }

    .method public static trash (II)I {
        code: {                               // 垃圾调用目标：全 int 参 static 方法
            bipush 7                          //   （JunkCall 池按返回类型挑，
            ireturn                           //    本仓无还原面——站点计数即可）
        }
    }
}
```

### 2.2 switch 保护与摘除样例（Controlflow 第 1/2 步）

```jasm
.super java/lang/Object
.class public super GtSwitchProtect {
    .method public static sw (I)I {
        code: {
            goto Lentry                       // switch 保护（第 1 步）：入口外挂
        Ldef:                                 //   转发 label（判据锚点：default
            goto Ldflt                        //   指向额外转发块）
        Lentry:
            iload 0                           // switch 输入 = 原键
            ldc 20161186                      // magic（判据锚点：紧邻 LDC int +
            ixor                              //   IXOR，newKey = key ^ magic）
            lookupswitch {                    // 20161222 = 100 ^ magic
                20161222: Lr0,                // 20161223 = 101 ^ magic
                20161223: Lr1,
                default: Ldef
            }
        Lr0:
            bipush 7
            ireturn
        Lr1:
            bipush 9
            ireturn
        Ldflt:
            iconst_m1
            ireturn
        }
    }
}
```

第 2 步摘除形态：`ISTORE slot` + 逐键 `ILOAD slot; INT key; IF_ICMPEQ case` 链 + `GOTO dflt`——每键一个常量比较，与 PredicateFold 的 switch 裁决面正对（键全为紧邻常量即可整站折叠）。

## 3 重定向面

### 3.1 FieldScramble（order 601）与 MethodScramble（order 602）

**做什么**。按 `ReplacePercentage`（默认 10）把字段访问/方法调用换进生成的 static 代理方法（随机名，`RandomName=false` 时 `get_/set_` 前缀）：代理体 = 实参逐槽过手 + 单次字段访问/invoke + 匹配返回；virtual 目标降成 invokestatic（调用点同步降，receiver 转首参）；owner+member 均 public 时代理可提到外置伴生类（`$FieldStatic` / `$MethodStatic`，`GenerateOuterClass`）。被 `@DisableScramble` 标注的 owner/member 跳过。`NativeAnnotation` 开关给代理挂 native 候选注解。

**判据分级**：S2——过手体是恒等转发，改回真实目标栈效应不变；owner `<clinit>` 中性是保守门（初始化暴露）。

**本仓现状**：已处理——`grunt.FieldAccessProxy` 与 `grunt.ProxyInline` 正是这对行为的逆（判据不依赖方法名/标志/家族名）。v3 同款行为（InvokeProxy / FieldAccessProxy）共用这两个 residue。

**样例**（getter/setter/方法代理 + 调用点，与 morpher obf-forms §3.5 同构）：

```jasm
.super java/lang/Object
.class public super GtProxy {
    .method public <init> ()V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            return
        }
    }

    .field static flag I

    .method public static get_flagG7kQ ()I {
        code: {
            getstatic GtProxy.flag I          // getter 代理：全文恰一次字段访问
            ireturn                           //   （判据锚点：直线无标签无异常表
        }                                     //   + 描述符与字段互证）
    }

    .method public static set_flagG7kQ (I)V {
        code: {
            iload 0                           // setter 代理：load args + 单次 put
            putstatic GtProxy.flag I
            return
        }
    }

    .method public static run_redirected (LGtProxy;)I {
        code: {
            aload 0                           // 方法代理（virtual 降 static）：
            invokevirtual GtProxy.run ()I     //   实参逐槽过手 + 转发一次
            ireturn
        }
    }

    .method public run ()I {
        code: {
            bipush 7
            ireturn
        }
    }

    .method public static use ()V {
        code: {
            invokestatic GtProxy.get_flagG7kQ ()I
            pop                               // 判据锚点（调用点）：invokestatic
            new GtProxy                       //   代理原地改回 getstatic / 真实
            dup                               //   invoke，槽效应不变
            invokespecial GtProxy.<init> ()V
            invokestatic GtProxy.run_redirected (LGtProxy;)I
            pop
            return
        }
    }
}
```

### 3.2 InvokeDynamic（order 1000）：调用点 indy 化

**做什么**。把 static/virtual 方法调用换成 `invokedynamic`（INVOKESPECIAL 排除；interface 走 invokeinterface 语义不换）。simple 形：BSM desc `(Lookup,String,MethodType,String,String,String,Integer)` 七参，静态参数 `[encOwner, encName, encDesc, isVirtual]`（三段串各自经单参解密器加密，解密器体内 `LDC key; IXOR`）；virtual 的 desc 首参补 `LObject;`。heavy 形：BSM desc 七参（`(Lookup,String,MethodType,String,Integer,Integer,Integer)`，`[encOwner, magic1, magic2, isVirtual]`——本文档早期「八参」记载有误，grunt2@36c0eef 生成面实为 7 参，simple/heavy 两形同为 7 参仅静态参数段不同），真 name/desc 编进 owner 类的 `@GruntMetadata` 注解（`mv`/`d1`/`d2`，`data1 % 59557 == magic2` 互证，`d2` 是 `name<>desc` 的 XOR 密文）——仿 Kotlin Metadata 形状。BSM 体 = 解密三段 + `Class.forName` + `MethodType.fromMethodDescriptorString` + `findStatic`/`findVirtual` + `ConstantCallSite` + `asType`，带 try/catch 与帧。`Reobfuscate` 开关让生成的 BSM/decrypt 方法再过一遍 Controlflow 与 LocalVariableRename（`EnhancedFlowReobf` 控制算术表达式参与）。`BSMNativeAnnotation` 可把 BSM 标 native 候选。

**判据分级**：S1（BSM 参数结构与元数据互证是语义后果——混淆器要保链接就必须让解密产物与调用点吻合）；还原判据按 BSM 参数结构直接解密（key 静态在场）。

**本仓现状**：`grunt.IndyRestore` 承载（缺口 #2 闭环，见 doc/indy-restore.md）。simple 形按 `encrypt` 自逆 XOR 纯算术直解 + 三路键链（同名解密器 / BSM 体跟随 / 名字族推导）回填真实 invoke，判据全部结构自证、不挂 Era（v2/v3 simple 逐字同源）；heavy 形 `@GruntMetadata` 互证回填挂 `Era::Grunt3` 门——v2 现场因 Grunt2 判据仍缺（缺口 #15 v2 半边挂账）按保守口径关闭，simple 面不受影响。RuntimeMaterial 联动的材料通道静态不可解，fail-closed 挂 `materialLane` 桶（缺口 #15 联动面）。`grunt.IndyFold`（S3，libjsim 模拟 BSM 求真目标）保留为兜底（BoogerV2 语料实测 foldable=0——真目标在解密器 key 链里，须按参数结构直接解密）；Kasumi 的 `IndyDecode` 是 8 参 + 解密器体形钉面的另一指纹，与本行为互斥。

**样例**（simple 形站点 + BSM 骨架 + 解密器；heavy 的元数据注解面见 morpher §3.4）：

```jasm
.super java/lang/Object
.class public super GtIndy {
    .method public static site (I)I {
        code: {
            iload 0                           // 实参（调用点描述符与目标一致；
            invokedynamic f (I)I { invokestatic, GtIndy.bsm, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; } { "\u00e9\u4e2d", "\u4e2d\u00e9", "\u00e9\u00e9", 0 }
                                              // 判据锚点（simple 7 参 desc）：静态
                                              //   参数 = [encOwner, encName, encDesc,
                                              //   isVirtual]，真目标三件套全在参数
                                              //   里，key 在解密器体内
            ireturn
        }
    }

    .method private static synthetic bridge bsm (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; {
        code: {                               // BSM 体骨架（真实体含 try/catch 与
            aload 3                           //   帧；判据读 desc + 静态参数，不
            checkcast java/lang/String        //   依赖体）——三段解密 + findStatic/
            astore 7                          //   findVirtual + ConstantCallSite
            aload 4                           //   + asType 的分派骨架：
            checkcast java/lang/String
            astore 8
            aload 5
            checkcast java/lang/String
            astore 9
            aload 6
            checkcast java/lang/Integer
            invokevirtual java/lang/Integer.intValue ()I
            istore 10
            aload 7                           // 解密 owner
            invokestatic GtIndy.dx (Ljava/lang/String;)Ljava/lang/String;
            pop
            aload 8                           // 解密 name
            invokestatic GtIndy.dx (Ljava/lang/String;)Ljava/lang/String;
            pop
            aload 9                           // 解密 desc
            invokestatic GtIndy.dx (Ljava/lang/String;)Ljava/lang/String;
            pop
            iload 10                          // isVirtual 开关（0=static 分支）
            ifne Lvirtual
            new java/lang/invoke/ConstantCallSite
            dup
            aload 0                           // 静态路径：findStatic + asType
            aconst_null                       //   （骨架：句柄桩）
            invokespecial java/lang/invoke/ConstantCallSite.<init> (Ljava/lang/invoke/MethodHandle;)V
            areturn
        Lvirtual:
            new java/lang/invoke/ConstantCallSite
            dup
            aload 0                           // virtual 路径：findVirtual + asType
            aconst_null
            invokespecial java/lang/invoke/ConstantCallSite.<init> (Ljava/lang/invoke/MethodHandle;)V
            areturn
        }
    }

    .method static dx (Ljava/lang/String;)Ljava/lang/String; {
        code: {                               // 单参解密器（v2 式）：char 循环 +
            aload 0                           //   体内 LDC key; IXOR——key 提取
            invokevirtual java/lang/String.length ()I
            istore 1                          //   锚点与 ConstPoolEncrypt 的 dx
            iconst_0                          //   同构
            istore 2
        Lloop:
            iload 2
            iload 1
            if_icmpge Ldone
            aload 0
            iload 2
            invokevirtual java/lang/String.charAt (I)C
            ldc 61680                         // 判据锚点：LDC int + IXOR
            ixor
            i2c
            istore 3
            iinc 2 1
            goto Lloop
        Ldone:
            new java/lang/String
            dup
            invokespecial java/lang/String.<init> ()V
            areturn
        }
    }
}
```

## 4 结构面

### 4.1 MethodExtractor（order 5）：实例方法体搬进伴生类

**做什么**。逐类把全部非 static、非 abstract/native/synthetic、非 lambda、非初始化器的方法体搬进新建伴生类 `<Class>$Methods`（伴生类持一个 `accessor L<Class>;` 私有字段与 `<init>(Lowner;)V>`）：伴生方法 = `PUBLIC+STATIC`、desc 前插接收者 `(<Lowner;>原 desc)`、体为原体但全部字段访问/方法调用改经 owner 类上的 `getter-*`/`setter-*`/`accessor-*` static 过手方法；原方法变透传桩（`ALOAD 0; 实参; INVOKESTATIC 伴生.方法`）；lambda 私有方法转 public。getter/setter/accessor 全部落在 **owner 类**（不进伴生类）；伴生类的 `accessor` 字段实际未接线（GETFIELD 路径走接收者参数），是生成面死字段。indy 的 BSM handle 也经 `accessor-indy-*` 桥接。

**判据分级**：S2——结构事实（伴生方法体与桩的转发关系可静态重建；还原 = 桩与伴生体合并、过手方法内联、伴生类删除）。

**本仓现状**：无处理（缺口清单 P2——面大，价值中；语料若见到 v2 MethodExtractor 产物优先补）。

**样例**（owner + 伴生类两类合并呈现）：

```jasm
// owner：方法体变成透传桩 + 过手方法留在此类
.super java/lang/Object
.class public super GtExtract {
    .method public <init> ()V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            return
        }
    }

    .method public add (II)I {
        code: {                               // 判据锚点（桩形）：ALOAD 0 + 实参
            aload 0                           //   + INVOKESTATIC 伴生.同名方法，
            iload 1                           //   desc 前插 Lowner;——原体已搬走
            iload 2
            invokestatic GtExtract$Methods.add (LGtExtract;II)I
            ireturn
        }
    }

    .method public call (II)I {
        code: {
            aload 0
            iload 1
            iload 2
            invokestatic GtExtract$Methods.call (LGtExtract;II)I
            ireturn
        }
    }

    .method public static final accessor-virtual-add$Qq9 (LGtExtract;II)I {
        code: {                               // 过手方法（判据锚点：留 owner 类，
            aload 0                           //   逐槽过手 + 转发一次）；伴生体内
            iload 1                           //   的 invokevirtual 调用改指这里
            iload 2
            invokevirtual GtExtract.add (II)I
            ireturn
        }
    }
}
```

```jasm
// 伴生类：方法体搬进来，槽位布局与实例方法一致（slot0 = 接收者）
.super java/lang/Object
.class public super GtExtract$Methods {
    .field private accessor LGtExtract;       // 生成面死字段（GETFIELD 路径未接线，
                                              //   还原侧可随伴生类整体删除）
    .method public <init> (LGtExtract;)V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            aload 0
            aload 1
            putfield GtExtract$Methods.accessor LGtExtract;
            return
        }
    }

    .method public static add (LGtExtract;II)I {
        code: {                               // 搬进来的原体（纯算术段原样保留）
            iload 1
            iload 2
            iadd
            ireturn
        }
    }

    .method public static call (LGtExtract;II)I {
        code: {                               // 原 call 体里的 this.add 调用改经
            aload 0                           //   owner 类的 accessor 过手：
            iload 1                           //   receiver + 实参压栈序不变
            iload 2
            invokestatic GtExtract.accessor-virtual-add$Qq9 (LGtExtract;II)I
            ireturn
        }
    }
}
```

### 4.2 DeclareFields（order 103，HideDeclaredFields）：常量字段显式化

**做什么**。把字段的 `ConstantValue`（`field.value`）搬进初始化器：static 字段在 `<clinit>` 头部插 `INT/LONG/BIPUSH/SIPUSH/LDC <value>; PUTSTATIC`，实例字段在全部 `<init>` 头部插同样写（`PUTFIELD`），随后清掉 `ConstantValue`。目的是让下游加密 transformer（NumberEncrypt 等）能处理这些值。

**判据分级**：S1——初始化器单次常量写 + 全容器无其他读写，回填 `ConstantValue`（并补 `ACC_FINAL`）是语义恒等；这正是本仓 ConstValuePromote 的判据。

**本仓现状**：已处理——`grunt.ConstValuePromote` 是该行为的精确逆（判据链互为镜像：单次常量写 / 类型与常量种类吻合 / 全容器无其他读写）。

**样例**（static 形态；实例字段形态同构进 `<init>`——样例验证器对 `<init>` 内 super 后 `aload 0` 存在已知误报，故样例只呈现 static 形态）：

```jasm
.super java/lang/Object
.class public super GtDeclared {
    .field public static tag Ljava/lang/String;
    .field public static code I
    .field private static secret J

    .method static <clinit> ()V {
        code: {                               // 判据锚点：字段唯一的写，操作数是
            ldc "PROTECTED BY GRUNT KLASS MASTER"   //   单条纯常量装载 → 回填
            putstatic GtDeclared.tag Ljava/lang/String;   //   ConstantValue + FINAL
            sipush 65535                      // 窄值走 BIPUSH/SIPUSH（写入侧）；
            putstatic GtDeclared.code I       //   还原侧类型与常量种类互证
            ldc 123456789L
            putstatic GtDeclared.secret J
            return
        }
    }
}
```

### 4.3 SyntheticBridge（order 701）：伪 synthetic/bridge 标志

**做什么**。非注解类整类撒标志：类带 `ACC_SYNTHETIC`（有注解的类跳过）、全部方法补 `ACC_SYNTHETIC`、未注解字段补 `ACC_SYNTHETIC`、非初始化器非抽象方法补 `ACC_BRIDGE`。`FlattenMethods=true` 时反向操作：摘掉方法的 synthetic（让 lambda 方法对反编译器可见）。

**判据分级**：S2——伪标志是结构事实（普通成员带编译器合成标志）。

**本仓现状**：已处理——`grunt.FlagsStrip` 摘伪 `synthetic`/`bridge`（仅 static 成员、名字不含 `$`、枚举跳过、顺带抬类版本）。差异点如实记录：v2 撒标志不挑 static，FlagsStrip 的保守门（static ∧ 无 `$`）意味着实例成员上的伪标志**不在** FlagsStrip 摘除面内（保守宁留）；morpher obf-forms §3.8 的「整类匹配门」是另一口径。

**样例**：与 morpher obf-forms §3.8 的 FakeSyntheticBridge 样例同构（类级 `synthetic` + 方法 `bridge synthetic` 对），v3 文档 §6.3 引用同一形状，此处不重复。

## 5 垃圾与署名面

### 5.1 ClonedClass（order 100）与 TrashClass（order 101）

**做什么**。ClonedClass：从输入类里随机挑 `Count` 个整体克隆，名字加 `-cloned$$<n>` 后缀，注解默认清空。TrashClass：按 `Count` 生成垃圾类（包名 `Package` + `Prefix` + 5 随机字符），父类随机取 `ConcurrentHashMap` / `ConcurrentLinkedDeque` / `ConcurrentSkipListMap` 之一，成员为一个 `public static int c = 8964`（ConstantValue）与调 super 的 `<init>`。垃圾类不接引用边，纯撑体积/对抗分析。

**判据分级**：S2——零引用 + 生成面形状（命名、父类选择、成员形状）是结构事实；删除需引用普查兜底（防反射/资源文本引用）。

**本仓现状**：无处理。DeadMember 只删成员不删类；类级删除面（垃圾类识别 + 引用普查）是缺口清单 P3。

**样例**（TrashClass 形态；ClonedClass 即输入类原样改名，无独立形态）：

```jasm
.super java/util/concurrent/ConcurrentHashMap
.class public super TrashAqBcD {
    .field public static c I                  // ConstantValue = 8964（jasm 不承载
                                              //   该属性，值以注释示意；判据锚点：
                                              //   零引用 + 生成面命名/父类形状）
    .method public <init> ()V {
        code: {
            aload 0
            dup                               // 判据锚点：ALOAD 0; DUP; super <init>
            invokespecial java/util/concurrent/ConcurrentHashMap.<init> ()V
            return                            //   ——调 super 两次压 receiver 的
        }                                     //   生成器习惯形
    }
}
```

### 5.2 Watermark（order 1003）

**做什么**。逐非接口类撒署名：`FieldMark`（随机水印字段三变体——`private static String <名字> = <marker>`、`private static int _<marker> _ = <幸运数>`、重复 String 形）、`MethodMark`（`private static <名>()Ljava/lang/String; { LDC marker; ARETURN }`）、`AnnotationMark`（挂 `ProtectedByGrunt` / `JvavMetadata` 等注解）、`InterfaceMark`（把 `FatherOfJava`（默认 `jvav/lang/YuShengJun`）塞进 interfaces）、`CustomTrashMethod`（自定义水印方法体）。默认 Names 含 `I AM WATERMARK`（**字段名带空格**——jasm 标识符词法装不下，样例以无空格形名示意）。

**判据分级**：S2——水印串/命名是结构事实；删除需内容判据（水印串列表）+ 零引用普查。

**本仓现状**：无处理。Kasumi `MetadataStrip` 的水印面判据是 Kasumi 家族口径（`PROTECTED BY Kasumi` / `Gruntpocalypse` / `kasumi_` 前缀），Grunt2 水印默认串（`PROTECTED BY GRUNT KLASS MASTER` 等）不同族不共判据；可按同构面扩家族判据（缺口清单 P3）。

**样例**：

```jasm
.super java/lang/Object
.class public super GtWatermark {
    .field private static final mark_str Ljava/lang/String;
                                              // 判据锚点（字段水印）：ConstantValue
                                              //   = marker 串（真实产物字段名可含
                                              //   空格，jasm 样例以无空格名示意）

    .method private static wm$Ab12 ()Ljava/lang/String; {
        code: {
            ldc "PROTECTED BY GRUNT KLASS MASTER"
            areturn                           // 判据锚点（方法水印）：LDC marker +
        }                                     //   ARETURN 两指令体，零引用
    }

    .method static <clinit> ()V {
        code: {
            ldc "CYKA BLYAT"
            putstatic GtWatermark.mark_str Ljava/lang/String;
            return
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

### 5.3 HWIDAuthenticator（order 102）：HWID 校验注入

**做什么**。生成 1~`CachePools` 个校验池类（名字 = 随机类名 + 3 随机字符）：每池一个 `static List<String>` 缓存字段，`<clinit>` 按模式填充——`OnlineMode=true` 走 `java/net/URL` + `BufferedReader` 拉取 `OnlineURL` 逐行入表（带 try/catch 与帧）；offline 模式直接把 `OfflineHWID` 列表入表。另生成一个 frameUtil 类（`showHWIDWhenFailed` 时才加，用于失败时弹 HWID）。再给每个非接口类的 `<clinit>`（没有则把验证方法改名成 `<clinit>`）头插 `INVOKESTATIC verify()`。验证方法体：`sun/misc/Unsafe` 反射取出 → 拼 `PROCESS_IDENTIFIER`/`PROCESSOR_*`/`COMPUTERNAME` 等环境变量（`makeConcatWithConstants` indy）→ AES 加密（`EncryptKey`）→ 与池表比对 → 不符走 frameUtil 弹 HWID/终止。`EncryptConst=true` 时验证方法体再叠 StringEncrypt + NumberEncrypt。

**判据分级**：检测-only——校验是运行时面，静态还原 = 删验证调用 + 删池类（等价于 DeadMember/DeadCode 的删除面 + 校验调用点摘除），比对逻辑本身不可「还原」只能「去除」；误删面（校验失败路径上的副作用）需 CFG 证明。

**本仓现状**：无处理（缺口清单 P3，标检测-only）。

**样例**（池类 clinit 骨架 + 验证方法骨架；AES/Unsafe 段以桩示意）：

```jasm
// 校验池类（名字 = 随机类名 + 随机字符）
.super java/lang/Object
.class public super GtHWID$Kx3pool {
    .field public static cache Ljava/util/List;

    .method static <clinit> ()V {
        code: {
            new java/util/ArrayList           // 判据锚点（池形）：NEW ArrayList +
            dup                               //   PUTSTATIC List 字段
            invokespecial java/util/ArrayList.<init> ()V
            putstatic GtHWID$Kx3pool.cache Ljava/util/List;
            getstatic GtHWID$Kx3pool.cache Ljava/util/List;
            ldc "hwid-line-1"                 // offline 形：逐行 LDC + List.add
            invokeinterface java/util/List.add (Ljava/lang/Object;)Z
            pop
            getstatic GtHWID$Kx3pool.cache Ljava/util/List;
            ldc "hwid-line-2"
            invokeinterface java/util/List.add (Ljava/lang/Object;)Z
            pop
            return                            // online 形：同槽位换 URL + BufferedReader
        }                                     //   读循环（try/catch + 帧）
    }

    .method static hash ()Ljava/lang/String; {
        code: {
            ldc "hwid-digest"                 // 拼接 + AES 摘要段骨架（verify 调它）
            areturn
        }
    }
}
```

```jasm
// owner 类里的验证方法（<clinit> 头部插 INVOKESTATIC verify）
.super java/lang/Object
.class public super GtHWID {
    .method public static verify ()V {
        code: {                               // 判据锚点（验证形）：环境变量拉取 →
            ldc "PROCESS_IDENTIFIER"          //   拼接 → 池表比对 → 失败路径 ATHROW
            invokestatic java/lang/System.getenv (Ljava/lang/String;)Ljava/lang/String;
            ldc "COMPUTERNAME"
            invokestatic java/lang/System.getenv (Ljava/lang/String;)Ljava/lang/String;
            invokestatic GtHWID$Kx3pool.hash ()Ljava/lang/String;
                                              //   （拼接 + AES 段骨架示意）
            getstatic GtHWID$Kx3pool.cache Ljava/util/List;
            invokeinterface java/util/List.contains (Ljava/lang/Object;)Z
            ifne Lok
            new java/lang/Error
            dup
            ldc "Can't reach Unsafe"
            invokespecial java/lang/Error.<init> (Ljava/lang/String;)V
            athrow                            // 失败出口（真实体走 frameUtil 弹窗）
        Lok:
            return
        }
    }
}
```

### 5.4 Crasher（order 1002）

**做什么**。把类/字段/方法三张 `Signature` 属性覆写成 massive 乱串（`Random` 开关选空白 massive 串变体），不改字节码，砸反编译器。

**判据分级**：S2——非法/超长 generic signature 是结构事实；清理 = 按通用非法 signature 规则删属性。

**本仓现状**：无处理（缺口清单 P3；v3 DecompilerCrasher 同面）。

## 6 改名与伴随面（要点账）

改名族（LocalVariableRename 800 / MethodRename 801 / MethodRenameKr 802 / FieldRename 811 / FieldRenameKr 812 / ClassRename 821 / MixinFieldRename 900 / MixinClassRename 901）：字典驱动（`Alphabet` / `Numbers` / `ConfuseIL`(Iil1) / `Confuse0O`(Oo0) / `ConfuseS5`(Ss5$) / `Arabic`(\u0600-\u06ff) / `Custom` 文件字典），`Prefix`/`Parent`(net/spartanb312/obf/)，`Reversed` 加 `U+200E` 尾缀，`HeavyOverloads` 同名不同 desc 复用，ClassRename 的 `CorruptedName` 破坏 zip 条目名。Kr 变体只是换谚文字典。**本仓现状：无处理（改名还原走 mapping 旁证，不在本仓 residue 面；CONVENTION 的 `evidence` 位留给 Rename）**。ReflectionSupport（order 199）伴随改名：把 `Class.forName`/`getResourceAsStream` 等反射串常量按映射表改写、并把反射访问的成员名收进黑名单让改名器跳过——产物里无独立形态（伴随面）。

## 7 清理面（要点账）

清理面行为删的是信息不是语义，还原侧无动作，只做如实登记：

- **SourceDebugRemove**（0）：`SourceDebug`/`LineDebug` 开关摘 SourceFile/SourceDebugExtension/LineNumberNode；`RenameSourceDebug` 可换成随机假名（默认表含中文假名）。
- **KotlinOptimize**（2）：摘 `kotlin/jvm/internal/Intrinsics` 六种检查调用（实参转 POP）、可抛型 Intrinsics 的字符串实参改写 `REMOVED BY GRUNT`、摘 `@kotlin.Metadata`/`SourceDebugExtension`/`DebugMetadata` 注解。
- **EnumOptimize**（3）：`values()` 里 `clone()` + `CHECKCAST` 删除。
- **DeadCodeRemove**（4）：删 `ILOAD/FLOAD/ALOAD + POP`、`LLOAD/DLOAD + POP2`、双 load + `POP2` 序列。
- **Shrinking**（1004）：删 InnerClasses/外层类信息、未用标签、NOP（默认关）、方法 Signature（默认关）、指定注解（默认 `@Override`）。
- **PostProcess**（MAX）：摘四个内部排除注解、删 dummy 指令（opcode 1919810 标记）、修 MANIFEST.MF / plugin.yml / bungee.yml / fabric.mod.json / velocity 的主类名映射。
- **LocalVariableRename**（800）的 `DeleteLocalVars`/`DeleteParameters`：删 LocalVariableTable。
- **ShuffleMembers**（1001）：methods/fields/annotations/exceptions 四面打乱——顺序随机化，还原侧无需动作（登记即可）。

## 8 v2 行为 → residue 映射速查

```text
已处理：DeclareFields→ConstValuePromote、FieldScramble→FieldAccessProxy、
        MethodScramble→ProxyInline、SyntheticBridge→FlagsStrip
部分  ：NumberEncrypt→NumberFold（IAND/LAND 模板 + arrayed 池缺口）、
        ConstPoolEncrypt→NumberFold+ConstValuePromote（调用点回填缺口）、
        Controlflow→TrampolineSqueeze/PredicateFold/NumberFold/DeadCode/SeedStrip
        （junk 调用残留、陷进环、ArithmeticExpr 缺口）
部分  ：InvokeDynamic→IndyRestore（simple 直接解密回填，缺口 #2 闭环；
        heavy 待 Era::Grunt2 判据）+ IndyFold（S3 兜底，语料 foldable=0）
无处理：MethodExtractor、ConstBuilder、StringEncrypt、ArithmeticEncrypt、
        StringEqualsRedirect、Crasher、Watermark、垃圾类三面、改名族、HWID
清理面：SourceDebugRemove、KotlinOptimize、EnumOptimize、DeadCodeRemove、
        Shrinking、PostProcess、ShuffleMembers、LocalVariableRename
```
