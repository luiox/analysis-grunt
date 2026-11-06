# 常量调用：本仓怎么还原

> 口径：本文件只写**本仓的实现**。技术形态见 [constant-call.md](constant-call.md)。
> 代码：`src/grunt/constant_call.cpp`（三段），`src/grunt/pipeline_adapter.cpp`（管线入口）。

## Scan

### 粗找（召回优先，允许钉形状）

扫全容器每个有方法体的方法，取 `INVOKESTATIC`，解析成员引用，命中容器方法索引即成为候选。**这里不判实参、不判纯度**——只保证不漏。

### 细找（精度优先，必须钉事实）

三条同时成立才进 `sites`：

1. **目标可折**
   - `ACC_STATIC` 置位；
   - 非 `ACC_SYNCHRONIZED`（监视器获取是可观测行为，删调用就变了）；
   - 名字不以 `<` 开头（排除 `<init>` / `<clinit>`）；
   - 返回类型不是 `void`（无值可折）；
   - 在容器内有方法体。
2. **实参定值**：从调用点向前逐条走，每条都必须是纯常量装载，累计槽数必须**恰好等于**描述符参数槽数（`long`/`double` 双槽）。中途遇到任何非常量装载（`iload`、`getstatic`、`dup` 等）即拒绝。`ldc` 经会话常量池解析：数值常量接受，`String`/`Class`/`MethodHandle` 等一律拒绝（不猜内容）。
3. **求值成功**：用 mjbase 的 `asm_evaluator` 在常量实参下跑目标方法。求值子集只含数值运算、栈整形、分支与返回——**不含字段访问、数组访问、方法调用、对象分配、显式抛出**。因此「跑出值」等价于「这条执行路径无堆副作用」。

额外保守面（不算判据，是拒绝理由）：被调类的 `<clinit>` 必须缺失或只剩 `NOP`/`RETURN` 且无异常表。理由：`invokestatic` 是类的主动使用，删掉调用会改变类初始化时机；只有确认初始化器什么都不做才允许折。**同类豁免**（JVMS 5.5）：调用点与目标同类时，本类方法开始执行时类已经在初始化中/已完成，同类 `INVOKESTATIC` 不触发初始化，删调用不移时机——豁免；豁免站点的纯性仍由求值独立证明（boar 取证：processor 键解锁链全部 3262 个可折蹬床站点都在非中性 clinit 类里，门不豁免整条链空转；跨类站点照旧拒绝）。

### 求值环境接管：`java/lang/Math.max(II)I`

 IndyRestore 还原 `__grt$indy$` 蹬床体内的 indy 后，CFF inline 桥的 MaxConst 站点（grunt3 `StateOp.MaxConst.emitTo` 逐字，`e41e852`）从「蹬床调用」还原成「蹬床体内 `invokestatic java/lang/Math.max(II)I`」——求值蹬床体必然踩到这条 JDK 调用，而 jsim 核心/num 包没有 JDK 数学面，缺接管整条面在 `EvalBadState` 拒。本残留的求值会话按签名注册 `Math.max` 接管：弹双参、全序取大；unknown 按未知口径传播（结果落 `ValueKindUnsupported`），不编值。申报理由（最小扩面）：max 是全序纯函数，双常量实参下值唯一确定，接管不引入任何猜测面。

### 拒绝面

`ScanResult::rejects` 逐条记 `(调用类, 调用方法, pc, 原因)`。原因枚举覆盖：容器外目标、非 static、无方法体、初始化器、同步方法、void、类初始化非空、实参非常量、实参类别不支持、求值不支持 / 熔断 / 状态不符、结果类别不可折。

`GRUNT_TRACE=1` 时把 `sites/rejects` 计数打到 stderr。

### 为什么求值要放在 scan 里

CONVENTION §5.1：三段的状态需求本来就不对称，**只有 scan 需要跨站点的昂贵缓存**。求值是判定的一部分（「这个调用是不是常量」），不是改写的一部分；放进 scan 才能让 rewrite 只消费结果、不回头找站点。

## Rewrite

按 `ScanResult` 分组：调用类 → 调用方法 → 站点。改写前先过一道**往返自检**（见下），每个站点再走六步：

