.. _seed-strip:

seed_strip：clinit 的 int[] 种子建表拆除
================================================================



形态
----


混淆器把跨类常量藏进静态 ``int[]`` ，在 ``<clinit>`` 里建表：

::

   static {
       int n = (int) 422104587L;
       yA.bfAT[0] = n; yA.bfAT[1] = ...;
   }


建表段形状固定（grunt 生成器形态，S2）：头 = ``NEWARRAY + PUTSTATIC F`` ，随后任意多个 ``[GETSTATIC F, 值链, IASTORE]`` 存储组，组内栈深平衡、以IASTORE 归零收尾。


判据
----


1. 字段是本类声明的 ``static [I`` ；
2. 访问面普查：该字段的 **全部读和写** 都在本类 ``<clinit>`` （类外/类内其他方法有任何访问即放弃，上游 array_fold 未折光读面时删建表会破坏语义）；
3. 建表段按上述全有或全无形状扫描成功：值链词汇表 = 常量装载 / 栈整形 /整长算术与转换（ **除法族排除** ，可能抛算术异常；局部变量读写不进词汇表，跨度内写、跨度外读会破语义）；中段有标签放弃。


改写
----


逐跨度擦除（首条 NOP 保标签锚与 srcPc，其余 eraseBatch）。擦除后 **不手剪SMT 孤儿帧** ，改写走 ``rewrite_tools::mutateMethodWithFrames`` 的正统帧链（prepare → lower → ``rewireFramesAfterLower`` Strict 重算），帧内容永远从改写后的代码真值再生（见 rewrite_toolbox.hpp 头注）。


统计
----


``fields`` 计数拆除的种子字段。clinit 拆空后由 dead_member 按拆空形状删整方法；字段本体成为零访问孤儿数组，交 dead_member 的孤儿检查删除。
