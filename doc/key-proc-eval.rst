.. _key-proc-eval:

key_proc_eval：v3 key processor 夹心跨方法静态求值（cff 桥解锁面）
================================================================================

v3 ControlflowFlattening Mixed 模式 50% 的桥走 processor 形（与 inline
span 同一 state 槽协议；源锚 ``grunt3@e41e852``
FlowControlFlowFlattener.kt ``emitProcessorStateProgram``），生成面逐字：

.. code-block:: text

    [INT(inputKey); ISTORE(slot); ILOAD(slot); INT(salt);
     INVOKESTATIC proc (II)I; ISTORE(slot)]

``proc`` 是 CFF key processor（ActionDesc 恰为 ``(II)I``，与 gate
processor 的 ``(III)I`` 不同构）。键在 proc 调用后，cff_unflatten 的 span
求值器只认 inline 闭链，processor 形整岛 fail-closed
（\ ``SpanKind::Processor``）——岛留下，CFR 0.152 SwitchReplacer 对该形
抛 ``ConfusedCFRException``，方法整体落 ``throw new
IllegalStateException("Decompilation failed")`` 兜底（jsoup v3_full 实测
251 文件 295 方法，其中 246 个是 dispatcher 残岛直接触发）。本 pass 补
obf-behaviors 缺口 #4 明记的解锁路径：跨方法静态求值
``proc(inputKey, salt)``，夹心改写成 ``[INT(result); ISTORE(slot)]``
——桥退化成纯 inline span，cff_unflatten 既有机制自然折叠整岛。


判据
----

1. ``INVOKESTATIC`` 描述符恰为 ``(II)I``\ （生成面 ActionDesc 指纹；gate
   processor 是 ``(III)I``，不同构不进面）；
2. 夹心逐字紧邻（DOM 序，NOP 不跳——squeeze 遗留 NOP 出现即非生成面形，
   静默非候选）：\ ``prev4`` int 常量装载（inputKey）、\ ``prev3``
   ISTORE(slot)、\ ``prev2`` ILOAD(slot)、\ ``prev1`` int 常量装载（salt）、
   ``next1`` ISTORE(slot)，三处 slot 全等；
3. 目标解析在容器内 ∧ static ∧ 非 synchronized；
4. 类初始化检查（constant_call / gate_processor_fold 同源）：目标类
   ``<clinit>`` 非中性时只留同类调用（JVMS 5.5，改写调用不移时机）。


纯性链（为什么 proc 体静态可求值）
----------------------------------

ReferenceObfuscate（chance=1.0）把多数 key processor 调用包进 indy，
IndyRestore 还原后 proc 体是直线纯算术（jsoup v3_full
``DataUtil$KeyProcessor$*`` 逐类实证：\ ``iload_0; iload_1; iadd; iload_1;
iadd; iload_1; isub; ldc; iadd; ireturn`` 形）；残存 indy 转发体经
IndyRestore 轮次解锁后同形。求值走 jsim 编排层（过程内递归进转发体/真
体）+ ``Integer.rotateLeft(II)I`` 接管（盐旋转步，gate processor 同款）；
任何堆访问/字符串/indy 面在求值处 fail-closed（拒绝计数逐项分列）。


改写与验证
----------

夹心五条改两条（\ ``[INT(result); ISTORE(slot)]``，srcPc 锚保，帧链整
体重算）。jsoup v3_full 实测（build13 主线 trace）：**sites=6241 → 261
类 6241/6241 全改写，failed=0、residual=0**，拒绝面全零；第二轮起 shape
面归零（岛已被 cff_unflatten 折叠，不动点吸收）。

unittest 7 测试：夹心形状钉与改写、(III)I 不同构拒证、跨类调用 clinit
守卫、求值 fail-closed 面、干净类零搅动等。
