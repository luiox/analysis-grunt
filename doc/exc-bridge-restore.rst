.. _exc-bridge-restore:

exc_bridge_restore：ControlflowJump 异常桥还原
================================================================================



形态
----


Grunt v3（ ``ControlflowJump`` ，grunt3@e41e852， ``exceptionBridgeChance > 0`` 时启用 ``FlowExceptionBridgeInserter`` ）把一条 Real 边改道进一个合成 trap 块、再经异常表跳回原目标（skidfuscator 同源面）。生成面逐字（ ``createExceptionTrapBlock`` / ``createExceptionHandlerBlock`` ）：

::

   原边入口（GOTO / IFxx 分支臂 / tableswitch case 臂）:
     ... goto Ltrap                 // edge.to = trap：原边被改指
   Ltrap:                           // trap 块（布局尾追加，多桥相邻成排）
     new java/lang/RuntimeException
     dup
     invokespecial java/lang/RuntimeException.<init> ()V
     athrow                         // 异常表项 [Ltrap, Lafter) → Lh
   Lafter:
   Lh:                              // bridge handler 块（随机落位）
     pop                            // 接住 RuntimeException 丢弃
     goto Lx                        // Synthetic goto → 原目标


异常表项 catch_type = ``java/lang/RuntimeException`` ，保护区恰为 trap 块四条指令（JvmFlowExporter ``emitExceptionRegions`` 按连续 run 出表，单块保护区 → 表项恰罩 ``[NEW, ATHROW]`` ），handler 恰为 ``POP; GOTO`` 两块。 ``maxBridgesPerMethod=4`` （每方法上限 4 桥，样本 A 实测 failed 站点平均每方法 4 条与此吻合）。

**位置落入变体** ： ``emitGoto`` 只在 ``mode==Fallthrough && nextBlock==target`` 时省略 GOTO，trap 恰好布局紧邻前驱块时，前驱（生成面 state span 尾 ``istore``/``iinc`` 或条件分支落空臂 ``ifle`` ）直接位置落入 trap 头，原边无显式 goto。样本 A 实测 1449 个位置落入站点 vs 6976 个显式跳转站点。

**handler 位置守卫** ：handler 随机落位后，exporter 对每个布局前驱显式补 GOTO（省略条件对 handler 永不成立），所以 handler 头（POP）不存在位置落入边，trap 头是唯一的位置落入面。


为什么可以静态还原
------------------


还原 = 把改道拆掉：任何到达 trap 头的执行路径（显式跳转或位置落入）都无条件 ``ATHROW`` → 被本桥表项接住 → ``POP`` → ``GOTO Lx`` ，所以 **trap 头的全体前驱直连 Lx 是语义恒等** ，随后 trap/handler 六条指令与异常表项整体擦除。生成器把这是合成桥的全部证据留在类文件里：

- **形状钉** ：trap 四条逐字（ ``NEW RTE; DUP; INVOKESPECIAL RTE.<init>()V; ATHROW`` ，保护区恰 4 条）+ handler 两条逐字（ ``POP; GOTO|GOTO_W`` ）+ catch_type 恰为 RuntimeException。真实 ``catch (RuntimeException)`` 的保护区是用户代码段（≠ 4 条 trap 形）或 handler 带异常槽存储（javac 形 ``ASTORE`` ≠ ``POP`` ），形状钉天然拒绝，样本 A 实测 loose-vs-strict 差仅 2 条、第 3 轮 复扫反误伤 0。
- **核对检查** （真实 throw 区不可能同构的剩余面）：其他表项保护区不覆盖 trap 头/handler 头（覆盖 = 派发歧义，拒）；其他表项 handler 不锚在本桥两块内（擦即侵占，拒）；handler goto 无字段引用（直接跳入会 POP 空栈，校验器拒绝）；非头受害者（DUP/INVOKESPECIAL/ATHROW/POP/goto）无字段引用（生成面块只从 trap 头进入）；桥目标 X 可解析且不落任何候选桥受害者集（X 进受害者集 = 改指进将擦/保留块，两态都错，拒）。
- 样本 A 全量站点上覆盖检查 / handler 碰撞检查 / X 落点检查命中全 0，生成面自恰，判据余量充足。

判据全为 S2 结构事实（形状逐字 + 异常表核对），无 Era 检查（v2 Controlflow 无异常桥面），无旁证注入，jsim 不需要（无求值面）。


