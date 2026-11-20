.. _trampoline-squeeze:

trampoline_squeeze：跳到下一条的 goto 蹦床
==================================================================



形态
----


grunt 打乱基本块布局后，用跳到布局下一条的 goto 缝合控制流：

::

     iconst_1
     goto +0          ; 目标就是物理下一条
     iconst_2         ; 实际后继


这类蹦床不表达任何语义，但把紧邻操作数区切断了：下游残留（predicate_fold / constant_call）按消费点往前紧邻收集找操作数，蹦床夹在中间就整站看不到。


判据（S2）
------------


``goto/goto_w`` 的目标解析后 == 布局顺序后继。判据走 ``rewrite_tools::isFallthroughGoto`` 工具箱单源（落点 srcPc == 自身 pc + 指令宽的 backing 布局真值； ``dom.order`` 相邻句柄判据在真 jar 上静默漏站点——order 不保证 pc 升序，q8 样本 b.main 族蹦床全漏实证，2026-11-16 修）。


改写
----


原地置 NOP、保 ``srcPc`` ，不擦除、布局不缩短，SMT/属性面零扰动。NOP 落点由 ``const_args::collectArgs`` 跳过，被压住的操作数区重新可见。

目标标签残渣处置（2026-11-16，与 DeadCode 穿线同根因）：标签锚在落点（存活指令）上，NOP 化后蹦床不再引用它——再无引用者时销毁（ ``destroyLabel`` ），否则它落在常量窗口中段会把 ArithFold 的「窗口内除锚点外无标签」判据挡死；有引用者保持原锚（落点即语义落点）。


证明
----


``classesIntact`` ：类数不增 + 脏类可重新解析。
