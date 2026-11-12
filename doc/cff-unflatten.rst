.. _cff-unflatten:

cff_unflatten：v3 控制流摊平反演
================================================



形态
----


Grunt v3 ``ControlflowFlatteningTransformer`` （grunt3@e41e852， ``FlowControlFlowFlattener`` 逐行，jbc jasm 样例 boar-main.jar``net/spartanb312/obf/AD.a`` 核对）把 maximal region 块序打乱收缩，每个方法留下一个state 槽 + dispatcher 岛 + 常量供给桥的三件套：

::

   // 方法头入口桥（每条入边一座，inline span 闭链）：
   <int baseKey>; <int c>; IXOR|IADD|ISUB|IMUL|IDIV|INVOKESTATIC Math.max (II)I; ...
   istore <s>                    // s = method.locals.nextSlot（每方法新槽）
   goto Ldisp

   // dispatcher 岛（每出口一座；同方法多岛共享 s）：
   Ldisp: iload <s>
   tableswitch { min: lo, max: lo+n-1, cases: {…}, default: Lalias }

   // 真块之间靠 goto 串回岛头再分发；真块自身不再带键


- **键窗 [lo, lo+n) 连续、case 乱序** ，键只决定真块次序，不参与运算。
- **default 落岛首真块（alias face）** ：入口键的 case 目标与 default 同址，出表键一律归岛首真块。
- **scramble 假桥** ：键落窗外 goto 同一 default（不可达装饰，剪除面）。
- **Mixed 模式** ：50% 桥走 processor 形 ``[INT; ISTORE s; ILOAD s; INT salt;INVOKESTATIC proc (II)I; ISTORE s]`` ，boar 侧 proc 即 ``__grt$indy$`` 蹦床（Rename 后）。
- **handler 桥同形** ：异常处理入口也是一座桥，异常表 handler 标签锚在桥头。
- **junk 检查/载体** ：方法头的 junk 变换在桥与岛之间插条件 guard 块（无状态， ``iconst_5; bipush 9; if_icmplt …`` ），guard 分支最终指向岛头。


为什么可以静态还原
------------------


反摊平要回答桥给出的键是什么 → 该 goto 哪个真块。v3 的答案全在类文件里：桥的 span 是 **常量闭链 ** （base 与链内操作数全是装载期常量，运算封闭），键表是 tableswitch 的 ** 静态编码** ，两半相拼即完整块序。恢复规则只有一条：

桥求值出表内键 k → 前驱直连 cases[k-lo]；出表 → default（alias face）。即使岛被误判，直连 default也与原经岛分发落 default逐位等价， **语义安全的下限是恒等的** 。


所以判据全是容器自证事实（S2）：state 槽全方法读写 confined（ILOAD 只在岛头或 processor 夹心内、ISTORE 只在 span 尾）、岛形两条块、桥 span 可解、default ∈ case 目标。任何一环不成立整岛 fail-closed，岛被擦一半的中间态比不擦更危险，拒绝是唯一安全面。

**无 Era 检查** ：v2 Controlflow 是 junk 分支面（无 state 槽摊平），岛形自证家族；SSA 变体（ ``FlowIRSSARoundTrip`` 后）不同构，LookupSwitch +per-case stub + passArgs 编组，遗留缺口 #4 余量，不在此面。


关键事实（判据的地基）
----------------------


- **InvokeDispatcher 原生头是天然反误伤面** ：分发器方法本体 ``[ILOAD key; TABLESWITCH; default 回跳方法头]`` 同样命中iload +tableswitch候选形，但 default 回指岛头（自环）且 ∉ case 目标（alias face 缺席）→ ShapeMismatch 整岛拒。只有 InvokeDispatcher 一个来源的正例 jar290 候选全数此桶，字节逐位不动。
- **processor 桥 fail-closed** ：键在 ``__grt$indy$`` 蹦床后的 processor 体里，静态不可解 → 整岛保留（boar 语料 6201/8358 拒绝来自此桶）。解锁路径 = NumberFold 先解 processor 体（缺口 #4 余量），本 pass 不猜。
- **usersOf 只看字段引用** ：条件分支的落空臂是位置边，字段引用看不见，岛头（被擦块）的布局前驱若有位置落入，擦除后位置边改落邻接后继。已归类的条件载体紧邻岛头时补 GOTO 钉回 case 目标，其余整岛拒。
- **保留形桥** ：条件形桥（cc）与带 junk 前缀的桥（span 起点非块首）一律保留块体只重指 goto，junk 可达且不可证伪，擦块体会让前缀失去执行。
- **入口块归位有退避** ：入口真块在异常保护区内 → 蹦床回退（不移动，入口桥 goto 改指）；带 junk 前缀 → 同样不移动（前缀必须保持执行）。
- **改写粒度 = 岛，回滚粒度 = 方法** ：任一步改写失败，方法在干净会话重放；重放再失败整类放弃，无帧的半落会话绝不落盘。


与相邻形态的区别
----------------


- **DispatcherRestore（InvokeDispatcher 反查）** ：那边把调用点 + 分发器方法还原成真实 invoke；本 pass 把方法体内的块序还原成直线序。两者互不越界，分发器方法原生头在本 pass 是反误伤面（见上），调用点站点不参与岛形。boar 语料验证：分发器头锚缺席由 post-Redirect junk前缀所致（非 CFF 摊平），本 pass 不解锁该面（见 impl 语料记录）。
- **TrampolineSqueeze** ：那边只 NOP跳顺序后继的蹦床；本 pass 的桥块是有语义的键供给，不与其争站点。
- **PredicateFold / DeadCode** ：本 pass 擦岛后留下的 scramble 残块（保留形重指 default 后不可达体）交它们收尾；本 pass 不删保留形。
- **NumberFold（缺口 #4 余量联动）** ：processor 桥的键藏在 ``__grt$indy$`` proc 体里，NumberFold 解出后本 pass 的 span 求值词表自然接管；当前该桶 fail-closed 如实计数。
