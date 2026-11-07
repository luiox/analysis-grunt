.. _gate-processor-fold-spec:

门点 processor 折叠规格（GateProcessorFold）
============================================

状态：定稿，已落地（缺口 #10 面 a；罐 A 组合链 4282/4282 折叠 residual=0，单测 264/264）。微观契约（Site/Reject 桶枚举、ScanResult 桶计数、TAKEN 改写受害面）在 ``gate_processor_fold.hpp`` 头注；本文承载适用情况、跨方法求值算法、前后 jasm 对照、拒绝桶与预算口径，读法：先 §2 判「这个站点吃不吃」，再 §3 看求值怎么跑、§4 看改写怎么落，§5 是每面的字节级实样（标注对应测试名）。\ **本篇是全家族复杂 pass 文档的样板篇**——后续复杂 pass 规格按本篇五段结构（适用情况 / 算法 / 前后 jasm 对照 / 拒绝桶 / 预算与记账）写作。

.. _gate-spec-niche:

1 生态位
--------

Grunt v3（``ControlflowJump``，grunt3@e41e852）往真分支边上插「processor 门」（guard / mangled-gate 块，生成器 ``OpaquePredicateProcessorRegistry``）：三个 int 常量喂一个生成面 processor，返回值与 compareValue 常量比较后条件跳转，真臂是原程序控制流、fallthrough 是 junk 臂。\ ``grunt.GateProcessorFold`` 把这类门点静态裁决成恒真并折叠掉比较，让原控制流直通。

三证据面叠起来，分支走向是静态事实而非统计近似：实参全常量（left/right/salt 紧邻纯常量装载，compareValue 是 IntConstChain 常量走廊）、proc 体封闭纯算术（生成面判据，跨方法求值在常量实参下有唯一确定返回值）、恒真自证（生成器带 ``require(comparison.test(result, compareValue.value))``——门点必须恒走真分支，否则混淆器自己的 payload 都到不了）。

与两位邻居的分工：

- ``ConstantCall`` 折「全常量实参 + 纯算术目标」的\ **调用点**，它的求值器装了 str/num/reflect/indy 扩展包 + Math.max 接管；门点 proc 调用是它的站点子集，罐 A 全链实测约 5814 个门点调用被它先折成常量——但\ **比较面它不碰**\ （折叠后的 const-vs-const 比较是 PredicateFold 的 face）。
- ``PredicateFold`` 折「操作数全常量」的\ **比较**，但看不见藏在前窗常量走廊后面的 proc 调用（操作数不是紧邻常量装载）。
- 本残留 = 门点专用：形状钉（(III)I ActionDesc + 前窗走廊）+ 自带求值器（最小 mock 面：只接管 rotateLeft，刻意不装扩展包——TLR / material / indy 面必须在这里 fail-closed 显形）+ 比较折叠一次完成。

**链序**：排在 ``grunt.ConstantCall`` 之后、\ ``grunt.CffUnflatten`` 之前（罐 A 实证链：\ ``IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,GateProcessorFold,CffUnflatten``）。上游 IndyRestore 把 ``__grt$indy$`` 蹬床体内的 indy 还原成直线转发（蹬床可求值面扩大）；上游 ConstantCall 先折纯算术 proc 调用——本残留吃的是 ConstantCall 求值不动的余量（rotateLeft 体），罐 A 复测上游四 pass 数字与锚点逐项全等；下游 CffUnflatten 岛还原不受影响（门点在 CFF 岛结构之外，islands 1543 不变）。

**无硬链依赖**：ReferenceObfuscate（chance=1.0）按 ``@GENERATED_METHOD`` 跳过生成方法，门点 proc 调用\ **从不被 indy 包装**——本残留单跑（无任何上游）在罐 A 原罐上即可折叠 7643 站点（含 ConstantCall 位置的 3361 个 + 上游不回归的 rotateLeft 体 4282 个）。推荐位置挂在 ConstantCall 之后是为了\ **不动既有锚点**\ （单跑会让 ConstantCall 的 24479 锚点位移——门点调用被本残留先折）。

**为什么 ConstantCall 折不完全**：proc 体里的 ``INVOKESTATIC java/lang/Integer.rotateLeft(II)I`` 是 JDK 调用，libjsim 核心/num 包都没有 JDK 数学面，ConstantCall 求值 EvalBadState 整站拒绝——罐 A 供给链实测剩余 4282 个 const-fed 门点全部因此存活。本残留自带同一枚举器并补 rotateLeft 接管（全序纯函数，位移掩码 5 位，常量实参下值唯一），把这一桶吃掉。