1. 在重新 promote 的 DOM 里按 `callSrcPc` 找回调用指令（`InsnId` 是会话内句柄，不能跨会话带出，所以站点用原始 pc 定位）；
2. 校验成员引用与 `ScanResult` 一致；
3. 用与 scan 相同的规则重新收集实参，并逐值比对——**这是防「scan 与 rewrite 之间 DOM 语义漂移」的自检**，不一致就放弃该站点；
4. 造一条常量装载：`iconst/bipush/sipush` 优先，放不下才落池。池索引一律经 `CodeEditor::loadConst` 选形态——`LDC` 的操作数域只有 u1，大类的池索引轻易超过 255，编码器对超界 `LDC` 直接 `Unencodable`；
5. **把首个实参装载原地改写成那条常量装载**，然后 `eraseBatch(其余实参装载… + 调用)`；
6. 失败则把首条写回原样，DOM 保持原样。

一个方法内全部站点改完后 `lowerMethod`；一个类内全部方法改完后 `writeClass` 并回写容器。**任一步失败即整类放弃**，不留半改状态。

### 为什么是「原地改写首条」而不是「插一条 + 删首条」

这一步不是风格选择，是唯一能走通的路：

- `CodeDom::insertAt` 把新指令的 `srcPc` **硬清成 `kNoPc`**（合成指令没有旧位置，这是 DOM 的不变量，绕不过去）。
- 而 `lowerMethod` 的属性重定位全靠 `pcmap`（旧 pc → 新 pc）：`LineNumberTable` 的每行、`LocalVariableTable` 的起止、`StackMapTable` 的每帧，都要**精确命中**一个旧 pc，命中不了就 `RelocationBadTarget` 整体失败——这是 fail-closed 的，不给猜。
- 所以「删掉首条」会让锚在区间起点的那些行全部失去落点，整个方法连同它的类一起作废。样本里这几乎是必然事件（注入的实参装载继承了被替换指令的行号/帧位）。

原地改写保住首条的 `srcPc`：锚在它上面的行照常重定位，锚在它上面的标签也不必迁移（位置没变）。区间里**其余**指令被删掉，它们的旧 pc 随之消失——因此额外要求这些指令上没有标签：有标签说明分支 / switch / 异常范围从区间中段进入，删掉会让目标悬垂，整站点放弃（`LabelBlocked`）。首条不受此限。

### 往返自检（rewrite 的前置门）

改任何方法之前，先在**未改动的独立会话**上把它 `promote → lower` 走一遍；走不完的方法整方法跳过（`DirtySet::skippedByRoundTrip` 记账）。

理由是 `lower` 不是 `promote` 的恒等逆：libjbc 的编码器对少数布局/属性组合会明确拒绝。这类方法与改写无关，但一旦被改写就会把整个类拖下水，表现为「改写有 bug」。先探一遍，失败面就只剩真正的改写缺陷。

### 栈形为什么不变

原序列：`n` 条常量装载（合计 `s` 槽）→ `invokestatic`（弹 `s` 压 `r` 槽，`r ∈ {1,2}`）。
新序列：`1` 条常量装载（压 `r` 槽）。

两条轨迹在「调用前」与「调用后」的栈深度相同，中间不产生新的栈形——所以 StackMapTable 只需按新布局重算，不需要额外的类型推理。`maxStack` 沿用原值：新序列的峰值（`base + 1`）不超过原峰值（`base + s`，`s ≥ 1`）。

## Proof

两道门，都是家族无关的：

1. **类数不增**：改写前后 `visitResources` 的类条目计数必须相等。
2. **脏类可重新解析**：每个被回写的类重新开会话，必须成功——抓「写出的字节自己都读不回来」这类错误。

帧与校验器层面的验收在语料回归里做（`test/tools/VerifyJar.java` 逐类过校验器），因为它需要 JDK，不适合放进单元测试。

## 已知边界

