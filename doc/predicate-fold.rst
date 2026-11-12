.. _predicate-fold:

predicate_fold：常量操作数分支/switch 裁决
================================================================



形态
----


grunt 往直线代码里插常量 vs 常量的假分支，真分支是原逻辑，假分支是诱饵死块（调 JDK 方法再 throw 凑栈形状）：

::

   if (138795133 <= 21992595) {
       TT_MaxProfile.nmaxSizeOfInstructions((long) -1275583517581489779L);
       throw null;
   }


switch 的键为常量时同理：目标臂唯一确定。


判据（S1）
------------


1. 分支是 ``ifeq..if_le`` （1 操作数）、 ``if_icmp*`` （2 操作数）或 ``tableswitch/lookupswitch`` （1 键）；
2. 操作数由 **紧邻的纯常量装载** 产生、槽数严格吻合（ ``const_args`` 工具箱，NOP 落点跳过，squeeze 先行后被压住的操作数区重新可见）；
3. 操作数全部 I32（引用/长整型不在本残留范围）。

混淆器必须让假分支恒走真分支，否则自己的 payload 到不了，裁决是静态事实，无需求值器。


改写
----


taken → 恒真条件跳转形态 ``[ICONST_0, IFEQ→T]`` ：锚（区域首条操作数）原地改 ``ICONST_0`` （保 srcPc），分支本体原地换 op 为 ``IFEQ`` 、目标不变，其余操作数擦除；not-taken → 锚置 NOP，其余操作数与分支本体擦除。

**为什么 TAKEN 不落 ``goto_w``** ：HotSpot 的线性走查要求无条件跳转之后的段首必须带帧（可达与否不论，带帧死代码放行、无帧死段实测报 ``Expecting a stack map frame`` ）。被擦区域的延续段在原 SMT 中无帧（非合并点），插 ``goto_w`` 即炸；条件跳转让延续段保持 fallthrough 可达（语义上永不执行），帧面零改动。NOT-TAKEN 折叠留下的分支目标死块带原帧，合法，交下游 ``DeadCode`` 清理。

**区域中段或分支本体带标签即整站放弃** ，那是合并点，外部到达路径的栈形状与区域产出耦合；区域首条的标签安全（到达起点即拿到折叠结果）。


证明
----


``classesIntact`` + 改写前 ``roundTripSafe`` 检查（原样往返不了的方法整方法跳过， ``skippedByRoundTrip`` 计数）。
