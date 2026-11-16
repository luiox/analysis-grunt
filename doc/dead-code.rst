.. _dead-code:

dead_code：不可达指令码清理
====================================



形态
----


predicate_fold 裁决掉假分支后，诱饵块从此再无控制流入口；return/athrow 之后的残段、永不命中的 switch 臂同理。这些指令还在字节布局里，反编译出来就是死块。

另有**冗余无条件跳转**（goto/goto_w 的目标是紧邻落点指令）：混淆器用这对（goto + 落点标签）把常量表达式 / 调用实参区从中间劈开，下游 NF/ArithFold 的常量窗口与 PAF 三元组都被落点上的真标签挡死（q8 样本 b/u 族池读 + L 族 junk 谓词实证）。跳转与顺序执行语义全等，擦除即穿线。


判据（S2）
------------


从入口做控制流可达性 BFS：顺序后继 + 分支/switch 全目标；return/athrow/ret 无后继。可达集合之外的指令即不可达（fall-through 已计入，不存在歧义）。

**异常表是隐藏控制流边** ：保护区间内任一可达指令都可能抛出并进入 handler，所以 handler 的可达性不能只看有没有显式跳转。做法是以区间与可达集相交为根把 handler 也纳入 BFS，并迭代到不动点，handler 自己落在别的保护区里时继续向外传导。带异常表的方法不再整体跳过（grunt 方法几乎全带 try/catch，跳过等于永不清死区）。

冗余 goto 与死指令同入站点面（ ``rewrite_tools::isFallthroughGoto`` 工具箱单源：落点 srcPc == 自身 pc + 指令宽的 backing 布局真值判据； ``dom.order`` 相邻判据在真 jar 上静默漏站点——order 不保证 pc 升序，b.main 族蹦床全漏实证）。


改写
----


先收敛异常表，再擦指令：

- 区间全死 → 删表项（这段代码抛不出异常，handler 也进不来），其端点/handler 标签不被任何存活表项引用时一并销毁；
- ``start`` 锚在死指令上 → 向前吸附到区间内第一条存活指令；
- ``end`` 锚在死指令上 → 向前吸附到其后第一条存活指令，尾部全死则落方法末哨兵（ ``bindLabel`` 无参形态）。

吸附用 ``bindLabel`` 搬移旧 Boundary 标签，句柄身份不变，表项引用不动；造新引用而不销毁旧标签会被 ``eraseBatch`` 的锚定保护整批拒绝（批内成员被存活 Boundary 标签锚定 → Err 且什么都不删）。吸附必须发生在擦除之前：吸附目标须在存活指令上。吸附规则与 ``frame_rewire`` 对 LNT/LVT 的悬垂处理一致，区间吞并被删段，不产生悬空端点。

随后 ``eraseBatch`` 整批擦除，布局缩短交 lower 的 pcmap。

穿线批独立先行（死区批异常时穿线成果不连坐）。victim 可达，存活分支 / 异常表可以正跳到它——入边标签不先行处置会被锚定保护整批拒绝（b.main 族实证）。处置规则（ ``rebindThreadingLabels`` ）：有引用者（批外分支 / 存活异常表） ``bindLabel`` 搬到最终落点（链形冗余沿目标链穿透到批外落点，句柄身份不变，语义逐位等价）；无引用者 ``destroyLabel`` 销毁——victim 的目标标签锚在落点（存活指令）上，擦除后若不销毁则成为死标签，落在常量窗口中段会把 ArithFold 的「窗口内除锚点外无标签」判据挡死（b.main hint 块池读实证）。


帧
----


死区面不剪帧。改写走 ``rewrite_tools::mutateMethodWithFrames`` （promote 一次 → mutate → ``prepareNestedAttrsForLayout`` → ``lowerMethod`` → ``rewireFramesAfterLower`` ），帧由 Strict 求解从改写后的代码真值整体重算。

穿线面在 Strict 求解不支持的方法（junk 走廊合并点栈形不一致，q8 b.main 族 20 法）退化为**声明帧口径**（ ``threadGotosDeclaredFrames`` ：prunePcAttrs + 处置入边标签 + eraseBatch + lowerMethod，不重推断）。依据：声明帧本就 verifier 有效（原 jar 可加载运行即证据），且擦 goto-to-fallthrough 不改任何剩余边的栈形（目标帧 = 落点帧）；同方法上 ConstantCall 的改写路径同构实证可落地。死区面维持 fail-closed 不动。


判据（S2）
------------


从入口做控制流可达性 BFS：顺序后继 + 分支/switch 全目标；return/athrow/ret 无后继。可达集合之外的指令即不可达（fall-through 已计入，不存在歧义）。

**异常表是隐藏控制流边** ：保护区间内任一可达指令都可能抛出并进入 handler，所以 handler 的可达性不能只看有没有显式跳转。做法是以区间与可达集相交为根把 handler 也纳入 BFS，并迭代到不动点，handler 自己落在别的保护区里时继续向外传导。带异常表的方法不再整体跳过（grunt 方法几乎全带 try/catch，跳过等于永不清死区）。


改写
----


先收敛异常表，再擦指令：

- 区间全死 → 删表项（这段代码抛不出异常，handler 也进不来），其端点/handler 标签不被任何存活表项引用时一并销毁；
- ``start`` 锚在死指令上 → 向前吸附到区间内第一条存活指令；
- ``end`` 锚在死指令上 → 向前吸附到其后第一条存活指令，尾部全死则落方法末哨兵（ ``bindLabel`` 无参形态）。

吸附用 ``bindLabel`` 搬移旧 Boundary 标签，句柄身份不变，表项引用不动；造新引用而不销毁旧标签会被 ``eraseBatch`` 的锚定保护整批拒绝（批内成员被存活 Boundary 标签锚定 → Err 且什么都不删）。吸附必须发生在擦除之前：吸附目标须在存活指令上。吸附规则与 ``frame_rewire`` 对 LNT/LVT 的悬垂处理一致，区间吞并被删段，不产生悬空端点。

随后 ``eraseBatch`` 整批擦除，布局缩短交 lower 的 pcmap。


帧
----


本残留不剪帧。改写走 ``rewrite_tools::mutateMethodWithFrames`` （promote 一次 → mutate → ``prepareNestedAttrsForLayout`` → ``lowerMethod`` → ``rewireFramesAfterLower`` ），帧由 Strict 求解从改写后的代码真值整体重算。


链序约束
--------


**``grunt.PredicateFold`` 之后必须跟 ``grunt.DeadCode``** 。PredicateFold 折叠假分支后留下的死区里可能还有 switch，其分支目标没有帧（Strict 只为可达代码生成帧），链尾导出时 HotSpot 会报Expecting a stackmap frame at branch target。死区由本残留清掉，中间态才合法。

**本残留清完走廊后，下游要再跟一轮折叠** 。死区擦除会让跳过走廊的 goto 退化为冗余（目标 = 新落点），下一轮 DeadCode 穿线掉它——但常量窗口的折叠（NF/ArithFold）若都排在穿线之前，被标签挡死的窗口就再无机会收拢（q8 b.main hint 块池读实证：DC 走廊清场在位置 19，两轮 ArithFold 在 9/12，窗口复扫 const=0）。链尾每轮 DeadCode 之后补 ConstantCall/NumberFold/ArithFold（需要池读面时再补 PoolArrayFold），折叠口径才收敛。 ``--converge`` 驱动下这两条约束由轮维度机械吸收（下一轮重扫），同轮排序只保效率——对照表见 :ref:`obf-behaviors-contract`。


证明
----


``classesIntact`` + 改写前 ``roundTripSafe`` 检查。