关键事实（判据的地基）
----------------------


- **优先级尾部** ：生成器把桥表项追加在异常表尾（priority=last），删除不影响先前表项的派发次序。
- **trap 相邻成排** ：trap 块布局尾追加 → 多桥 trap 共享边界（前桥 end 锚 = 后桥 trap 头）；两桥互为邻居时覆盖检查不互斥（区间 exclusive 端），还原需支持同方法多站点。
- **真实表项 end 锚吸附** ：真实 try 区若紧邻追加 trap，其 end 锚（exclusive）恰锚在 trap 头上，擦桥前必须把这类锚前移吸附到下一存活指令（dead_code 同形），否则 Boundary 标签保护锚点会让 ``eraseBatch`` 整批拒绝。
- **X 的可达性不需要证** ：X 是原边的原始目标（桥只是插入物），原程序本就可达；census 实测 X_unreach=0。
- **与 CFF 岛的关系** ：样本 A config 里 ControlflowFlattening（order 17）先于 ControlflowJump（order 18），桥插在已摊平代码上，X 落岛头的桥 5657 个、trap 头落岛 0、sp_island 0，互不阻塞。


链序
----


**``grunt.ExcBridgeRestore`` 排在 ``grunt.CffUnflatten`` 之前 ** ：桥摘除后，CFF 的 state span goto 从经 trap 绕行恢复为直达岛头的整块形，岛识别与 span 求值的输入更干净。样本 A 验证（同供给链 ``IndyRestore,StringPoolFold,ConstantCall`` 之上）：不加本 pass CffUnflatten 还原岛 1281，加本 pass 后 **1543（+262，+20.5%）** ；rejects 面同步收窄（foreign-ref 487→292、shape-mismatch 3073→1866）。组合链 ``-p grunt.IndyRestore,grunt.StringPoolFold,grunt.ConstantCall,grunt.ExcBridgeRestore,grunt.CffUnflatten`` 产物 ``-Xverify:all`` 失败集与输入全等（227 NCDFE，VerifyError=0）。


边界（不还原的面）
------------------


- **processor 检查（缺口 #10 面 a）** ： ``INVOKESTATIC (III)I`` processor 返回值参与的 gate 需要跨方法静态求值 processor 体，不属本残留。样本 A 复测（r2 链）：gate 6259 站全数 const-fed（slot-fed 0），目标链分类 3387 indy_body + 2872 impure + 0 pure，全部深水，跨方法求值是下一批的活（ConstantCall 纯算术先例可复用）。
- **改写失败面（fail-closed）** ：Strict 帧重算类型冲突的方法（样本 A 31 个方法 / 124 站点，DecompilerCrasher 畸形 SMT 邻接面），DOM 改写已随 lower 提交、帧保留 lower 形、 ``-Xverify:all`` 全绿，但 ``dirty.failed`` 如实计数且不计入 proof 检查；residual 复扫为 0（站点已消失）。
- 拒绝面四桶（shape / covered / handlerCollide / xInvalid / rebind）全部原样保留、字节不动，逐桶进 trace。


语料验证（样本 A，`doc/corpus-ledger.rst` §1）
--------------------------------------------------------------------------


- **boar（对照 jar， ``exceptionBridgeChance=0.0``）** ：scan sites=0（rejects=1111 全 shape 桶 = 真实 try 区与杂形 RTE 表项，正确拒绝），类条目 CRC 与输入逐条全等（对照零站点 pass 产物逐字节全等，jar 级差异仅 CLI writer 的 MANIFEST 重建与目录桩，纯 writer 行为），no-op 检查成立。
- **样本 A（ ``exceptionBridgeChance=1.0``） ** ：scan sites=13566 / 726 类（census strict 口径 13458，Δ=108 为 census 解码器与 promote 视图差）；rejects=4702 全 shape 桶（真实 try 区反误伤面）；rewrite classes=726 methods=4451 **bridges=13442 pins=1448** （≈ census live_fall=1449，Δ1 在 failed 站内）insns=80652 failed=124 proof=ok residual=0；独立字节级 census 复扫 第 3 轮 **bridge=0** （jar 内 13458 严格形桥全消）；类数 3074 不增； ``-Xverify:all`` 失败集与输入全等（227 NCDFE 外部依赖，VerifyError=0）；双跑产物逐条目 CRC 全等。
