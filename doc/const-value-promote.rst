.. _const-value-promote:

const_value_promote：clinit 常量写入回填 ConstantValue
==============================================================================================



形态
----


混淆器把 ``static final`` 常量的字面量抹掉，改成 ``<clinit>`` 里 putstatic：

::

   static {
       aGjy = 89737686;
       aaKO = 855681668;
       aGUJ = 424665907;
   }


反编译出来一坨静态块，常量无法内联传播。这正是 JVM 标准的 ConstantValue形态被人工降级的样子。


判据（S1）
------------


1. 字段 ``static`` ，描述符为基本类型或 ``Ljava/lang/String;`` （ConstantValue的合法形态），尚未带 ConstantValue；
2. 访问面普查（ ``field_census`` ，promote 精确口径）：全容器该字段 **只有一次写** ，落在本类 ``<clinit>`` （读不限）；
3. putstatic 紧邻前驱（NOP 跳过） **一步** 产生全部槽位的常量装载（int 族 / ldc String / ldc2_w long+double； ``aconst_null`` 不是合法ConstantValue 形态，拒绝）。


改写
----


值指令置 NOP、putstatic 擦除（孤儿帧先剪再 lower）；字段补 ``ACC_FINAL`` （ConstantValue 只对 static final 生效，这也是语义的一部分：从此任何putstatic 都非法，所以唯一写入检查是前提不是可选项）并挂 ConstantValue 属性。

libjbc 0.4.15 未建模该属性：用 ``RawAttr`` 持 2 字节大端 CP 索引（属性内容就是 constantvalue_index 一项，JVMS §4.7.2）。


证明
----


``classesIntact`` 。JVM 类初始化时按 ConstantValue 赋值，语义与原 clinit 写入等价（唯一写、纯常量）。