.. _gate-spec-accept:

2 适用情况：门点形状门
----------------------

生成面逐字形（源锚 grunt3@e41e852 ``ControlflowJump.kt`` guard 块发射与 ``OpaquePredicateProcessorRegistry.kt`` / ``IntExpression.kt``）：

::

   bipush <left>
   bipush <right>
   bipush <salt>
   invokestatic <proc> (III)I      // 门点 processor（ActionDesc 恰为 (III)I，与门点同类）
   <compareValue 常量走廊>          // compareValue.emit()：[INT; (INT; OP)*]，OP ∈ imul/iand/ior/ixor
   if_icmpxx  Lreal                // 真分支臂（taken）；fallthrough = junk 臂

proc 体（生成面 ``IntExpression`` / ``BitAction``）：三条槽位解码链（``iload n; 常量走廊; ixor; istore n`` × 3）+ BitAction 栈算术（SWAP / DUP_X1 / DUP2 / DUP_X2 / POP 形）+ ``PredicateStep`` 盐旋转步（``INVOKESTATIC java/lang/Integer.rotateLeft(II)I``）+ ``ireturn``，全程纯算术。\ ``RandomBound`` 变体的 proc 体额外引用 ``ThreadLocalRandom``\ （运行期随机，见 §6）。部分门点经 NativePreProcessor 的 ``__grt$indy$`` 蹬床调 proc——IndyRestore 还原后蹬床 = 纯 ``iload`` 前缀 + 单 invoke + ``ireturn`` 的直线转发器，求值经编排层递归穿透。

Scan 的形状钉六判据，逐条如下；所有判据只在容器内字节码上可证时放行——「运行时可能补齐证据」的形态一律进 §6 拒绝面。

2.1 调用面：ActionDesc 形状钉
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``INVOKESTATIC`` 描述符恰为 ``(III)I``。这是生成面 ActionDesc 指纹：CFF key processor 是 ``(II)I``，不同构不进本面（span 求值第一参 slot-fed，缺口 #4 余量面）；其余 desc 静默跳过，不计拒绝桶。

2.2 实参面：三常量喂
^^^^^^^^^^^^^^^^^^^^

逆向三个实参必须全为紧邻纯常量装载（``const_args`` 工具箱，NOP 跳过——squeeze 遗留落点不碍事）。任何一参来自槽装载/字段/栈底旧值即整站拒绝（``args-not-const`` 桶）——slot-fed 形在常量实参下不可求值，是 §1 说的 CFF key processor 桶同理的门。

2.3 前窗面：compareValue 走廊 + IF_ICMP 钉形
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

调用与分支之间必须恰为「compareValue 常量走廊 → 终点分支」：走廊 = ``[INT; (INT; IMUL|IAND|IOR|IXOR)*]`` 按 JVM 栈语义左折（imul/iand/ior/ixor 全交换律 + 结合律，左折即真值），折叠栈恰剩 1 个 i32 = compareValue；终点恰为 ``IF_ICMPEQ..IF_ICMPLE`` 六 op 之一。NOP 跳过。其它任何内容整站拒绝（``forward-shape`` 桶）；其中停点引用 ``ThreadLocalRandom`` 的（RandomBound 窗口变体，运行期随机）单独入 ``tlr-window`` 桶明确排除、与走样面分开记账（trace 可见）。

2.4 目标面：容器内 + static + 有码 + 初始化门
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

目标解析走容器方法索引（constant_call 同源 keyOf）：容器外 / 非 static / synchronized 逐桶拒绝；目标有码检查在求值段做（loc.index 是目标类下标，scan 会话是调用者类——错位检查是单测钉过的缺陷面）。类初始化门（constant_call 同源）：目标类 ``<clinit>`` 非中性时只留同类调用——JVMS 5.5 本类方法开始执行时类已在初始化中，同类 ``INVOKESTATIC`` 不触发初始化，删调用不移时机；门点 proc 与蹬床和门点同类是生成面事实，同类豁免站点的纯性由封闭环境求值独立证明。

2.5 求值面：唯一返回值 + 恒真裁决
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

