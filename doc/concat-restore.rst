.. _concat-restore:

concat_restore：字符串拼接 indy 的包装 BSM 还原（JDK 标准单形）
================================================================================

Grunteon v2/v3 把 ``makeConcatWithConstants`` 的 recipe 拆成两半：indy 站点
的 bootstrap 静态参数只留 tag 串（\ ``U+0001``\ =实参位、\ ``U+0002``\ =常量
位），常量段搬进 BSM 体里的 ``Object[]``，BSM 本体是一个纯转发包装：

.. code-block:: java

    CallSite <wrapper>(Lookup l, String name, MethodType t, String tags) {
        return StringConcatFactory.makeConcatWithConstants(
            l, name, t, tags, new Object[]{". Status=", ", URL=[", "]"});
    }

CFR 0.152 对这个形态析不出 ``+`` 号（站点 recipe 是纯 tag 串，常量藏在方
法体里，跨 CP 两半拼不回来），restored 面 ``Exception``/残差全部堆积。
本 pass 把两半拼回 JDK 标准单形。


判据
----

结构自证三件套全过才改（任一不符 fail-closed 计数不动刀）：

1. BSM 句柄 → 本类 private static 包装方法；
2. 包装体逐字节形状：\ ``aload×4`` → ``[iconst_N; anewarray Object;
   N×(dup; iconst_i; ldc String; aastore)]`` → ``invokestatic
   StringConcatFactory.makeConcatWithConstants`` 五参形 → ``areturn``；
3. recipe 只含 tag 字节，且 ``U+0002`` 个数 == 常量段个数。

无 Era 门——形状自证即判据。


改写
----

把 ``U+0002`` 位用 BSM 体常量按序回填成完整 recipe，句柄改指 JDK 标准
``StringConcatFactory.makeConcatWithConstants(Lookup,String,MethodType,
String)CallSite``，静态参数表换成 ``[完整 recipe]``。站点零改动（站点
只引 BSM 序号）。语义等价依据：\ ``makeConcatWithConstants`` 对
``U+0002`` 的解析就是按序吃 varargs，包装转发与标准形产出同一 CallSite。

包装方法还原后零引用，DeadMember 引用普查收走；体里的常量串由压池面
带走。


验证
----

jsoup v3_full 实测（build13 主线 trace）：扫描 24084 条 BSM 面出 49 个
包装、shape-bad/recipe 拒证 0；改写 **28 类 49 条全过、failed=0、
residual=0**\ （ProofResult 要求单 jar 残留包装数为 0）。第二轮起
wrappers=0（不动点吸收）。v2 同族转发形同一判据覆盖。

unittest 3 测试：包装形状全自证改写、形状不符 fail-closed、DeadMember
接力（包装方法收走）。