- **不做跨方法常量传播**：目标方法若读字段或调别的纯方法，求值器直接 `Unsupported`，站点被拒。要支持得先做字段终值/调用内联，那是另外的残留。
- **不做 `<clinit>` 常量传播**：`getstatic arr; <idx>; iaload` 这类字段池化数字不在本残留范围内。
- **不折引用结果**：返回 `String`/对象的方法即使纯也不折——引用身份不可静态构造。
- **同类豁免只豁免「初始化时机」这一个门**：纯性、常量实参、求值三条判据对同类站点照收不误。
- **processor 键解锁链的静态天花板**（boar 全量取证，grunt3 `e41e852` 源钉死）：CFF 桥的 key processor 形（`emitProcessorStateProgram` 五条窗口 `[INT; ISTORE; ILOAD; INT; INVOKESTATIC proc]`）的 proc 恢复目标全部落在 native 摘除的 action 壳上——boar 配置 `keyProcessorNativeCandidateRatio=1.0`，6117 个 `(II)I` 恢复目标 100% `ACC_NATIVE` 无体，算术已随 native pipeline 出 jar。实参又是 `ILOAD s`（夹心形，非紧邻常量）——两道静态墙叠加，这部分**永远不可静态折**，属 CffUnflatten key-processor 桶的永久余量（6121）。可折的只有 Math.max 蹬床面（inline 链 MaxConst 被 ReferenceObfuscate→NativePreProcessor 包成蹬床的形态）：687 站点折叠、80 座岛离开 processor 桶（66 解 + 14 转 foreign-ref 桶）。

## 语料账本（boar 全链罐，缺口 #4 余量解锁链）

链（`--list` 实证注册名）`grunt.IndyRestore,grunt.ConstantCall,grunt.CffUnflatten,grunt.DispatcherRestore`：

| pass | 改前 | 改后 |
| --- | --- | --- |
| IndyRestore | sites=30279 | 同（不改） |
| ConstantCall | sites=7 | **sites=687**（413 类） |
| CffUnflatten | islands=1481 / key-processor=6201 | **islands=1547 / key-processor=6121** |
| DispatcherRestore | 0 | 0（见下） |

- **proc 蹬床全貌**（独立解析器全量取证）：jar 内 78154 处 Grunt simple-form indy **全部**位于 `__grt$indy$` 蹬床体内（NativePreProcessor `e41e852` 的 indy 桥，体形 `[实参装载; indy; ireturn]`）；其中 `(II)I` 蹬床 19701 座。IndyRestore 已覆盖蹬床（无名字过滤）：8477 还原 / 10173 材料通道拒 / 1051 键链断。
- **恢复目标构成**（19701 `(II)I` 蹬床的去向）：Math.max 1699；native action 壳 6117（`keyProcessorNativeCandidateRatio=1.0`）； exotic/缺类 62；其余 desc 非 `(II)I`。
- **数学**：5 条窗口形（真 key processor 桥）3262 站点全部指 native 壳——静态不可折（算术不在 jar 里）；Math.max 蹬床 1699 站点全是链中位形，其中双紧邻常量实参的 657 上限——687 实折（含同类门豁免放行的其余纯算术站点）落在这个量级。
- **DispatcherRestore 仍 0**：5827 desc 形候选中有岛的 43 个（23 processor 门）里，processor 门岛全部属 native action 面（population A）——Math.max 面消化 80 座岛没有一座落在分发器方法上，头锚不出场。加挂 NumberFold/PredicateFold 变体验证（sites=21422 / 0）同样 0——**瓶颈是 native 墙，不是链序**。DispatcherRestore 的 boar 解锁需要 native action 的静态语义（无 native 库即无解），如实挂账。
- **门结果**：boar + origin 双罐 corpus-regress（链 `IndyRestore,ConstantCall,CffUnflatten,DispatcherRestore`）0 失败；类数 3065→3065 / 775→775；JVM `-Xverify:all` 输入/输出失败集逐类相同（基线同挂，零新增）；双跑产物逐条目 CRC 全等（3084 条目 0 差异）；run-lab 语料全链与 ConstantCall 单跑双绿。
- **unittest**：`constant_call_test.cpp` 19 案（原 16 + 同类门豁免正例 + Math.max 蹬床求值正例 + 同类 Math.max 蹬床改写正例），全套 229/229（基线 226 + 3）。
