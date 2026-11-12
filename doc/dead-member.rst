.. _dead-member:

dead_member：零引用成员与空 clinit 删除
==========================================================



形态
----


内联与折叠完成后大量成员只剩空壳：纯算术辅助方法（调用点已折成常量）、 ``/* bridge */ /* synthetic */`` 转发器（调用点已内联）、private 辅助字段、拆空后的 ``<clinit>`` 。


判据
----


形状候选（scan，S2 结构事实）：

- 方法： ``static`` ∧ 方法体纯算术形态（常量/栈/算术/分支/return 词汇表，LDC 单查数值类别），或 ``ACC_SYNTHETIC/ACC_BRIDGE`` ，或 ``private`` ；main /native / abstract / 无码跳过；
- 字段： ``private`` （public/包级零引用可能是按名反射面，宁留勿删），或 ``static [I`` 零访问孤儿数组（seed_strip 拆完建表后的形状，独立强证明）；serialVersionUID / 带 ConstantValue 的不删；
- ``<clinit>`` ：方法体只剩 NOP/RETURN。

引用检查（rewrite 复核）： **先全容器 CP 压缩 ** ，压缩闭包保证池里枚举即活引用。普查按 ** 全容器对外引用全集** 口径：压缩成功的类枚举压缩后的池；压缩失败 / 会话打不开的类照常入普查（原始字节骨架解析后枚举原池，未压缩池含死条目即超集，宁误留不误删），只是不作删除候选；连骨架都解不开的类置census-blind，本轮成员删除与空壳回收整体放弃。普查全部 Fieldref/Methodref/接口引用得活引用集，候选必须在集合外。

反射是理论逃逸面（按名反射方法拦不住）；统计如实记录每一次删除。


改写
----


全容器压缩 → 活引用普查（挂起时间片跑全量，census-blind 则整轮放弃）→逐类删成员（名字+描述+backing 序三重定位复核，倒序 erase）→ 落盘 → 空壳类回收 → 再压缩一次带走死描述符。


统计
----


``methods / fields / clinits / compacted / rejectedByRef / censusRawSources /censusBlind`` 分项计数。死类删除（v3 classStage 的零引用类不动点）遗留：mjbase ResourceContainer 尚无 removeResource 接口。
