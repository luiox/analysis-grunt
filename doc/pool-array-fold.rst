.. _pool-array-fold:

pool_array_fold：池数组读点的求值通道折叠（读点 → ldc 明文）
================================================================================

模拟执行池类 ``<clinit>`` 读回数组终态，\ ``GETSTATIC; idx; AALOAD`` 三元
组原地折 ``ldc`` 明文。

**与 string_pool_fold 的分工**：\ :ref:`string-pool-fold`\ 是 Grunt
vanilla 池三件套的**形状钉**通道（池分配前导 + arrayInit 逐块钉形 + 解密
器三窗口静态解码），fork 变体（junk 包裹解密器、seed 走廊
``parseUnsignedLong`` 计算、填充方法独立成体）钉不上。本 pass 是同一残留
族（池读点 → ldc）的**求值通道**：不钉池链形状，判据要求数组字段形态 +
读点三元组形状 + clinit 求值收敛（运行期算什么明文就是什么，与
ConstPoolRestore 求值通道、DeadMember 池明文提取同一哲学）。两通道共存：
形状钉先吃 vanilla 面，求值通道收 fork 残余。


形态（样本 C 实测口径）
-----------------------

池类持有 ``static String[]`` 字段（伴生类伴生字段，读点全部同类——业务
类调池类静态方法取串，数组读点不跨类），\ ``<clinit>``\ （直连或经填充方
法，编排层接力）以任意计算链填池：解密器调用、seed 走廊、junk 包裹、恒
真谓词。读点统一三元组形：\ ``GETSTATIC <池>; <int idx>; AALOAD``。


判据分级
--------

- **S2 结构事实**：字段 static + 描述符 ``[Ljava/lang/String;``；读点三
  元组紧邻（NOP 跳过）且跨度无标签（合并点 = 外部到达路径栈形状不可知，
  fail-closed）；下标常量。
- **S1 语义后果**：JVM 类初始化语义——任何主动使用先跑 ``<clinit>``，读
  点处的数组终态由 clinit 唯一确定；模拟执行忠实收敛即得终态。保守面
  （该 (owner, field) 整组放弃）：clinit 求值 Bail / 步数熔断；非
  ``<clinit>`` 方法存在 ``PUTSTATIC`` 池字段或 ``GETSTATIC`` + ``AASTORE``
  共存（运行期改写嫌疑，无法排除就保留）；读点下标越界 / 元素 Unknown；
  明文含孤立代理（\ ``ldc`` 常量池承载不了）。
- **无 Era 检查**：判据不依赖 Grunt 世代形状（求值通道的立意），家族检查
  由三元组 + 数组字段形态自证。


改写
----

对齐 string_pool_fold 读点折叠口径：锚点（getstatic）**原地换** ``ldc``
明文（srcPc 保留，pcmap/调试行落点不变），三元组其余两条 eraseBatch 擦
除；帧链 mutateMethodWithFrames 整体重算，前置 roundTripSafe。
**池字段 / clinit / 解密器不删**，读点归零后交 DeadCode / DeadMember /
垃圾类面统一回收。

真 bug 在案：改写写回不走 Strict 帧再生——CFG 推导对 junk 谓词走廊方法
会把 append 帧的 locals 基算丢（样本 C ``c.a()`` 实证 VerifyError:
locals[0] 应为 this 而 stackmap 声明 double），改走 lower 保真重定位
（pcmap 平移 SMT/LVT/LNT，帧内容不动）。


验证
----

样本 C（fork 变体语料，台账 2026-11-14 批）：21 个池字段组 / 198 个同类
池读点 / 129 个零 indy clinit；求值 eval-bailed **0**\ （21 组全部收敛）；
三元组 72 站、回填 **71**\ （1 站 roundtrip 不安全 fail-closed）、
write-suspect 1 组、residual 1；改写 jar 全类 ``-Xverify:all`` 加载通过。

jsoup v3_full 实测（build13 主线 trace）：sites=0（259 个池类 triple-hits
=0、eval-bailed=322 组宁留）——vanilla 面由 string_pool_fold 吃尽，本
pass 求值通道如实 0 动作，符合分工预期。

unittest 4 测试：三元组折叠与锚点保位、write-suspect 整组放弃、
roundtrip 不安全 fail-closed、干净类零搅动。