跨方法求值（§3）必须返回 known i32，且 ``proc(left, right, salt)`` 与 compareValue 按 ``if_icmpxx`` 分支语义比较必须 = taken（生成面 ``require`` 保证的恒真自证）。判非 taken = 求值与生成面脱钩（mock 语义走样 / 目标错解 / 随机体），fail-closed 拒绝——罐 A 实测全量站点 not-true = 0，判据余量充足，这条门是防御面不是主判据。

.. _gate-spec-algo:

3 算法：跨方法求值流程
----------------------

Scan 两段：形状钉（全容器逐方法）→ 求值（按目标 owner 分组）。

**形状钉**\ 逐方法扫 ``INVOKESTATIC (III)I``，命中后逆向三实参 + 正向前窗解析 + 目标解析（§2.1–2.4），通过者暂记为候选站点（此时未求值）。

**求值**\ 把候选按目标 owner 分组，逐目标类开会话：

1. 类初始化门（§2.4）：非中性时整组过筛，只留同类调用；
2. 目标有码检查：目标方法无码 / 目标类会话开不了 → ``no-code`` 桶（未被任何目标类覆盖的候选——时间片止步或会话失败——统一按此记账，fail-closed 同口径）；
3. ``SimExecutor.invoke`` 跨方法求值：jsim 引擎按设计只跑单方法、不递归，\ **引擎单方法、编排递归**——编排层把「调用」接住，开新引擎实例跑子方法（同一 Session：堆与静态字段跨方法共享），\ ``__grt$indy$`` 转发器（``iload×3; invokestatic 真体; ireturn``）经这条递归穿透到真体；结果缓存 + 深度/步数预算（§7）；
4. known i32 出值 → ``gateVerdict(branchOp, R, compareValue)`` 恒真裁决（§2.5），通过者写回 ``Site.result``。

**mock 面（最小申报）**：只接管 ``java/lang/Integer.rotateLeft(II)I``——门点 proc 体的 PredicateStep AddSaltRotate/RotateBySalt 生成面 emit 这一条（IntExpression.kt 盐旋转步）；jsim 核心/num 包没有 JDK 数学面，缺这个接管整桶 EvalBadState（罐 A 供给链实测 4282 个 const-fed 门点全部因此存活）。接管语义 = JVM ishl/iushr 位移掩码 5 位：\ ``(i << d) | (i >>> (32 - d))``，\ ``d &= 31``\ （d=0 恒等）；实参任一 unknown 按未知口径传播不编值（折叠面 ``value-kind`` 桶拒）。

**刻意不装扩展包**：str/num/reflect/indy 扩展包全不装——门点 proc 体按生成面是纯算术 + rotateLeft，任何堆访问/字符串/indy 面都该在求值处 fail-closed 显形（TLR / material-key / indy 蹬床桶靠这条边界现形），装包会把它们误放进折叠面。罐 A 实测 eval-bad=3686/6139（原罐）正是这些面；上游 ConstantCall 专用求值面约 2453 个（Math.max / 扩展包语义）刻意留白，那些调用已被上游折叠，无 restoration 损失。

.. _gate-spec-rewrite:

4 改写：TAKEN 形折叠
--------------------

按类聚类 → 方法级 ``roundTripSafe`` 前置门 → promote 一次 → DOM 改写 → lower 全部候选方法（任一失败整类放弃，半改写态不外泄）。

``rewriteMethod`` 逐站五步：

1. 分支按 ``branchSrcPc + branchOp`` 重定位（跨会话稳定）；
2. 操作数区复核：从分支逆向越过前窗（宽松形定位）找到门点调用，成员全等；\ ``collectArgs`` 三实参 ``sameValue``；\ **正向重跑前窗解析**，终点分支 srcPc/op 与 compareValue 必须与 scan 全等（权威复核）；
3. 裁决复核：\ ``gateVerdict`` 仍 taken（纯函数下恒成立，防御面）；
4. 标签门：操作数区中段（right/salt/调用/窗内指令）或分支本体带真实合并点标签（有入边、栈形相容）= 合并点，整站放弃、字节不动；区域首条（left 锚）的标签安全（scan 不设此门——站点照常入改写面，由复核段拦；未引用标签在 promote 后不驻 DOM，测不出该门）；
5. TAKEN 改写：锚（left 常量装载）原地改 ``ICONST_0``\ （保 srcPc），分支本体原地换 ``IFEQ``\ （目标 = 原分支目标，保 srcPc），其余受害者（right/salt/invokestatic/窗内走廊）单批 ``eraseBatch``\ （``kRejectIfUsed``——异常表/帧锚占用即拒收，整站回滚 anchor 与分支，不留半改写态）。

