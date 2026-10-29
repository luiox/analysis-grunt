# const_method.impl

判据等级：S2 结构事实（两指令形状 + TYPE 表 + clinit 纯度白名单）。

## Scan

两趟：

1. **定义侧**（buildDefs）：逐类开只读会话，对每个 static 方法 promote 后
   要求 `dom.order()` 恰两条指令：
   - 生产者解码（decodeProducer）：iconst*/bipush/sipush/ldc 整数 → I32；
     aconst_null → Null；getstatic → 查 `knowledge::primitiveTypeDesc`，
     必须是九大原始包装类的 `TYPE` 字段（desc `Ljava/lang/Class;`）→
     PrimClass，其余 getstatic 记 `GetstaticUnsupported`；非常量生产者
     **静默跳过**（不进拒绝面，否则全 jar 的普通方法刷屏）。
   - 返回指令与生产值类别必须匹配（I32↔ireturn，Null/PrimClass↔areturn），
     错配记 `BodyNotSingleConst`。
   - `synchronized` 记 `TargetSynchronized`。
   - **所有者 clinit 纯度门**（clinitPure）：`<clinit>` 缺失，或全部指令落在
     纯常量白名单（常量装载限数值类池常量、数值运算/转换/比较、对**本类**
     静态字段的 putstatic、return）且**无任何标签**（直线；有分支/循环即不
     纯——循环不终止是副作用）。跨类 putstatic 判不纯：写别人的字段会在
     初始化链上触发别的类，跳过初始化可能让字段池永远没人写。
   - 通过者进定义表 `owner\nname\ndesc -> ConstValue`。

2. **调用侧**：逐方法逐条 invokestatic，查定义表命中即站点（记 callSrcPc，
   跨会话定位）。

已知边界：TYPE 取值器所在类若用「clinit 跨类写字段池」的结构（Grunt 家族
常态），纯度门整方法拒绝（记 `OwnerClassInitImpure`）——解锁依赖字段池残留
（getstatic 传播）先跑：读取点折成常量后，跨类写入成为死代码，门才有放宽的
语义依据。拒绝数即这个边界在这个样本上的覆盖量。

## Rewrite

按调用类分组。前置 `proof_tools::roundTripSafe`（往返自检，排除本来就
lower 不了的方法）。站点改写是**原地替换**：按 callSrcPc 定位 invokestatic，
`memberOf` 校验目标一致后，整条指令换成常量装载（I32 走
`rewrite_tools::makeConstLoad`；Null 落 aconst_null；PrimClass 在调用方池内
现 intern `addUtf8(prim)+addClass` 再 `CodeEditor::loadConst`），srcPc 保留。
没有删除、没有区间——不需要 constant_call 的标签门。方法 lower 失败整类
放弃；写回同 constant_call 的 pending 批量形态。

## Proof

类数不增 + 脏类逐个可重新解析（同仓口径）。逐类加载校验由语料回归覆盖。