改写不新增合并点、不插无条件 GOTO：HotSpot 线性走查要求无条件跳转后段首带帧，而被擦延续段在原 StackMapTable 中无帧——恒真条件跳转（``ICONST_0; IFEQ→真臂``）让延续段保持 fallthrough 可达，帧面零改动（PredicateFold TAKEN 形同款契约）。junk 臂原地保留成不可达码，交下游 DeadCode 清理。

.. _gate-spec-jasm:

5 前后 jasm 对照
----------------

夹具全部为 test/unittest/gate_processor_fold_test.cpp 的现场汇编 jasm（经 mjbase jasm 汇编 + major 钉 61 装载），块内逐字与夹具一致；每例标注对应测试名。

5.1 正例：三常量喂纯算术 proc（恒真折叠全流程）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

折叠前（``gateClass`` 夹具：\ ``TEST(GateProcessorFoldScan, FindsGateSiteAndEvaluates)``）：

::

   .super java/lang/Object
   .class public super fixture/Gate {
     .method public static proc (III)I {
       code: {
         iload_0
         iload_1
         ixor
         iload_2
         iadd
         ireturn
       }
     }
     .method public static m ()I {
       code: {
         bipush 10
         bipush 20
         bipush 30
         invokestatic fixture/Gate.proc (III)I
         bipush 60
         if_icmpeq Lreal
         iconst_0
         ireturn
       Lreal:
         iconst_1
         ireturn
       }
     }
   }

门点读法：left=10、right=20、salt=30 三常量喂 ``proc``；proc(10,20,30) = (10^20)+30 = 60；compareValue=60 单段走廊；\ ``if_icmpeq`` 恒真（60 == 60）→ 真臂 ``Lreal`` 恒走，junk 臂（``iconst_0; ireturn``）永远不达。scan 断言站点五元组（uri / targetOwner=fixture/Gate / targetName=proc / targetDesc=(III)I / branchOp=IF_ICMPEQ）、三实参 (10,20,30)、compareValue=60、求值 result=60。

折叠后（``TEST(GateProcessorFoldRewrite, FoldsToAlwaysTrueForm)`` 断言面）：

::

     .method public static m ()I {
       code: {
         iconst_0
         ifeq Lreal
         iconst_0
         ireturn
       Lreal:
         iconst_1
         ireturn
       }
     }

变化清单：锚（``bipush 10``）原地变 ``ICONST_0``；分支本体原地换 ``IFEQ``，目标 = 原真臂头（``iconst_1``）；right/salt/invokestatic/compareValue 四受害者单批擦除；\ ``proc`` 本体一字不动（交下游 DeadCode 回收）。测试断言：INVOKESTATIC=0、IF_ICMPEQ=0、IFEQ=1、方法首条=ICONST_0、IFEQ 目标=ICONST_1；proof 类数不增 + residual=0（折叠后同门不得再成站点）。

5.2 rotateLeft 盐旋转体（接管面哨兵）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``TEST(GateProcessorFoldRewrite, RotateLeftBodyFolds)``：proc 体 = ``iload_0; iload_1; invokestatic java/lang/Integer.rotateLeft (II)I; iload_2; iadd; ireturn``，实参 (3,1,2)、compareValue=8——rotateLeft(3,1)=6，6+2=8 恒真。rotateLeft 接管缺失则此桶整批 EvalBadState（罐 A 供给链 4282 站点的存活原因）；mock 语义走样（如错成 rotateRight）则判非恒真、不折叠——本测试同时是接管面的哨兵。

5.3 跨类 proc（求值穿透类边界）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``TEST(GateProcessorFoldRewrite, CrossClassProcFolds)``：门点在 ``fixture/GateX.m``、真体在 ``fixture/GateY.proc``\ （GateY 无 ``<clinit>``，初始化门放行）——跨方法求值必须穿透类边界，折叠只脏调用者类（dirty.uris = GateX 一类）。

5.4 ``__grt$indy$`` 蹬床转发器（编排递归穿透）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``TEST(GateProcessorFoldRewrite, ForwarderRecursionFolds)``：门点调 ``fixture/GateF.fwd``，转发器体 = 纯 iload 前缀 + 单 invoke + ireturn（IndyRestore 还原产物形）：

::

     .method public static fwd (III)I {
       code: {
         iload_0
         iload_1
         iload_2
         invokestatic fixture/GateF.proc (III)I
         ireturn
       }
     }

求值必须经编排层递归穿透转发器落到 ``proc`` 纯算术体；站点 targetName 断言为 ``fwd``\ （转发器即门点目标）。

5.5 拒绝面夹具（字节不动的反证组）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- ``TEST(GateProcessorFoldScan, TlrBodyFailsClosed)``：proc 体取 ``ThreadLocalRandom.current`` 进算术（RandomBound 体变体）——目标不在容器内、无 mock，求值 ``EvalBadState`` 整站拒绝，运行期随机面 fail-closed。
- ``TEST(GateProcessorFoldScan, TlrWindowRejected)``：\ ``TLR.current; pop`` 夹在调用与 compareValue 之间（RandomBound 窗口变体）——前窗停点具名识别入 ``tlr-window`` 桶，与走廊走样（``forward-shape``）分开记账。
- ``TEST(GateProcessorFoldScan, NotAlwaysTrueRejected)``：proc(1,2,3) = 6 != compareValue 7——生成面恒真保证被违反，判非 taken 即拒绝（``not-true`` 桶），绝不按非真方向改写。
- ``TEST(GateProcessorFoldScan, NonConstantArgsRejected)``：salt 来自 ``iload_0`` 槽装载——非紧邻常量，\ ``args-not-const`` 桶。
- ``TEST(GateProcessorFoldScan, PollutedWindowRejected)``：调用与比较之间夹 ``aconst_null; pop``——非生成面逐字形，\ ``forward-shape`` 桶。
- ``TEST(GateProcessorFoldScan, OutsideTargetRejected)``：\ ``(III)I`` 调用目标 ``fixture/Missing.proc`` 不在容器——``target-outside`` 桶。
- ``TEST(GateProcessorFoldScan, CrossClassClinitPresentRejected)``：目标类 GateY 的 ``<clinit>`` 写静态字段（非中性）且调用跨类——删调用会改变被调类初始化时机，\ ``clinit-present`` 桶（同类豁免见 §2.4）。
- ``TEST(GateProcessorFoldRewrite, MidRegionLabelBlocked)``：salt 常量上带真实合并点标签（双前驱 ``Lmid``）——scan 照常入站点，rewrite 标签门整站放弃、字节不动（dirty.failed=1）。

.. _gate-spec-reject:

6 拒绝桶清单（逐桶 fail-closed）
--------------------------------

任一桶命中 → 该站点原样保留（字节逐位不变），原因进 trace；候选改写中途失败 → 整站回滚。scan 面十二桶：

.. list-table::
   :header-rows: 1
   :widths: 20 44 36

   * - 桶
     - 触发
     - 语义
   * - target-outside
     - ``(III)I`` 调用目标不在容器内
     - 跨容器目标不可证（容器内字节码全称命题口径）
   * - not-static
     - 目标非 ACC_STATIC
     - 生成面 proc 全 static，非 static 形不是门点
   * - synchronized
     - 目标 ACC_SYNCHRONIZED
     - monitor 语义求值器不建模，不可证即拒
   * - no-code
     - 目标无码 / 目标类会话开不了 / 候选未被任何目标类覆盖（时间片止步）
     - 求值段统一 fail-closed 记账
   * - args-not-const
     - 逆向三实参非紧邻纯常量装载（slot-fed 形）
     - 常量实参下不可求值；CFF key processor 桶同理（缺口 #4 面）
   * - forward-shape
     - 前窗非「走廊 → IF_ICMP」逐字形（异物/走样/多值）
     - 非生成面发射形，反误伤门
   * - tlr-window
     - 前窗停点引用 ``ThreadLocalRandom``
     - RandomBound 窗口变体，运行期随机，\ **永久排除面**\ （与走样分开记账）
   * - clinit-present
     - 目标类 ``<clinit>`` 非中性且调用跨类
     - 删调用改变被调类初始化时机（JVMS 5.5 同类豁免）
   * - eval-bad
     - 跨方法求值 Bail（TLR 体 / material 蹬床体 indy 未还原 / indy 蹬床体 / 堆访问面）
     - 不装扩展包的 fail-closed 边界（§3），TLR/material/indy 面在此显形
   * - step-limit
     - 求值步数熔断（``maxStepsPerCall`` / ``maxTotalSteps``）
     - 预算帽（§7），超限不可证即拒
   * - value-kind
     - 求值结果非 known i32（unknown 传播）
     - 不可编值即不可裁决
   * - not-true
     - 裁决非 taken
     - 生成面恒真保证被违反 = 求值与生成面脱钩（mock 走样/目标错解/随机体），防御门

rewrite 面另有五桶直方图（``RwResult``，逐站复核拦截面）：\ ``branch-not-found``\ （srcPc+op 重定位不到）、\ ``region-mismatch``\ （操作数区复核不符）、\ ``verdict-changed``\ （复核裁决与 scan 不一致）、\ ``label-blocked``\ （§4 标签门）、\ ``erase-fail``\ （``eraseBatch`` 拒收，整站回滚）。语料 trace 口径（``GRUNT_TRACE=1`` 桶计数、\ ``GRUNT_GATE_PROCESSOR_FOLD_TRACE=1`` 逐条）与桶分解对拍见 :ref:`corpus-ledger` §1。

.. _gate-spec-budget:

7 预算与记账口径
----------------

**求值预算**\ （jsim 编排层 ``SimExecutor.Limits``，写死不做配置面）：递归深度上限 6（解密链互相调用的栈闸门）；单调用步数预算 200000、总会话步数预算 4000000（步数熔断 → ``step-limit`` 桶）；结果缓存 8192 条（同目标同实参不重跑）。门点 proc 体是直线短体，预算内余量极大——预算是反恶意形的帽，不是吞吐瓶颈。

**三段数据流**：\ ``ScanResult{sites, rejects, totalShape, 十二桶计数, classesSkipped}`` → ``DirtySet{uris, sites, attempted, failed, skippedByRoundTrip, failedByReason}`` → ``ProofResult{ok, failed, residual}``。桶计数与 rejects 逐条同源（每拒一条两边各记一次）：trace 行吃计数、trace 门吃逐条。

**Proof 门**：类数不增 + 脏类可重解析（``proof_tools::classesIntact``，效力门挂时间片）+ residual 复扫——脏类集上重跑全判据（形状 + 求值 + 恒真裁决），仍成站点数即 residual；改写 fail-closed 放弃的站点如实入账，求值失败面（TLR/material，从未进改写面）不计入。罐 A 单罐门实测 residual=0。

**Era 与注入面**：Era 不消费——门点 processor 是 v3-only 形态（v2 Controlflow 无此面），ActionDesc ``(III)I`` 形状锚自证家族归属；无旁证注入面（判据全容器自证，无 ``--tsv`` 类外部证据口）。

.. _gate-spec-evidence:

8 语料与测试对账
----------------

语料数字全账在 :ref:`corpus-ledger` §1（罐 A 供给链站点量表两行 GateProcessorFold 实测），要点：

- **boar 全 native 对照罐**：scan shape=13732 sites=0（proc 体 100% 随 NativePipeline 出 jar / indy 化，全数 fail-closed），类条目 CRC 与输入逐条全等——no-op 门成立；
- **罐 A 原罐单跑**：shape=13782，\ **7643/7643 折叠**\ （classes=721 failed=0 residual=0），not-true=0；类数 3074 不增；\ ``-Xverify:all`` 失败集与输入全等；
- **罐 A 组合链**：上游四 pass 数字与锚点逐项全等；本 pass shape=7968 sites=**4282/4282**\ （classes=720）residual=0（eval-bad=3686 = material/indy 体 3576 + TLR 体 110）；CffUnflatten islands=1543 不变；双跑产物逐条目 CRC 全等；
- **census 对拍**\ （独立字节级普查 r1 口径）：raw (III)I 13656 / const-fed 10177 / plain-foldable 4282 / verdict-true 4282——本 pass scan shape=7968、折叠 4282、not-true=0 与普查逐项同源；TLR 体 110 普查过宽判 eval-success，pass 正确落 EvalBadState（fail-closed 口径以 pass 为准）；
- **ConstantCall 分工量化**：5814 个门点调用被上游先折（= 原罐 shape 13782 − 链上 shape 7968），其中 3361 本 pass 亦可求值、2453 为上游专用扩展包语义面（刻意留白）。

单测 264/264（基线 251 + 本 pass 13 新增），夹具清单见 §5：正例求值 / rotateLeft 哨兵 / 跨类 / 转发器递归 / TLR 体 / TLR 窗 / 判非恒真 / 实参非常量 / 前窗走样 / 容器外目标 / clinit 暴露门 / 中段标签门 / proof+residual 归零（含在正例折叠测试尾部）。
